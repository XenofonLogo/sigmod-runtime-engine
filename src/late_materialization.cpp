#include "late_materialization.h"
#include "columnar.h"
#include <hardware.h>
#include <plan.h>
#include <table.h>
#include <iostream>
#include <fstream>

namespace Contest {

extern Table& GetTable(size_t table_id);

// forward-declare debug dump
void dump_columnar_debug(const ColumnarTable& table);

// Helper: Check if INT32 column has any nulls - OPTIMIZED VERSION
// Helper: Check if INT32 column has any nulls - CORRECT FULL VERSION
static bool column_has_nulls(const Column& column) {

    // μόνο INT32 μας ενδιαφέρει για zero-copy
    if (column.type != DataType::INT32)
        return true;

    // άδειος -> σίγουρα δεν έχει nulls
    if (column.pages.empty())
        return false;

    // έλεγξε ΟΛΕΣ τις σελίδες
    for (const auto& page_ptr : column.pages) {

        const uint8_t* page =
            reinterpret_cast<const uint8_t*>(page_ptr->data);

        uint16_t num_rows =
            *reinterpret_cast<const uint16_t*>(page);

        // bytes bitmap
        size_t bitmap_bytes = (num_rows + 7) / 8;

        // bitmap βρίσκεται ΠΑΝΤΑ στο τέλος της σελίδας
        const uint8_t* bitmap =
            page + PAGE_SIZE - bitmap_bytes;

        // έλεγχος κάθε byte
        for (size_t i = 0; i < bitmap_bytes; ++i) {

            // πλήρες byte (8 rows)
            uint8_t expected = 0xFF;

            // τελευταίο byte -> μάσκα μόνο valid bits
            if (i == bitmap_bytes - 1 && (num_rows % 8 != 0)) {
                expected = (1u << (num_rows % 8)) - 1;
            }

            // αν ΔΕΝ είναι όλα 1 → υπάρχει NULL
            if ((bitmap[i] & expected) != expected)
                return true;
        }
    }

    // δεν βρέθηκαν NULLs σε καμία page
    return false;
}

// ----------------------------------------------------------------------------
// SCAN
// ----------------------------------------------------------------------------
ColumnBuffer scan_columnar_to_columnbuffer(
    const Plan& plan,
    const ScanNode& scan,
    const std::vector<std::tuple<size_t, DataType>>& output_attrs)
{
    auto table_id = scan.base_table_id;
    const auto& input_columnar = plan.inputs[table_id];

    ColumnBuffer buf(output_attrs.size(), input_columnar.num_rows);
    buf.types.reserve(output_attrs.size());
    for (auto& t : output_attrs) buf.types.push_back(std::get<1>(t));

    for (size_t col_idx = 0; col_idx < output_attrs.size(); ++col_idx) {
        size_t in_col_idx = std::get<0>(output_attrs[col_idx]);
        const auto& column = input_columnar.columns[in_col_idx];
        auto& out_col = buf.columns[col_idx];

        // ============================================================
        // ZERO-COPY PATH for INT32 without nulls
        // ============================================================
        if (column.type == DataType::INT32 && !column_has_nulls(column)) {
            out_col.is_zero_copy = true;
            out_col.src_column = &column;
            out_col.num_values = input_columnar.num_rows;
            
            // Build page offsets for fast lookup
            out_col.page_offsets.push_back(0);
            size_t cumulative = 0;
            for (const auto& page_ptr : column.pages) {
                auto* page = page_ptr->data;
                uint16_t num_rows = *reinterpret_cast<const uint16_t*>(page);
                cumulative += num_rows;
                out_col.page_offsets.push_back(cumulative);
            }
            
            continue; // Skip materialization
        }

        // ============================================================
        // FALLBACK materialization
        // ============================================================
        for (size_t page_idx = 0; page_idx < column.pages.size(); ++page_idx) {
            auto* page = column.pages[page_idx]->data;
            uint16_t num_rows_in_page =
                *reinterpret_cast<const uint16_t*>(page);

            switch (column.type) {

                case DataType::INT32: {
                    auto* data_begin =
                        reinterpret_cast<const int32_t*>(page + 4);
                    auto* bitmap = reinterpret_cast<const uint8_t*>(
                        page + PAGE_SIZE - (num_rows_in_page + 7) / 8);

                    uint16_t data_idx = 0;
                    for (uint16_t i = 0; i < num_rows_in_page; ++i) {
                        if (get_bitmap_local_col(bitmap, i)) {
                            out_col.append(
                                value_t::make_i32(data_begin[data_idx++]));
                        } else {
                            out_col.append(value_t::make_null());
                        }
                    }
                    break;
                }

                case DataType::VARCHAR: {
                    if (num_rows_in_page == 0xffff) {
                        out_col.append(value_t::make_str_ref(
                            (uint8_t)table_id,
                            (uint8_t)in_col_idx,
                            (uint32_t)page_idx,
                            0xffff));
                    }
                    else if (num_rows_in_page == 0xfffe) {
                        // continuation page → skip
                    }
                    else {
                        auto* bitmap = reinterpret_cast<const uint8_t*>(
                            page + PAGE_SIZE - (num_rows_in_page + 7) / 8);

                        uint16_t data_idx = 0;
                        for (uint16_t i = 0; i < num_rows_in_page; ++i) {
                            if (get_bitmap_local_col(bitmap, i)) {
                                out_col.append(value_t::make_str_ref(
                                    (uint8_t)table_id,
                                    (uint8_t)in_col_idx,
                                    (uint32_t)page_idx,
                                    data_idx++));
                            } else {
                                out_col.append(value_t::make_null());
                            }
                        }
                    }
                    break;
                }

                default:
                    break;
            }
        }
    }

    return buf;
}

// ----------------------------------------------------------------------------
// RESOLVER
// ----------------------------------------------------------------------------
std::pair<const char*, size_t>
StringRefResolver::resolve(uint64_t raw_ref, std::string& buffer) {
    PackedStringRef ref = PackedStringRef::unpack(raw_ref);

    if (ref.parts.table_id >= plan->inputs.size()) return {nullptr, 0};
    const ColumnarTable& ct = plan->inputs[ref.parts.table_id];

    if (ref.parts.col_id >= ct.columns.size()) return {nullptr, 0};
    const auto& column = ct.columns[ref.parts.col_id];

    if (ref.parts.page_idx >= column.pages.size()) return {nullptr, 0};
    auto* page_data = column.pages[ref.parts.page_idx]->data;

    uint16_t num_rows = *reinterpret_cast<const uint16_t*>(page_data);

    if (num_rows == 0xffff || num_rows == 0xfffe) {
        buffer.clear();
        size_t cur_page = ref.parts.page_idx;
        while (cur_page < column.pages.size()) {
            auto* p = column.pages[cur_page]->data;
            uint16_t nr = *reinterpret_cast<const uint16_t*>(p);
            if (nr != 0xffff && nr != 0xfffe) break;
            uint16_t chunk_len = *reinterpret_cast<const uint16_t*>(p + 2);
            buffer.append(reinterpret_cast<const char*>(p + 4), chunk_len);
            ++cur_page;
        }
        return {buffer.data(), buffer.size()};
    }

    uint16_t num_offsets =
        *reinterpret_cast<const uint16_t*>(page_data + 2);
    if (ref.parts.slot_idx >= num_offsets) return {nullptr, 0};

    auto* offsets = reinterpret_cast<const uint16_t*>(page_data + 4);
    size_t offsets_end = 4 + num_offsets * 2;

    uint16_t start =
        (ref.parts.slot_idx == 0) ? 0 : offsets[ref.parts.slot_idx - 1];
    uint16_t end = offsets[ref.parts.slot_idx];

    buffer.assign(
        reinterpret_cast<const char*>(page_data + offsets_end + start),
        end - start);

    return {buffer.data(), buffer.size()};
}

// ----------------------------------------------------------------------------
// FINALIZE
// ----------------------------------------------------------------------------
ColumnarTable finalize_columnbuffer_to_columnar(
    const Plan& plan,
    const ColumnBuffer& buf,
    const std::vector<std::tuple<size_t, DataType>>& output_attrs)
{
    ColumnarTable output;
    output.num_rows = buf.num_rows;
    output.columns.reserve(output_attrs.size());

    StringRefResolver resolver(&plan);
    std::string tmp_buf;

    for (size_t col_idx = 0; col_idx < output_attrs.size(); ++col_idx) {
        DataType dtype = buf.types[col_idx];
        Column col(dtype);

        if (buf.num_rows == 0) {
            output.columns.emplace_back(std::move(col));
            continue;
        }

        if (dtype == DataType::INT32) {
            ColumnInserter<int32_t> inserter(col);
            for (size_t row_idx = 0; row_idx < buf.num_rows; ++row_idx) {
                const auto& v = buf.columns[col_idx].get(row_idx);
                if (!v.is_null()) inserter.insert(v.as_i32());
                else inserter.insert_null();
            }
            inserter.finalize();
        }
        else if (dtype == DataType::VARCHAR) {
            ColumnInserter<std::string> inserter(col);
            for (size_t row_idx = 0; row_idx < buf.num_rows; ++row_idx) {
                const auto& v = buf.columns[col_idx].get(row_idx);

                if (v.is_null()) {
                    inserter.insert_null();
                    continue;
                }

                auto [ptr, len] = resolver.resolve(v.as_ref(), tmp_buf);
                if (ptr) inserter.insert(std::string_view(ptr, len));
                else inserter.insert_null();
            }
            inserter.finalize();
        }

        output.columns.emplace_back(std::move(col));
    }

    dump_columnar_debug(output);
    return output;
}

// ----------------------------------------------------------------------------
// DEBUG DUMP
// ----------------------------------------------------------------------------
void dump_columnar_debug(const ColumnarTable& table) {
    try {
        ::DumpTable dum(const_cast<::ColumnarTable*>(&table));
        std::ofstream bin("/tmp/last_result.tbl", std::ios::binary);
        if (bin) dum.dump(bin);
    } catch (...) {}
}

// ----------------------------------------------------------------------------
// HASH / EQ (needed by joins)
// ----------------------------------------------------------------------------

size_t StringRefHash::operator()(const PackedStringRef& k) const {
    // Fast path: hash directly on raw reference
    return std::hash<uint64_t>{}(k.raw);
}

bool StringRefEq::operator()(const PackedStringRef& a,
                            const PackedStringRef& b) const
{
    // Fast path: identical reference
    if (a.raw == b.raw)
        return true;

    // Slow path: resolve strings only if needed
    if (plan == nullptr)
        return false;

    StringRefResolver resolver(plan);
    std::string sa, sb;

    auto [pa, la] = resolver.resolve(a.raw, sa);
    auto [pb, lb] = resolver.resolve(b.raw, sb);

    if (!pa || !pb || la != lb)
        return false;

    return std::memcmp(pa, pb, la) == 0;
}

} // namespace Contest
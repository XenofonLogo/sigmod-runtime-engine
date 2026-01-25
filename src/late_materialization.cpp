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

// Έλεγχος αν στήλη INT32 περιέχει NULL (zero-copy γίνεται μόνο αν δεν υπάρχουν)
static bool column_has_nulls(const Column& column) {

    // Αν δεν είναι INT32, θεωρούμε ότι δεν μπορεί να είναι zero-copy
    if (column.type != DataType::INT32)
        return true;

    // Αν δεν υπάρχουν σελίδες, δεν υπάρχουν nulls
    if (column.pages.empty())
        return false;

    // Σάρωση όλων των σελίδων για NULL bits
    for (const auto& page_ptr : column.pages) {
        // Raw pointer στη σελίδα
        const uint8_t* page = reinterpret_cast<const uint8_t*>(page_ptr->data);
        uint16_t num_rows = *reinterpret_cast<const uint16_t*>(page);
        size_t bitmap_bytes = (num_rows + 7) / 8;
        const uint8_t* bitmap = page + PAGE_SIZE - bitmap_bytes;
        // Έλεγχος κάθε byte του bitmap
        for (size_t i = 0; i < bitmap_bytes; ++i) {
            uint8_t expected = 0xFF;
            if (i == bitmap_bytes - 1 && (num_rows % 8 != 0)) {
                expected = (1u << (num_rows % 8)) - 1;
            }
            if ((bitmap[i] & expected) != expected)
                return true;
        }
    }
    // Δεν βρέθηκαν nulls σε καμία σελίδα
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
    auto table_id = scan.base_table_id;                         // ID πίνακα εισόδου
    const auto& input_columnar = plan.inputs[table_id];         // Columnar πηγή

    ColumnBuffer buf(output_attrs.size(), input_columnar.num_rows); // Προορισμός
    buf.types.reserve(output_attrs.size());
    for (auto& t : output_attrs) buf.types.push_back(std::get<1>(t));

    for (size_t col_idx = 0; col_idx < output_attrs.size(); ++col_idx) {
        size_t in_col_idx = std::get<0>(output_attrs[col_idx]);       // Πηγή στήλης
        const auto& column = input_columnar.columns[in_col_idx];      // Είσοδος
        auto& out_col = buf.columns[col_idx];                         // Έξοδος

        // Διαδρομή ZERO-COPY για INT32 χωρίς NULLs
        if (column.type == DataType::INT32 && !column_has_nulls(column)) {
            out_col.is_zero_copy = true;
            out_col.src_column = &column;
            out_col.num_values = input_columnar.num_rows;
            
            // Δημιουργία page offsets για γρήγορο lookup
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

        // Fallback materialization για όλους τους άλλους τύπους/περιπτώσεις
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

                    uint16_t data_idx = 0; // Προχωρά μόνο στα valid entries
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
                        // Πρώτη σελίδα long string block (ref σε αρχή)
                        out_col.append(value_t::make_str_ref(
                            (uint8_t)table_id,
                            (uint8_t)in_col_idx,
                            (uint32_t)page_idx,
                            0xffff));
                    }
                    else if (num_rows_in_page == 0xfffe) {
                        // Σελίδα συνέχειας: δεν εκπέμπει refs, απλώς παραλείπεται
                    }
                    else {
                        auto* bitmap = reinterpret_cast<const uint8_t*>(
                            page + PAGE_SIZE - (num_rows_in_page + 7) / 8);

                        uint16_t data_idx = 0; // offset στον πίνακα offsets
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
    PackedStringRef ref = PackedStringRef::unpack(raw_ref); // Αποσυσκευή packed αναφοράς

    if (ref.parts.table_id >= plan->inputs.size()) return {nullptr, 0}; // Εκτός ορίων
    const ColumnarTable& ct = plan->inputs[ref.parts.table_id];

    if (ref.parts.col_id >= ct.columns.size()) return {nullptr, 0};     // Εκτός ορίων
    const auto& column = ct.columns[ref.parts.col_id];

    if (ref.parts.page_idx >= column.pages.size()) return {nullptr, 0}; // Εκτός ορίων
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
        *reinterpret_cast<const uint16_t*>(page_data + 2); // Πλήθος offsets στην πρώτη σελίδα
    if (ref.parts.slot_idx >= num_offsets) return {nullptr, 0};

    auto* offsets = reinterpret_cast<const uint16_t*>(page_data + 4);
    size_t offsets_end = 4 + num_offsets * 2;

    uint16_t start =
        (ref.parts.slot_idx == 0) ? 0 : offsets[ref.parts.slot_idx - 1]; // Αρχή substring
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
    ColumnarTable output;                         // Τελικό columnar αποτέλεσμα
    output.num_rows = buf.num_rows;
    output.columns.reserve(output_attrs.size());

    StringRefResolver resolver(&plan);
    std::string tmp_buf;

    for (size_t col_idx = 0; col_idx < output_attrs.size(); ++col_idx) {
        DataType dtype = buf.types[col_idx];       // Τύπος στήλης
        Column col(dtype);                         // Column εξόδου

        if (buf.num_rows == 0) {
            output.columns.emplace_back(std::move(col)); // Άδειο αποτέλεσμα
            continue;
        }

        if (dtype == DataType::INT32) {
            ColumnInserter<int32_t> inserter(col);              // Εισαγωγέας για INT32
            for (size_t row_idx = 0; row_idx < buf.num_rows; ++row_idx) {
                const auto& v = buf.columns[col_idx].get(row_idx);
                if (!v.is_null()) inserter.insert(v.as_i32());
                else inserter.insert_null();
            }
            inserter.finalize();
        }
        else if (dtype == DataType::VARCHAR) {
            ColumnInserter<std::string> inserter(col);          // Εισαγωγέας για strings
            for (size_t row_idx = 0; row_idx < buf.num_rows; ++row_idx) {
                const auto& v = buf.columns[col_idx].get(row_idx);

                if (v.is_null()) {
                    inserter.insert_null();
                    continue;
                }

                auto [ptr, len] = resolver.resolve(v.as_ref(), tmp_buf); // Resolve packed ref
                if (ptr) inserter.insert(std::string_view(ptr, len));
                else inserter.insert_null();
            }
            inserter.finalize();
        }

        output.columns.emplace_back(std::move(col));
    }

    dump_columnar_debug(output); // Προαιρετικό dump για debugging
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
    return std::hash<uint64_t>{}(k.raw); // Hash πάνω στο raw reference
}

bool StringRefEq::operator()(const PackedStringRef& a,
                            const PackedStringRef& b) const
{
    if (a.raw == b.raw)
        return true; // Ίδια packed αναφορά

    if (plan == nullptr)
        return false; // Χωρίς πλάνο δεν μπορούμε να resolve

    StringRefResolver resolver(plan);
    std::string sa, sb;

    auto [pa, la] = resolver.resolve(a.raw, sa);
    auto [pb, lb] = resolver.resolve(b.raw, sb);

    if (!pa || !pb || la != lb)
        return false; // Μη συγκρίσιμες ή διαφορετικού μήκους

    return std::memcmp(pa, pb, la) == 0; // Τελική σύγκριση bytes
}

} // namespace Contest
#include "late_materialization.h"
#include "columnar.h"

#include <hardware.h>
#include <plan.h>
#include <table.h>

#include <iostream>
#include <fstream>

#include <column_zero_copy.h>

namespace Contest {

// ============================================================================
//  Βοηθητικά / forward declarations
// ============================================================================

// Πρόσβαση σε base tables (χρησιμοποιείται για VARCHAR resolve)
extern Table& GetTable(size_t table_id);

// Debug dump του τελικού columnar αποτελέσματος
void dump_columnar_debug(const ColumnarTable& table);

// ============================================================================
//  Helper: Έλεγχος αν INT32 column περιέχει NULLs
// ============================================================================
//
//  Σύμφωνα με την εκφώνηση:
//  - Zero-copy επιτρέπεται ΜΟΝΟ για INT32
//  - ΚΑΙ ΜΟΝΟ αν ΔΕΝ υπάρχει ΚΑΝΕΝΑ NULL
//
//  Η πληροφορία για NULLs βρίσκεται στο bitmap κάθε page.
//  Αν δεν υπάρχει bitmap (επικάλυψη με data), τότε η page
//  θεωρείται πλήρως NON-NULL.
//
static bool column_has_nulls(const Column& column) {

    // Μόνο INT32 μπορεί να είναι zero-copy
    if (column.type != DataType::INT32)
        return true;

    // Ελέγχουμε ΟΛΕΣ τις pages
    for (const auto& page_ptr : column.pages) {

        // Η Page αποθηκεύεται ως std::byte[]
        const uint8_t* page =
            reinterpret_cast<const uint8_t*>(page_ptr->data);

        // Header: bytes [0..1] = αριθμός rows
        uint16_t num_rows =
            *reinterpret_cast<const uint16_t*>(page);

        // Υπολογισμός μεγέθους bitmap
        size_t bitmap_bytes = (num_rows + 7) / 8;

        // Bitmap τοποθετείται στο τέλος της page
        const uint8_t* bitmap =
            page + PAGE_SIZE - bitmap_bytes;

        // Τέλος περιοχής δεδομένων INT32
        const uint8_t* data_end =
            page + 4 + num_rows * sizeof(int32_t);

        // Αν το bitmap "μπαίνει" μέσα στα δεδομένα,
        // σημαίνει ότι ΔΕΝ υπάρχει bitmap ⇒ ΚΑΝΕΝΑ NULL
        if (bitmap < data_end) {
            continue;
        }

        // Bitmap υπάρχει ⇒ πρέπει να ελεγχθεί
        for (size_t i = 0; i < bitmap_bytes; ++i) {
            uint8_t expected = 0xFF;

            // Τελευταίο byte: πιθανώς όχι πλήρες
            if (i == bitmap_bytes - 1 && (num_rows % 8 != 0)) {
                expected = (1u << (num_rows % 8)) - 1;
            }

            // Αν κάποιο bit είναι 0 ⇒ NULL
            if ((bitmap[i] & expected) != expected) {
                return true;
            }
        }
    }

    // Δεν βρέθηκε ΚΑΝΕΝΑ NULL
    return false;
}

// ============================================================================
//  SCAN: ColumnarTable -> ColumnBuffer
// ============================================================================
//
//  Εδώ υλοποιείται:
//   - ZERO-COPY για INT32 χωρίς NULLs
//   - MATERIALIZATION για όλες τις άλλες περιπτώσεις
//
ColumnBuffer scan_columnar_to_columnbuffer(
    const Plan& plan,
    const ScanNode& scan,
    const std::vector<std::tuple<size_t, DataType>>& output_attrs)
{
    // ------------------------------------------------------------
    // Εντοπισμός input πίνακα
    // ------------------------------------------------------------
    size_t table_id = scan.base_table_id;
    const ColumnarTable& input_columnar = plan.inputs[table_id];

    // Δημιουργία ColumnBuffer:
    // - τόσες στήλες όσα τα output attributes
    // - αρχικό num_rows = input rows
    ColumnBuffer buf(output_attrs.size(), input_columnar.num_rows);

    // Καταγραφή τύπων στηλών
    buf.types.reserve(output_attrs.size());
    for (const auto& t : output_attrs) {
        buf.types.push_back(std::get<1>(t));
    }

    // ------------------------------------------------------------
    // Επεξεργασία κάθε output στήλης
    // ------------------------------------------------------------
    for (size_t out_col_idx = 0; out_col_idx < output_attrs.size(); ++out_col_idx) {

        size_t in_col_idx = std::get<0>(output_attrs[out_col_idx]);

        const Column& input_col =
            input_columnar.columns[in_col_idx];

        column_t& out_col =
            buf.columns[out_col_idx];

        // ============================================================
        // ZERO-COPY PATH
        // ============================================================
        //
        // Επιτρέπεται ΑΝ ΚΑΙ ΜΟΝΟ ΑΝ:
        //  - INT32
        //  - ΧΩΡΙΣ ΚΑΝΕΝΑ NULL
        //
        if (input_col.type == DataType::INT32 &&
            !column_has_nulls(input_col)) {

            // Ενεργοποίηση zero-copy mode
            out_col.is_zero_copy = true;

            // Δείκτης στην αρχική column (όχι αντιγραφή)
            out_col.src_column = &input_col;

            // page_offsets[i] = συνολικά rows πριν την page i
            out_col.page_offsets.clear();
            out_col.page_offsets.push_back(0);

            size_t cumulative_rows = 0;

            for (Page* page_ptr : input_col.pages) {
                const uint8_t* page =
                    reinterpret_cast<const uint8_t*>(page_ptr->data);

                uint16_t rows_in_page =
                    *reinterpret_cast<const uint16_t*>(page);

                cumulative_rows += rows_in_page;
                out_col.page_offsets.push_back(cumulative_rows);
            }

            // Συνολικός αριθμός τιμών
            out_col.num_values = cumulative_rows;
            buf.num_rows = cumulative_rows;

            // ΣΗΜΑΝΤΙΚΟ:
            // Δεν κάνουμε materialization
            continue;
        }

        // ============================================================
        // FALLBACK: MATERIALIZATION
        // ============================================================
        //
        // Χρησιμοποιείται για:
        //  - VARCHAR
        //  - INT32 με NULLs
        //
        for (size_t page_idx = 0;
             page_idx < input_col.pages.size();
             ++page_idx) {

            const uint8_t* page =
                reinterpret_cast<const uint8_t*>(
                    input_col.pages[page_idx]->data);

            uint16_t num_rows_in_page =
                *reinterpret_cast<const uint16_t*>(page);

            switch (input_col.type) {

                // -----------------------------
                // INT32 (με bitmap)
                // -----------------------------
                case DataType::INT32: {

                    const int32_t* data_begin =
                        reinterpret_cast<const int32_t*>(page + 4);

                    const uint8_t* bitmap =
                        page + PAGE_SIZE - (num_rows_in_page + 7) / 8;

                    uint16_t data_idx = 0;

                    for (uint16_t i = 0; i < num_rows_in_page; ++i) {
                        if (get_bitmap_local_col(bitmap, i)) {
                            out_col.append(
                                value_t::make_i32(
                                    data_begin[data_idx++]));
                        } else {
                            out_col.append(value_t::make_null());
                        }
                    }
                    break;
                }

                // -----------------------------
                // VARCHAR (string references)
                // -----------------------------
                case DataType::VARCHAR: {

                    // Long string start page
                    if (num_rows_in_page == 0xffff) {
                        out_col.append(
                            value_t::make_str_ref(
                                (uint8_t)table_id,
                                (uint8_t)in_col_idx,
                                (uint32_t)page_idx,
                                0xffff));
                    }
                    // Continuation page → skip
                    else if (num_rows_in_page == 0xfffe) {
                        // nothing
                    }
                    // Normal VARCHAR page
                    else {
                        const uint8_t* bitmap =
                            page + PAGE_SIZE - (num_rows_in_page + 7) / 8;

                        uint16_t data_idx = 0;

                        for (uint16_t i = 0; i < num_rows_in_page; ++i) {
                            if (get_bitmap_local_col(bitmap, i)) {
                                out_col.append(
                                    value_t::make_str_ref(
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
    if (plan == nullptr) return std::hash<uint64_t>{}(k.raw);
    StringRefResolver resolver(plan);
    std::string tmp;
    auto [ptr, len] = resolver.resolve(k.raw, tmp);
    if (!ptr) return std::hash<uint64_t>{}(k.raw);
    return std::hash<std::string_view>{}(std::string_view(ptr, len));
}

bool StringRefEq::operator()(
    const PackedStringRef& a,
    const PackedStringRef& b) const
{
    if (a.raw == b.raw) return true;
    if (plan == nullptr) return false;

    StringRefResolver resolver(plan);
    std::string ta, tb;
    auto [pa, la] = resolver.resolve(a.raw, ta);
    auto [pb, lb] = resolver.resolve(b.raw, tb);
    if (!pa || !pb || la != lb) return false;
    return std::string_view(pa, la) == std::string_view(pb, lb);
}

} // namespace Contest
#include "late_materialization.h"
#include "columnar.h" 
#include <hardware.h>
#include <plan.h>
#include <table.h>
#include <iostream>
#include <fstream>

namespace Contest {

extern Table& GetTable(size_t table_id); 

// forward-declare debug dump (defined later)
void dump_columnar_debug(const ColumnarTable& table);

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
        auto& column = input_columnar.columns[in_col_idx];
        auto& out_col = buf.columns[col_idx];

        for (size_t page_idx = 0; page_idx < column.pages.size(); ++page_idx) {
            auto* page = column.pages[page_idx]->data;
            // Τα δεδομένα στη σελίδα (plan.h PAGE definition)
            uint16_t num_rows_in_page = *reinterpret_cast<const uint16_t*>(page);
            
            switch (column.type) {
                case DataType::INT32: {
                    auto* data_begin = reinterpret_cast<const int32_t*>(page + 4);
                    auto* bitmap = reinterpret_cast<const uint8_t*>(page + PAGE_SIZE - (num_rows_in_page + 7) / 8);
                    uint16_t data_idx = 0;
                    for (uint16_t i = 0; i < num_rows_in_page; ++i) {
                        if (get_bitmap_local_col(bitmap, i)) {
                            out_col.append(value_t::make_i32(data_begin[data_idx++]));
                        } else {
                            out_col.append(value_t::make_null());
                        }
                    }
                    break;
                }
                case DataType::INT64: {
                    auto* data_begin = reinterpret_cast<const int64_t*>(page + 8);
                    auto* bitmap = reinterpret_cast<const uint8_t*>(page + PAGE_SIZE - (num_rows_in_page + 7) / 8);
                    uint16_t data_idx = 0;
                    for (uint16_t i = 0; i < num_rows_in_page; ++i) {
                        if (get_bitmap_local_col(bitmap, i)) {
                            out_col.append(value_t::make_i64(data_begin[data_idx++]));
                        } else {
                            out_col.append(value_t::make_null());
                        }
                    }
                    break;
                }
                case DataType::FP64: {
                    auto* data_begin = reinterpret_cast<const double*>(page + 8);
                    auto* bitmap = reinterpret_cast<const uint8_t*>(page + PAGE_SIZE - (num_rows_in_page + 7) / 8);
                    uint16_t data_idx = 0;
                    for (uint16_t i = 0; i < num_rows_in_page; ++i) {
                        if (get_bitmap_local_col(bitmap, i)) {
                            out_col.append(value_t::make_f64(data_begin[data_idx++]));
                        } else {
                            out_col.append(value_t::make_null());
                        }
                    }
                    break;
                }
                case DataType::VARCHAR: {
                    if (num_rows_in_page == 0xffff) {
                        out_col.append(value_t::make_str_ref((uint8_t)table_id, static_cast<uint8_t>(in_col_idx), static_cast<uint32_t>(page_idx), 0xffff));
                        // first page of a long string: single row reference
                    } else if (num_rows_in_page == 0xfffe) {
                        // continuation page for a long string: no row starts here
                        // skip appending anything for continuation pages
                    } else {
                        auto* bitmap = reinterpret_cast<const uint8_t*>(page + PAGE_SIZE - (num_rows_in_page + 7) / 8);
                        uint16_t data_idx = 0;
                        for (uint16_t i = 0; i < num_rows_in_page; ++i) {
                            if (get_bitmap_local_col(bitmap, i)) {
                                out_col.append(value_t::make_str_ref(
                                    (uint8_t)table_id, 
                                    static_cast<uint8_t>(in_col_idx), 
                                    static_cast<uint32_t>(page_idx), 
                                    data_idx
                                ));
                                ++data_idx;
                            } else {
                                out_col.append(value_t::make_null());
                            }
                        }
                    }
                    break;
                }
                default: break;
            }
        }
    }
    return buf;
}

// ----------------------------------------------------------------------------
// RESOLVER
// ----------------------------------------------------------------------------
std::pair<const char*, size_t> StringRefResolver::resolve(uint64_t raw_ref, std::string& buffer) {
    PackedStringRef ref = PackedStringRef::unpack(raw_ref);
    
    if (ref.parts.table_id >= plan->inputs.size()) return {nullptr, 0};
    const ColumnarTable& ct = plan->inputs[ref.parts.table_id];
    
    if (ref.parts.col_id >= ct.columns.size()) return {nullptr, 0};
    const auto& column = ct.columns[ref.parts.col_id];
    
    if (ref.parts.page_idx >= column.pages.size()) return {nullptr, 0};
    auto* page_data = column.pages[ref.parts.page_idx]->data;
    
    uint16_t num_rows = *reinterpret_cast<const uint16_t*>(page_data);

    if (num_rows == 0xffff || num_rows == 0xfffe) {
        // Long string stored across one or more pages. Concatenate all
        // successive long-string pages starting at page_idx.
        buffer.clear();
        size_t cur_page = ref.parts.page_idx;
        while (cur_page < column.pages.size()) {
            auto* p = column.pages[cur_page]->data;
            uint16_t nr = *reinterpret_cast<const uint16_t*>(p);
            if (nr != 0xffff && nr != 0xfffe) break;
            uint16_t chunk_len = *reinterpret_cast<const uint16_t*>(p + 2);
            const char* chunk_ptr = reinterpret_cast<const char*>(p + 4);
            buffer.append(chunk_ptr, chunk_len);
            ++cur_page;
        }
        return {buffer.data(), buffer.size()};
    }
    
    uint16_t num_offsets = *reinterpret_cast<const uint16_t*>(page_data + 2); 
    if (ref.parts.slot_idx >= num_offsets) return {nullptr, 0}; 

    auto* offsets = reinterpret_cast<const uint16_t*>(page_data + 4);
    size_t offsets_end = 4 + num_offsets * 2; 
    
    uint16_t start_offset = (ref.parts.slot_idx == 0) ? 0 : offsets[ref.parts.slot_idx - 1];
    uint16_t end_offset = offsets[ref.parts.slot_idx]; 
    size_t len = end_offset - start_offset;
    
    const char* str_ptr = reinterpret_cast<const char*>(page_data + offsets_end + start_offset);
    buffer.assign(str_ptr, len);

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
        else if (dtype == DataType::INT64) {
            ColumnInserter<int64_t> inserter(col);
            for (size_t row_idx = 0; row_idx < buf.num_rows; ++row_idx) {
                const auto& v = buf.columns[col_idx].get(row_idx);
                if (!v.is_null()) inserter.insert(v.as_i64());
                else inserter.insert_null();
            }
            inserter.finalize();
        }
        else if (dtype == DataType::FP64) {
            ColumnInserter<double> inserter(col);
            for (size_t row_idx = 0; row_idx < buf.num_rows; ++row_idx) {
                const auto& v = buf.columns[col_idx].get(row_idx);
                if (!v.is_null()) inserter.insert(v.as_f64());
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

                // Try fast-path: interpret the packed string ref and point
                // directly into the source page for short strings. Only use
                // the resolver (which may allocate) for long/continued pages.
                const uint64_t raw = v.as_ref();
                PackedStringRef pref = PackedStringRef::unpack(raw);
                const char* ptr = nullptr;
                size_t len = 0;

                if (pref.parts.table_id < plan.inputs.size()) {
                    const ColumnarTable& src_ct = plan.inputs[pref.parts.table_id];
                    if (pref.parts.col_id < src_ct.columns.size()) {
                        const auto& src_col = src_ct.columns[pref.parts.col_id];
                        if (pref.parts.page_idx < src_col.pages.size()) {
                            auto* page_data = src_col.pages[pref.parts.page_idx]->data;
                            uint16_t num_rows_in_page = *reinterpret_cast<const uint16_t*>(page_data);
                            // Short-string page
                            if (num_rows_in_page != 0xffff && num_rows_in_page != 0xfffe
                                && pref.parts.slot_idx < *reinterpret_cast<const uint16_t*>(page_data + 2)) {
                                uint16_t num_offsets = *reinterpret_cast<const uint16_t*>(page_data + 2);
                                auto* offsets = reinterpret_cast<const uint16_t*>(page_data + 4);
                                size_t offsets_end = 4 + num_offsets * 2;
                                uint16_t start_offset = (pref.parts.slot_idx == 0) ? 0 : offsets[pref.parts.slot_idx - 1];
                                uint16_t end_offset = offsets[pref.parts.slot_idx];
                                len = static_cast<size_t>(end_offset - start_offset);
                                ptr = reinterpret_cast<const char*>(page_data + offsets_end + start_offset);
                            }
                        }
                    }
                }

                if (ptr != nullptr) {
                    inserter.insert(std::string_view(ptr, len));
                } else {
                    // Fallback: use resolver for long strings or when fast-path fails
                    auto [rptr, rlen] = resolver.resolve(v.as_ref(), tmp_buf);
                    if (rptr != nullptr) inserter.insert(std::string_view(rptr, rlen));
                    else inserter.insert_null();
                }
            }
            inserter.finalize();
        }

        output.columns.emplace_back(std::move(col));
    }

    // Debug: dump last result for inspection
    dump_columnar_debug(output);
    return output;
}

} // namespace Contest

// Debug dump: write last finalized ColumnarTable to /tmp for inspection
namespace Contest {
    void dump_columnar_debug(const ColumnarTable& table) {
        try {
            ::DumpTable dum(const_cast<::ColumnarTable*>(&table));
            std::ofstream bin("/tmp/last_result.tbl", std::ios::binary);
            if (bin) dum.dump(bin);
            // also write a textual CSV-like file
            auto tab = ::Table::from_columnar(table);
            std::ofstream txt("/tmp/last_result.txt");
            if (txt) {
                namespace views = ranges::views;
                auto escape_string = [](const std::string& s) {
                    std::string escaped;
                    for (char c: s) {
                        switch (c) {
                        case '"':  escaped += "\\\""; break;
                        case '\\': escaped += "\\\\"; break;
                        case '\n': escaped += "\\n"; break;
                        case '\r': escaped += "\\r"; break;
                        case '\t': escaped += "\\t"; break;
                        default:   escaped += c; break;
                        }
                    }
                    return escaped;
                };
                for (auto& record: tab.table()) {
                    std::string line;
                    for (size_t i=0;i<record.size();++i) {
                        const auto& field = record[i];
                        std::string cell;
                        std::visit([&](const auto& arg){ using T = std::decay_t<decltype(arg)>;
                            if constexpr (std::is_same_v<T,std::monostate>) cell = "NULL";
                            else if constexpr (std::is_same_v<T,int32_t> || std::is_same_v<T,int64_t> || std::is_same_v<T,double>) cell = fmt::format("{}", arg);
                            else if constexpr (std::is_same_v<T,std::string>) cell = fmt::format("\"{}\"", escape_string(arg));
                        }, field);
                        if (i) line += '|';
                        line += cell;
                    }
                    txt << line << '\n';
                }
            }
        } catch (...) {}
    }
}

// Definitions for StringRefHash/StringRefEq that need the resolver implementation
namespace Contest {
    size_t StringRefHash::operator()(const PackedStringRef& k) const {
        if (plan == nullptr) return std::hash<uint64_t>{}(k.raw);
        StringRefResolver resolver(plan);
        std::string tmp;
        auto [ptr, len] = resolver.resolve(k.raw, tmp);
        if (!ptr) return std::hash<uint64_t>{}(k.raw);
        return std::hash<std::string_view>{}(std::string_view(ptr, len));
    }

    bool StringRefEq::operator()(const PackedStringRef& a, const PackedStringRef& b) const {
        if (a.raw == b.raw) return true;
        if (plan == nullptr) return false;
        StringRefResolver resolver(plan);
        std::string ta, tb;
        auto [pa, la] = resolver.resolve(a.raw, ta);
        auto [pb, lb] = resolver.resolve(b.raw, tb);
        if (!pa || !pb) return false;
        if (la != lb) return false;
        return std::string_view(pa, la) == std::string_view(pb, lb);
    }
}
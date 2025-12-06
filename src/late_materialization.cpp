#include "late_materialization.h"
#include "columnar.h" 
#include <hardware.h>
#include <plan.h>
#include <table.h>
#include <iostream>

namespace Contest {

extern Table& GetTable(size_t table_id); 

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
                    } else if (num_rows_in_page == 0xfffe) {
                        out_col.append(value_t::make_null());
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
        buffer = "LONG_STR_PLACEHOLDER";
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
                if (v.type == value_t::Type::I32) inserter.insert(v.u.i32);
                else inserter.insert_null();
            }
            inserter.finalize();
        } 
        else if (dtype == DataType::INT64) {
            ColumnInserter<int64_t> inserter(col);
            for (size_t row_idx = 0; row_idx < buf.num_rows; ++row_idx) {
                const auto& v = buf.columns[col_idx].get(row_idx);
                if (v.type == value_t::Type::I64) inserter.insert(v.u.i64);
                else inserter.insert_null();
            }
            inserter.finalize();
        }
        else if (dtype == DataType::FP64) {
            ColumnInserter<double> inserter(col);
            for (size_t row_idx = 0; row_idx < buf.num_rows; ++row_idx) {
                const auto& v = buf.columns[col_idx].get(row_idx);
                if (v.type == value_t::Type::FP64) inserter.insert(v.u.f64);
                else inserter.insert_null();
            }
            inserter.finalize();
        }
        else if (dtype == DataType::VARCHAR) {
            ColumnInserter<std::string> inserter(col);
            for (size_t row_idx = 0; row_idx < buf.num_rows; ++row_idx) {
                const auto& v = buf.columns[col_idx].get(row_idx);
                
                if (v.type == value_t::Type::STR_REF) {
                    auto [ptr, len] = resolver.resolve(v.u.ref, tmp_buf);
                    if (ptr != nullptr) inserter.insert(std::string_view(ptr, len));
                    else inserter.insert_null();
                } else {
                    inserter.insert_null();
                }
            }
            inserter.finalize();
        }

        output.columns.emplace_back(std::move(col));
    }

    return output;
}

} // namespace Contest
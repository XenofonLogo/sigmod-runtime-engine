#include "columnar.h"

#include <cstring>
#include <algorithm>
#include <unordered_map>

namespace Contest {

static inline bool get_bitmap_local_col(const uint8_t* bitmap, uint16_t idx) {
    auto byte_idx = idx / 8;
    auto bit = idx % 8;
    return bitmap[byte_idx] & (1u << bit);
}

ColumnBuffer scan_columnar_to_columnbuffer(const Plan& plan,
    const ScanNode& scan,
    const std::vector<std::tuple<size_t, DataType>>& output_attrs) {
    auto table_id = scan.base_table_id;
    const auto& input = plan.inputs[table_id];
    
    ColumnBuffer buf(output_attrs.size(), input.num_rows);
    buf.types.reserve(output_attrs.size());
    for (auto& t : output_attrs) buf.types.push_back(std::get<1>(t));

    for (size_t col_idx = 0; col_idx < output_attrs.size(); ++col_idx) {
        size_t in_col_idx = std::get<0>(output_attrs[col_idx]);
        auto& column = input.columns[in_col_idx];
        auto& out_col = buf.columns[col_idx];

        for (size_t page_idx = 0; page_idx < column.pages.size(); ++page_idx) {
            auto* page = column.pages[page_idx]->data;
            switch (column.type) {
            case DataType::INT32: {
                uint16_t num_rows = *reinterpret_cast<uint16_t*>(page);
                auto* data_begin = reinterpret_cast<int32_t*>(page + 4);
                auto* bitmap = reinterpret_cast<uint8_t*>(page + PAGE_SIZE - (num_rows + 7) / 8);
                uint16_t data_idx = 0;
                for (uint16_t i = 0; i < num_rows; ++i) {
                    if (get_bitmap_local_col(bitmap, i)) {
                        out_col.append(value_t::make_i32(data_begin[data_idx++]));
                    } else {
                        out_col.append(value_t());
                    }
                }
                break;
            }
            case DataType::INT64: {
                uint16_t num_rows = *reinterpret_cast<uint16_t*>(page);
                auto* data_begin = reinterpret_cast<int64_t*>(page + 8);
                auto* bitmap = reinterpret_cast<uint8_t*>(page + PAGE_SIZE - (num_rows + 7) / 8);
                uint16_t data_idx = 0;
                for (uint16_t i = 0; i < num_rows; ++i) {
                    if (get_bitmap_local_col(bitmap, i)) {
                        out_col.append(value_t::make_i64(data_begin[data_idx++]));
                    } else {
                        out_col.append(value_t());
                    }
                }
                break;
            }
            case DataType::FP64: {
                uint16_t num_rows = *reinterpret_cast<uint16_t*>(page);
                auto* data_begin = reinterpret_cast<double*>(page + 8);
                auto* bitmap = reinterpret_cast<uint8_t*>(page + PAGE_SIZE - (num_rows + 7) / 8);
                uint16_t data_idx = 0;
                for (uint16_t i = 0; i < num_rows; ++i) {
                    if (get_bitmap_local_col(bitmap, i)) {
                        out_col.append(value_t::make_f64(data_begin[data_idx++]));
                    } else {
                        out_col.append(value_t());
                    }
                }
                break;
            }
            case DataType::VARCHAR: {
                uint16_t num_rows = *reinterpret_cast<uint16_t*>(page);
                if (num_rows == 0xffff) {
                    out_col.append(value_t::make_str(StringRef(static_cast<uint16_t>(table_id), 
                        static_cast<uint8_t>(in_col_idx), static_cast<uint32_t>(page_idx), 0xffff)));
                } else if (num_rows == 0xfffe) {
                    out_col.append(value_t());
                } else {
                    uint16_t num_non_null = *reinterpret_cast<uint16_t*>(page + 2);
                    auto* bitmap = reinterpret_cast<uint8_t*>(page + PAGE_SIZE - (num_rows + 7) / 8);
                    uint16_t data_idx = 0;
                    for (uint16_t i = 0; i < num_rows; ++i) {
                        if (get_bitmap_local_col(bitmap, i)) {
                            out_col.append(value_t::make_str(StringRef(static_cast<uint16_t>(table_id), 
                                static_cast<uint8_t>(in_col_idx), static_cast<uint32_t>(page_idx), data_idx)));
                            ++data_idx;
                        } else {
                            out_col.append(value_t());
                        }
                    }
                }
                break;
            }
            }
        }
    }

    return buf;
}

ColumnBuffer join_columnbuffer_hash(const Plan& plan,
    const JoinNode& join,
    const std::vector<std::tuple<size_t, DataType>>& output_attrs,
    const ColumnBuffer& left,
    const ColumnBuffer& right) {
    ColumnBuffer out(output_attrs.size(), 0);
    out.types.reserve(output_attrs.size());
    for (auto& t : output_attrs) out.types.push_back(std::get<1>(t));

    auto emit_pair = [&](size_t lidx, size_t ridx) {
        size_t left_cols = left.num_cols();
        for (size_t col_idx = 0; col_idx < output_attrs.size(); ++col_idx) {
            size_t src_idx = std::get<0>(output_attrs[col_idx]);
            if (src_idx < left_cols) out.columns[col_idx].append(left.columns[src_idx].get(lidx));
            else out.columns[col_idx].append(right.columns[src_idx - left_cols].get(ridx));
        }
        ++out.num_rows;
    };

    auto join_numeric = [&](auto tag, bool build_left_side) {
        using T = decltype(tag);
        std::unordered_map<T, std::vector<size_t>> ht;
        if (build_left_side) {
            for (size_t i = 0; i < left.num_rows; ++i) {
                const auto& v = left.columns[join.left_attr].get(i);
                if constexpr (std::is_same_v<T, int32_t>) {
                    if (v.type == value_t::Type::I32) ht[v.u.i32].push_back(i);
                } else if constexpr (std::is_same_v<T, int64_t>) {
                    if (v.type == value_t::Type::I64) ht[v.u.i64].push_back(i);
                } else if constexpr (std::is_same_v<T, double>) {
                    if (v.type == value_t::Type::FP64) ht[v.u.f64].push_back(i);
                }
            }
            for (size_t j = 0; j < right.num_rows; ++j) {
                const auto& v = right.columns[join.right_attr].get(j);
                if constexpr (std::is_same_v<T, int32_t>) {
                    if (v.type != value_t::Type::I32) continue;
                    auto it = ht.find(v.u.i32);
                    if (it != ht.end()) for (auto li : it->second) emit_pair(li, j);
                } else if constexpr (std::is_same_v<T, int64_t>) {
                    if (v.type != value_t::Type::I64) continue;
                    auto it = ht.find(v.u.i64);
                    if (it != ht.end()) for (auto li : it->second) emit_pair(li, j);
                } else if constexpr (std::is_same_v<T, double>) {
                    if (v.type != value_t::Type::FP64) continue;
                    auto it = ht.find(v.u.f64);
                    if (it != ht.end()) for (auto li : it->second) emit_pair(li, j);
                }
            }
        } else {
            for (size_t i = 0; i < right.num_rows; ++i) {
                const auto& v = right.columns[join.right_attr].get(i);
                if constexpr (std::is_same_v<T, int32_t>) {
                    if (v.type == value_t::Type::I32) ht[v.u.i32].push_back(i);
                } else if constexpr (std::is_same_v<T, int64_t>) {
                    if (v.type == value_t::Type::I64) ht[v.u.i64].push_back(i);
                } else if constexpr (std::is_same_v<T, double>) {
                    if (v.type == value_t::Type::FP64) ht[v.u.f64].push_back(i);
                }
            }
            for (size_t j = 0; j < left.num_rows; ++j) {
                const auto& v = left.columns[join.left_attr].get(j);
                if constexpr (std::is_same_v<T, int32_t>) {
                    if (v.type != value_t::Type::I32) continue;
                    auto it = ht.find(v.u.i32);
                    if (it != ht.end()) for (auto ri : it->second) emit_pair(j, ri);
                } else if constexpr (std::is_same_v<T, int64_t>) {
                    if (v.type != value_t::Type::I64) continue;
                    auto it = ht.find(v.u.i64);
                    if (it != ht.end()) for (auto ri : it->second) emit_pair(j, ri);
                } else if constexpr (std::is_same_v<T, double>) {
                    if (v.type != value_t::Type::FP64) continue;
                    auto it = ht.find(v.u.f64);
                    if (it != ht.end()) for (auto ri : it->second) emit_pair(j, ri);
                }
            }
        }
    };

    auto join_varchar = [&](bool build_left_side) {
        using Key = StringRef;
        StringRefHash hasher(&plan);
        StringRefEq eq(&plan);
        std::unordered_map<Key, std::vector<size_t>, StringRefHash, StringRefEq> ht(1024, hasher, eq);
        if (build_left_side) {
            for (size_t i = 0; i < left.num_rows; ++i) {
                const auto& v = left.columns[join.left_attr].get(i);
                if (v.type == value_t::Type::STR) ht[v.u.ref].push_back(i);
            }
            for (size_t j = 0; j < right.num_rows; ++j) {
                const auto& v = right.columns[join.right_attr].get(j);
                if (v.type != value_t::Type::STR) continue;
                auto it = ht.find(v.u.ref);
                if (it != ht.end()) for (auto li : it->second) emit_pair(li, j);
            }
        } else {
            for (size_t i = 0; i < right.num_rows; ++i) {
                const auto& v = right.columns[join.right_attr].get(i);
                if (v.type == value_t::Type::STR) ht[v.u.ref].push_back(i);
            }
            for (size_t j = 0; j < left.num_rows; ++j) {
                const auto& v = left.columns[join.left_attr].get(j);
                if (v.type != value_t::Type::STR) continue;
                auto it = ht.find(v.u.ref);
                if (it != ht.end()) for (auto ri : it->second) emit_pair(j, ri);
            }
        }
    };

    DataType key_type = join.build_left ? std::get<1>(plan.nodes[join.left].output_attrs[join.left_attr])
                                         : std::get<1>(plan.nodes[join.right].output_attrs[join.right_attr]);
    switch (key_type) {
        case DataType::INT32:   join_numeric(int32_t{}, join.build_left); break;
        case DataType::INT64:   join_numeric(int64_t{}, join.build_left); break;
        case DataType::FP64:    join_numeric(double{}, join.build_left); break;
        case DataType::VARCHAR: join_varchar(join.build_left); break;
    }

    return out;
}

ColumnarTable finalize_columnbuffer_to_columnar(const Plan& plan,
    const ColumnBuffer& buf,
    const std::vector<std::tuple<size_t, DataType>>& output_attrs) {
    // Convert paged columns directly to ColumnarTable format
    ColumnarTable result;
    result.num_rows = buf.num_rows;
    result.columns.reserve(output_attrs.size());

    StringRefResolver resolver(&plan);
    std::string tmp;

    for (size_t col_idx = 0; col_idx < output_attrs.size(); ++col_idx) {
        DataType dtype = std::get<1>(output_attrs[col_idx]);
        Column col(dtype);

        if (buf.num_rows == 0) {
            // Empty result
            result.columns.emplace_back(std::move(col));
            continue;
        }

        // Process the column data
        if (dtype == DataType::INT32) {
            ColumnInserter<int32_t> inserter(col);
            for (size_t row_idx = 0; row_idx < buf.num_rows; ++row_idx) {
                const auto& v = buf.columns[col_idx].get(row_idx);
                if (v.type == value_t::Type::I32) inserter.insert(v.u.i32);
                else inserter.insert_null();
            }
            inserter.finalize();
        } else if (dtype == DataType::INT64) {
            ColumnInserter<int64_t> inserter(col);
            for (size_t row_idx = 0; row_idx < buf.num_rows; ++row_idx) {
                const auto& v = buf.columns[col_idx].get(row_idx);
                if (v.type == value_t::Type::I64) inserter.insert(v.u.i64);
                else inserter.insert_null();
            }
            inserter.finalize();
        } else if (dtype == DataType::FP64) {
            ColumnInserter<double> inserter(col);
            for (size_t row_idx = 0; row_idx < buf.num_rows; ++row_idx) {
                const auto& v = buf.columns[col_idx].get(row_idx);
                if (v.type == value_t::Type::FP64) inserter.insert(v.u.f64);
                else inserter.insert_null();
            }
            inserter.finalize();
        } else if (dtype == DataType::VARCHAR) {
            ColumnInserter<std::string> inserter(col);
            for (size_t row_idx = 0; row_idx < buf.num_rows; ++row_idx) {
                const auto& v = buf.columns[col_idx].get(row_idx);
                if (v.type == value_t::Type::STR) {
                    auto [ptr, len] = resolver.resolve(v.u.ref, tmp);
                    if (ptr != nullptr) inserter.insert(std::string(ptr, ptr + len));
                    else inserter.insert_null();
                } else inserter.insert_null();
            }
            inserter.finalize();
        }

        result.columns.emplace_back(std::move(col));
    }

    return result;
}

} // namespace Contest

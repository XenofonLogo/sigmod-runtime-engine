
     #include "columnar.h"
#include "late_materialization.h"
#include <cstring>
#include <algorithm>
#include <unordered_map>

namespace Contest {

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
        
        // Build
        const ColumnBuffer& build_buf = build_left_side ? left : right;
        size_t build_key_idx = build_left_side ? join.left_attr : join.right_attr;
        
        for (size_t i = 0; i < build_buf.num_rows; ++i) {
            const auto& v = build_buf.columns[build_key_idx].get(i);
            if constexpr (std::is_same_v<T, int32_t>) {
                if (!v.is_null()) ht[v.as_i32()].push_back(i);
            } else if constexpr (std::is_same_v<T, int64_t>) {
                if (!v.is_null()) ht[v.as_i64()].push_back(i);
            } else if constexpr (std::is_same_v<T, double>) {
                if (!v.is_null()) ht[v.as_f64()].push_back(i);
            }
        }

        // Probe
        const ColumnBuffer& probe_buf = build_left_side ? right : left;
        size_t probe_key_idx = build_left_side ? join.right_attr : join.left_attr;

        for (size_t j = 0; j < probe_buf.num_rows; ++j) {
            const auto& v = probe_buf.columns[probe_key_idx].get(j);
            bool match = false;
            typename std::unordered_map<T, std::vector<size_t>>::iterator it;

            if constexpr (std::is_same_v<T, int32_t>) {
                if (!v.is_null()) { match = true; it = ht.find(v.as_i32()); }
            } else if constexpr (std::is_same_v<T, int64_t>) {
                if (!v.is_null()) { match = true; it = ht.find(v.as_i64()); }
            } else if constexpr (std::is_same_v<T, double>) {
                if (!v.is_null()) { match = true; it = ht.find(v.as_f64()); }
            }

            if (match && it != ht.end()) {
                for (auto build_idx : it->second) {
                    if (build_left_side) emit_pair(build_idx, j);
                    else emit_pair(j, build_idx);
                }
            }
        }
    };

    auto join_varchar = [&](bool build_left_side) {
        using Key = Contest::StringRef; // Use the typedef from late_materialization.h
        Contest::StringRefHash hasher(&plan);
        Contest::StringRefEq eq(&plan);
        std::unordered_map<Key, std::vector<size_t>, Contest::StringRefHash, Contest::StringRefEq> ht(1024, hasher, eq);
        
        // Build
        const ColumnBuffer& build_buf = build_left_side ? left : right;
        size_t build_key_idx = build_left_side ? join.left_attr : join.right_attr;

        for (size_t i = 0; i < build_buf.num_rows; ++i) {
            const auto& v = build_buf.columns[build_key_idx].get(i);
            if (!v.is_null()) {
                ht[Contest::PackedStringRef::unpack(v.as_ref())].push_back(i);
            }
        }

        // Probe
        const ColumnBuffer& probe_buf = build_left_side ? right : left;
        size_t probe_key_idx = build_left_side ? join.right_attr : join.left_attr;

        for (size_t j = 0; j < probe_buf.num_rows; ++j) {
            const auto& v = probe_buf.columns[probe_key_idx].get(j);
            if (!v.is_null()) {
                auto it = ht.find(Contest::PackedStringRef::unpack(v.as_ref()));
                if (it != ht.end()) {
                    for (auto build_idx : it->second) {
                        if (build_left_side) emit_pair(build_idx, j);
                        else emit_pair(j, build_idx);
                    }
                }
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

} // namespace Contest
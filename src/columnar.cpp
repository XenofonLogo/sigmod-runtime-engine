// columnar.cpp
#include "columnar.h"                  
#include "late_materialization.h"      
#include "parallel_unchained_hashtable.h" 
#include <cstring>                       
#include <algorithm>                     

namespace Contest {

// Returns a ColumnBuffer with the join result
// Performs a hash join over two ColumnBuffers and returns a new ColumnBuffer
ColumnBuffer join_columnbuffer_hash(const Plan& plan,
    const JoinNode& join,
    const std::vector<std::tuple<size_t, DataType>>& output_attrs,
    const ColumnBuffer& left,
    const ColumnBuffer& right) {
    // Create output buffer with the requested schema
    ColumnBuffer out(output_attrs.size(), 0);
    out.types.reserve(output_attrs.size());
    for (auto& t : output_attrs) out.types.push_back(std::get<1>(t));

    // Helper: append a pair (left row, right row) to the output
    auto emit_pair = [&](size_t lidx, size_t ridx) {
        size_t left_cols = left.num_cols();                  // Number of columns in left
        for (size_t col_idx = 0; col_idx < output_attrs.size(); ++col_idx) {
            size_t src_idx = std::get<0>(output_attrs[col_idx]); // Which source column it comes from
            if (src_idx < left_cols)                           // From left side
                out.columns[col_idx].append(left.columns[src_idx].get(lidx));
            else                                               // From right side
                out.columns[col_idx].append(right.columns[src_idx - left_cols].get(ridx));
        }
        ++out.num_rows;                                       // Increase output row count
    };

    // Join for INT32: uses a flat UnchainedHashTable (cache-friendly)
    auto join_numeric = [&](bool build_left_side) {
        using T = int32_t;

        const ColumnBuffer& build_buf = build_left_side ? left : right; // Build side
        size_t build_key_idx = build_left_side ? join.left_attr : join.right_attr; // Build key column

        // Collect entries from the build side (ignore NULLs)
        std::vector<HashEntry<T>> entries;
        entries.reserve(build_buf.num_rows);
        for (std::size_t i = 0; i < build_buf.num_rows; ++i) {
            const auto& v = build_buf.columns[build_key_idx].get(i);
            if (v.is_null()) continue;
            entries.push_back(HashEntry<T>{v.as_i32(), static_cast<uint32_t>(i)});
        }

        UnchainedHashTable<T> ht;               // Build hash table
        ht.build_from_entries(entries);

        const ColumnBuffer& probe_buf = build_left_side ? right : left; // Probe side
        size_t probe_key_idx = build_left_side ? join.right_attr : join.left_attr; // Probe key column

        // Probe all rows and emit matches
        for (std::size_t j = 0; j < probe_buf.num_rows; ++j) {
            const auto& v = probe_buf.columns[probe_key_idx].get(j);
            if (v.is_null()) continue;

            T probe_key = v.as_i32();

            std::size_t len = 0;
            const auto* base = ht.probe(probe_key, len); // Find bucket
            if (!base) continue;

            for (std::size_t k = 0; k < len; ++k) {
                if (base[k].key == probe_key) { // Confirm same key
                    if (build_left_side) emit_pair(static_cast<size_t>(base[k].row_id), j);
                    else emit_pair(j, static_cast<size_t>(base[k].row_id));
                }
            }
        }
    };

    // Join for VARCHAR: hash on packed 64-bit references to avoid string compares
    auto join_varchar = [&](bool build_left_side) {
        const ColumnBuffer& build_buf = build_left_side ? left : right;
        size_t build_key_idx = build_left_side ? join.left_attr : join.right_attr;

        std::vector<HashEntry<uint64_t>> entries; // Collect references
        entries.reserve(build_buf.num_rows);

        for (std::size_t i = 0; i < build_buf.num_rows; ++i) {
            const auto& v = build_buf.columns[build_key_idx].get(i);
            if (v.is_null()) continue;
            entries.push_back(HashEntry<uint64_t>{v.as_ref(), static_cast<uint32_t>(i)});
        }

        UnchainedHashTable<uint64_t> ht; // Build hash table for refs
        ht.build_from_entries(entries);

        const ColumnBuffer& probe_buf = build_left_side ? right : left; // Probe side
        size_t probe_key_idx = build_left_side ? join.right_attr : join.left_attr;

        for (std::size_t j = 0; j < probe_buf.num_rows; ++j) {
            const auto& v = probe_buf.columns[probe_key_idx].get(j);
            if (v.is_null()) continue;

            uint64_t key = v.as_ref();           // Packed ref
            std::size_t len = 0;
            const auto* base = ht.probe(key, len);
            if (!base) continue;

            for (std::size_t k = 0; k < len; ++k) {
                if (base[k].key == key) {
                    if (build_left_side) emit_pair(static_cast<size_t>(base[k].row_id), j);
                    else emit_pair(j, static_cast<size_t>(base[k].row_id));
                }
            }
        }
    };

    // Choose key type (INT32 or VARCHAR) based on the build side
    DataType key_type = join.build_left ? std::get<1>(plan.nodes[join.left].output_attrs[join.left_attr])
                                        : std::get<1>(plan.nodes[join.right].output_attrs[join.right_attr]);
    switch (key_type) {
        case DataType::INT32:   join_numeric(join.build_left); break;
        case DataType::VARCHAR: join_varchar(join.build_left); break;
        default: break;
    }

    return out;
}

} // namespace Contest
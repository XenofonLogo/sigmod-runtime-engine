#include <hardware.h>
#include <plan.h>
#include <table.h>
#include "columnar.h"

#include "unchained_hashtable.h"
#include "hash_functions.h"
#include "bloom_filter.h"

namespace Contest {

using ExecuteResult = ColumnBuffer;

ExecuteResult execute_impl(const Plan& plan, size_t node_idx);

// -----------------------------------------------------------------------------
// JoinAlgorithm (cleaned / INT32-only)
// -----------------------------------------------------------------------------
// Σημείωση: η εκφώνηση δηλώνει ρητά ότι οι ζεύξεις γίνονται πάνω σε στήλες INT32.
// Γι' αυτό η παρακάτω υλοποίηση υποστηρίζει μόνο INT32 join keys — απλοποιεί
// τον κώδικα και βελτιώνει την απόδοση (no generic templating).
// -----------------------------------------------------------------------------
struct JoinAlgorithm {
    bool                                             build_left;
    ExecuteResult&                                   left;      // build-side = left if build_left==true
    ExecuteResult&                                   right;     // probe-side = right if build_left==true
    ExecuteResult&                                   results;
    size_t                                           left_col, right_col;
    const std::vector<std::tuple<size_t, DataType>>& output_attrs;

    // run for INT32 joins using UnchainedHashTable over the columnar buffers
    void run_int32() {
        using Key = int32_t;

        // Which side is build/probe in terms of ColumnBuffer
        const ColumnBuffer* build_buf = build_left ? &left : &right;
        const ColumnBuffer* probe_buf = build_left ? &right : &left;
        size_t build_key_col = build_left ? left_col : right_col;
        size_t probe_key_col = build_left ? right_col : left_col;

        // gather entries from build side
        std::vector<std::pair<Key, std::size_t>> entries;
        entries.reserve(build_buf->num_rows);
        for (size_t i = 0; i < build_buf->num_rows; ++i) {
            const auto& v = build_buf->columns[build_key_col].get(i);
            if (v.type == value_t::Type::I32) {
                entries.emplace_back(v.u.i32, i);
            }
        }

        if (entries.empty()) return;

        UnchainedHashTable<Key> table;
        table.reserve(entries.size());
        table.build_from_entries(entries);

        // prepare output emitter
        auto emit_pair = [&](size_t lidx, size_t ridx) {
            size_t left_cols = left.num_cols();
            for (size_t col_idx = 0; col_idx < output_attrs.size(); ++col_idx) {
                size_t src_idx = std::get<0>(output_attrs[col_idx]);
                if (src_idx < left_cols) results.columns[col_idx].append(left.columns[src_idx].get(lidx));
                else results.columns[col_idx].append(right.columns[src_idx - left_cols].get(ridx));
            }
            ++results.num_rows;
        };

        // probe
        for (size_t j = 0; j < probe_buf->num_rows; ++j) {
            const auto& vp = probe_buf->columns[probe_key_col].get(j);
            if (vp.type != value_t::Type::I32) continue;
            Key probe_key = vp.u.i32;
            size_t len = 0;
            const auto* base = table.probe(probe_key, len);
            if (!base || len == 0) continue;
            for (size_t bi = 0; bi < len; ++bi) {
                if (base[bi].key != probe_key) continue;
                size_t build_row = base[bi].row_id;
                if (build_left) {
                    emit_pair(build_row, j);
                } else {
                    emit_pair(j, build_row);
                }
            }
        }
    }
};

// -----------------------------------------------------------------------------
// execute_hash_join / scan / impl / execute
// -----------------------------------------------------------------------------
ExecuteResult execute_hash_join(const Plan&          plan,
    const JoinNode&                                  join,
    const std::vector<std::tuple<size_t, DataType>>& output_attrs) {

    auto left_idx  = join.left;
    auto right_idx = join.right;
    auto& left_node  = plan.nodes[left_idx];
    auto& right_node = plan.nodes[right_idx];
    auto& left_types = left_node.output_attrs;
    auto& right_types= right_node.output_attrs;

    // execute subtrees to get ColumnBuffers
    auto left  = execute_impl(plan, left_idx);
    auto right = execute_impl(plan, right_idx);

    ColumnBuffer results(output_attrs.size(), 0);
    results.types.reserve(output_attrs.size());
    for (auto &t : output_attrs) results.types.push_back(std::get<1>(t));

    JoinAlgorithm join_algorithm{
        .build_left   = join.build_left,
        .left         = left,
        .right        = right,
        .results      = results,
        .left_col     = join.left_attr,
        .right_col    = join.right_attr,
        .output_attrs = output_attrs
    };

    // INT32-only as before
    if (join.build_left) {
        if (std::get<1>(left_types[join.left_attr]) != DataType::INT32) {
            throw std::runtime_error("Only INT32 join columns are supported in this executor");
        }
    } else {
        if (std::get<1>(right_types[join.right_attr]) != DataType::INT32) {
            throw std::runtime_error("Only INT32 join columns are supported in this executor");
        }
    }

    join_algorithm.run_int32();

    return results;
}

ExecuteResult execute_scan(const Plan&               plan,
    const ScanNode&                                  scan,
    const std::vector<std::tuple<size_t, DataType>>& output_attrs) {
    return scan_columnar_to_columnbuffer(plan, scan, output_attrs);
}

ExecuteResult execute_impl(const Plan& plan, size_t node_idx) {
    auto& node = plan.nodes[node_idx];
    return std::visit(
        [&](const auto& value) -> ExecuteResult {
            using T = std::decay_t<decltype(value)>;
            if constexpr (std::is_same_v<T, JoinNode>) {
                return execute_hash_join(plan, value, node.output_attrs);
            } else {
                return execute_scan(plan, value, node.output_attrs);
            }
        },
        node.data);
}

ColumnarTable execute(const Plan& plan, [[maybe_unused]] void* context) {
    auto buf = execute_impl(plan, plan.root);
    return finalize_columnbuffer_to_columnar(plan, buf, plan.nodes[plan.root].output_attrs);
}

void* build_context() {
    return nullptr;
}

void destroy_context([[maybe_unused]] void* context) {}

} // namespace Contest

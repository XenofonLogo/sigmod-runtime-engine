// execute.cpp
#include <hardware.h>
#include <plan.h>
#include <table.h>
#include "columnar.h"
#include "hashtable_interface.h" 

// Hash Table Implementations:
#include "unchained_hashtable_wrapper.h"
//#include "robinhood_wrapper.h"
//#include "cuckoo_wrapper.h"
// #include "hopscotch_wrapper.h" 
namespace Contest {

using ExecuteResult = ColumnBuffer;
ExecuteResult execute_impl(const Plan& plan, size_t node_idx);

// JoinAlgorithm (INT32-only)
struct JoinAlgorithm {
    bool                                               build_left;
    ExecuteResult&                                     left;      // build-side if build_left=true
    ExecuteResult&                                     right;     // probe-side if build_left=true
    ExecuteResult&                                     results;
    size_t                                             left_col, right_col;
    const std::vector<std::tuple<size_t, DataType>>& output_attrs;

    void run_int32() {
        using Key = int32_t;

        const ColumnBuffer* build_buf = build_left ? &left : &right;
        const ColumnBuffer* probe_buf = build_left ? &right : &left;
        size_t build_key_col = build_left ? left_col : right_col;
        size_t probe_key_col = build_left ? right_col : left_col;

        // Build entries: (key, row_id)
        std::vector<std::pair<Key, size_t>> entries;
        entries.reserve(build_buf->num_rows);
        for (size_t i = 0; i < build_buf->num_rows; ++i) {
            const auto& v = build_buf->columns[build_key_col].get(i);
            if (!v.is_null())
                entries.emplace_back(v.as_i32(), i);
        }

        if (entries.empty()) return;

        // Build hash table
        auto table = create_hashtable<Key>();
        table->reserve(entries.size());
        table->build_from_entries(entries);

        // Emit joined row
        auto emit_pair = [&](size_t lidx, size_t ridx) {
            size_t left_cols = left.num_cols();

            for (size_t i = 0; i < output_attrs.size(); ++i) {
                size_t src = std::get<0>(output_attrs[i]);
                if (src < left_cols)
                    results.columns[i].append(left.columns[src].get(lidx));
                else
                    results.columns[i].append(right.columns[src - left_cols].get(ridx));
            }
            results.num_rows++;
        };

        // Probe
        for (size_t j = 0; j < probe_buf->num_rows; ++j) {
            const auto& v = probe_buf->columns[probe_key_col].get(j);
            if (v.is_null()) continue;

            Key probe_key = v.as_i32();

            size_t len = 0;
            // Use the interface method (table->probe)
            const auto* bucket = table->probe(probe_key, len);
            if (!bucket || len == 0) continue;

            for (size_t k = 0; k < len; ++k) {
                
                if (bucket[k].key != probe_key) continue;
                size_t build_row = bucket[k].row_id;

                if (build_left)
                    emit_pair(build_row, j);
                else
                    emit_pair(j, build_row);
            }
        }
    }
};


ExecuteResult execute_hash_join(const Plan&                plan,
    const JoinNode&                                    join,
    const std::vector<std::tuple<size_t, DataType>>& output_attrs)
{
    size_t left_idx  = join.left;
    size_t right_idx = join.right;

    auto& left_node  = plan.nodes[left_idx];
    auto& right_node = plan.nodes[right_idx];

    auto left  = execute_impl(plan, left_idx);
    auto right = execute_impl(plan, right_idx);

    // prepare output ColumnBuffer
    ColumnBuffer results(output_attrs.size(), 0);
    results.types.reserve(output_attrs.size());
    for (auto& t : output_attrs)
        results.types.push_back(std::get<1>(t));

    JoinAlgorithm ja {
        .build_left   = join.build_left,
        .left         = left,
        .right        = right,
        .results      = results,
        .left_col     = join.left_attr,
        .right_col    = join.right_attr,
        .output_attrs = output_attrs
    };

    // enforce INT32 join key
    if (join.build_left) {
        if (std::get<1>(left_node.output_attrs[join.left_attr]) != DataType::INT32)
            throw std::runtime_error("Only INT32 join columns supported.");
    } else {
        if (std::get<1>(right_node.output_attrs[join.right_attr]) != DataType::INT32)
            throw std::runtime_error("Only INT32 join columns supported.");
    }

    // run the hashtable join
    ja.run_int32();

    return results;
}


ExecuteResult execute_scan(const Plan&                  plan,
    const ScanNode&                                    scan,
    const std::vector<std::tuple<size_t, DataType>>& output_attrs)
{
    return scan_columnar_to_columnbuffer(plan, scan, output_attrs);
}


ExecuteResult execute_impl(const Plan& plan, size_t node_idx) {
    auto& node = plan.nodes[node_idx];
    return std::visit(
        [&](auto const& value) -> ExecuteResult {
            using T = std::decay_t<decltype(value)>;
            if constexpr (std::is_same_v<T, JoinNode>)
                return execute_hash_join(plan, value, node.output_attrs);
            else
                return execute_scan(plan, value, node.output_attrs);
        },
        node.data);
}

ColumnarTable execute(const Plan& plan, void* context) {
    auto buf = execute_impl(plan, plan.root);
    return finalize_columnbuffer_to_columnar(
        plan, buf, plan.nodes[plan.root].output_attrs
    );
}

void* build_context() { return nullptr; }
void destroy_context(void*) {}

} // namespace Contest
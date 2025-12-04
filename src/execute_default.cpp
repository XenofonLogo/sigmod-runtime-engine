#include <hardware.h>
#include <plan.h>
#include <table.h>
#include "columnar.h"

namespace Contest {

using ExecuteResult = ColumnBuffer;

ExecuteResult execute_impl(const Plan& plan, size_t node_idx);

ExecuteResult execute_hash_join(const Plan&          plan,
    const JoinNode&                                  join,
    const std::vector<std::tuple<size_t, DataType>>& output_attrs) {
    auto left_idx  = join.left;
    auto right_idx = join.right;
    auto& left_node  = plan.nodes[left_idx];
    auto& right_node = plan.nodes[right_idx];

    auto left  = execute_impl(plan, left_idx);
    auto right = execute_impl(plan, right_idx);

    // Use columnar join implementation
    return join_columnbuffer_hash(plan, join, output_attrs, left, right);
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
    namespace views = ranges::views;
    auto buf = execute_impl(plan, plan.root);
    // finalize to ColumnarTable without materializing intermediate rows
    return finalize_columnbuffer_to_columnar(plan, buf, plan.nodes[plan.root].output_attrs);
}

void* build_context() {
    return nullptr;
}

void destroy_context([[maybe_unused]] void* context) {}

} // namespace Contest

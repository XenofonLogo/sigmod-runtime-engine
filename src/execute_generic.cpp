#include <hardware.h>
#include <plan.h>
#include <table.h>

#include "columnar.h"
#include "late_materialization.h"

#include <unordered_map>
#include <vector>

namespace Contest {

// -----------------------------
// Row-store types
// -----------------------------
using Row      = std::vector<value_t>;
using RowStore = std::vector<Row>;

// -----------------------------
// ColumnBuffer -> RowStore
// -----------------------------
static RowStore columnbuffer_to_rowstore(const ColumnBuffer &buf) {
    RowStore rows;
    rows.resize(buf.num_rows);

    if (buf.num_cols() == 0 || buf.num_rows == 0) {
        return rows;
    }

    // Κάθε row έχει num_cols στήλες
    for (auto &r : rows) {
        r.resize(buf.num_cols());
    }

    for (size_t c = 0; c < buf.num_cols(); ++c) {
        const auto &col = buf.columns[c];
        for (size_t r = 0; r < buf.num_rows; ++r) {
            rows[r][c] = col.get(r);
        }
    }

    return rows;
}

// -----------------------------
// RowStore -> ColumnBuffer
// (για το τελικό ColumnarTable)
// -----------------------------
static ColumnBuffer rowstore_to_columnbuffer(
    const RowStore &rows,
    const std::vector<std::tuple<size_t, DataType>> &output_attrs)
{
    size_t num_rows = rows.size();
    size_t num_cols = output_attrs.size();

    ColumnBuffer buf(num_cols, num_rows);
    buf.types.reserve(num_cols);
    for (auto &t : output_attrs) {
        buf.types.push_back(std::get<1>(t));
    }

    if (num_rows == 0 || num_cols == 0) {
        return buf;
    }

    // Γράφουμε στήλη-στήλη
    for (size_t c = 0; c < num_cols; ++c) {
        auto &out_col = buf.columns[c];
        for (size_t r = 0; r < num_rows; ++r) {
            out_col.append(rows[r][c]);
        }
    }

    buf.num_rows = num_rows;
    return buf;
}

// -----------------------------
// Scan: ColumnarTable -> RowStore
// Χρησιμοποιούμε το columnar.cpp για LM scan,
// αλλά δουλεύουμε τελικά σε row-store.
// -----------------------------
static RowStore execute_scan_rowstore(const Plan &plan,
                                      const ScanNode &scan,
                                      const std::vector<std::tuple<size_t, DataType>> &output_attrs)
{
    // 1) Columnar -> ColumnBuffer (value_t)  (late materialization για VARCHAR)
    ColumnBuffer buf = scan_columnar_to_columnbuffer(plan, scan, output_attrs);

    // 2) ColumnBuffer -> RowStore
    return columnbuffer_to_rowstore(buf);
}

// -----------------------------
// Join σε RowStore (INT32 keys)
// -----------------------------
static RowStore execute_join_rowstore(const Plan &plan,
                                      const JoinNode &join,
                                      const std::vector<std::tuple<size_t, DataType>> &output_attrs,
                                      const RowStore &left,
                                      const RowStore &right)
{
    RowStore result;
    result.reserve(left.size() + right.size());

    size_t left_cols  = left.empty()  ? 0 : left[0].size();
    size_t right_cols = right.empty() ? 0 : right[0].size();

    // Helper: φτιάχνει ένα output row σύμφωνα με output_attrs
    auto emit_pair = [&](size_t li, size_t ri) {
        Row row;
        row.reserve(output_attrs.size());
        for (size_t out_idx = 0; out_idx < output_attrs.size(); ++out_idx) {
            size_t src_idx = std::get<0>(output_attrs[out_idx]);
            if (src_idx < left_cols) {
                row.push_back(left[li][src_idx]);
            } else {
                row.push_back(right[ri][src_idx - left_cols]);
            }
        }
        result.emplace_back(std::move(row));
    };

    // Βρίσκουμε τον τύπο του κλειδιού (η εκφώνηση λέει INT32, αλλά ας το ελέγξουμε)
    const auto &left_node  = plan.nodes[join.left];
    const auto &right_node = plan.nodes[join.right];

    DataType key_type =
        join.build_left
            ? std::get<1>(left_node.output_attrs[join.left_attr])
            : std::get<1>(right_node.output_attrs[join.right_attr]);

    // Σύμφωνα με την εκφώνηση, οι ζεύξεις γίνονται πάνω σε INT32.
    if (key_type != DataType::INT32) {
        // Αν θέλεις, μπορείς να το επεκτείνεις για INT64/FP64 με copy-paste.
        throw std::runtime_error("execute_join_rowstore: expected INT32 join key");
    }

    std::unordered_map<int32_t, std::vector<size_t>> ht;

    if (join.build_left) {
        // Build side = left
        for (size_t i = 0; i < left.size(); ++i) {
            const auto &v = left[i][join.left_attr];
            if (v.type == value_t::Type::I32) {
                ht[v.u.i32].push_back(i);
            }
        }
        // Probe side = right
        for (size_t j = 0; j < right.size(); ++j) {
            const auto &v = right[j][join.right_attr];
            if (v.type != value_t::Type::I32) continue; // NULL ή άλλο type
            auto it = ht.find(v.u.i32);
            if (it != ht.end()) {
                for (auto li : it->second) {
                    emit_pair(li, j);
                }
            }
        }
    } else {
        // Build side = right
        for (size_t i = 0; i < right.size(); ++i) {
            const auto &v = right[i][join.right_attr];
            if (v.type == value_t::Type::I32) {
                ht[v.u.i32].push_back(i);
            }
        }
        // Probe side = left
        for (size_t j = 0; j < left.size(); ++j) {
            const auto &v = left[j][join.left_attr];
            if (v.type != value_t::Type::I32) continue;
            auto it = ht.find(v.u.i32);
            if (it != ht.end()) {
                for (auto ri : it->second) {
                    emit_pair(j, ri);
                }
            }
        }
    }

    return result;
}

// -----------------------------
// Recursive executor πάνω σε RowStore
// -----------------------------
static RowStore execute_impl_rowstore(const Plan &plan, size_t node_idx) {
    const auto &node = plan.nodes[node_idx];

    return std::visit(
        [&](const auto &n) -> RowStore {
            using T = std::decay_t<decltype(n)>;
            if constexpr (std::is_same_v<T, ScanNode>) {
                return execute_scan_rowstore(plan, n, node.output_attrs);
            } else {
                // JoinNode
                auto left_rows  = execute_impl_rowstore(plan, n.left);
                auto right_rows = execute_impl_rowstore(plan, n.right);
                return execute_join_rowstore(plan, n, node.output_attrs, left_rows, right_rows);
            }
        },
        node.data);
}

// -----------------------------
// Top-level execute (Version B)
// RowStore -> ColumnBuffer -> ColumnarTable
// -----------------------------
ColumnarTable execute(const Plan &plan, [[maybe_unused]] void *context) {
    // Εκτέλεση πλάνου σε row-store (value_t, LM για strings)
    RowStore rows = execute_impl_rowstore(plan, plan.root);

    const auto &attrs = plan.nodes[plan.root].output_attrs;

    // RowStore -> ColumnBuffer
    ColumnBuffer buf = rowstore_to_columnbuffer(rows, attrs);

    // ColumnBuffer -> ColumnarTable (τελική materialization για VARCHAR)
    return finalize_columnbuffer_to_columnar(plan, buf, attrs);
}

void* build_context() {
    return nullptr;
}

void destroy_context([[maybe_unused]] void *context) {
    // nothing to clean up
}

} // namespace Contest
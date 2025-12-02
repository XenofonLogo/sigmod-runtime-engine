#include <hardware.h>
#include <plan.h>
#include <table.h>
#include "cuckoo_map.h"
#include "late_materialization.h"

// Additional includes used by the integration helpers below
#include <optional>
#include <type_traits>
#include <string_view>
#include <set>
#include <algorithm>
#include <iterator>

namespace Contest {

using ExecuteResult = std::vector<std::vector<Data>>;

ExecuteResult execute_impl(const Plan& plan, size_t node_idx);

ExecuteResult execute_hash_join(const Plan& plan,
    const JoinNode& join,
    const std::vector<std::tuple<size_t, DataType>>& output_attrs);

ExecuteResult execute_scan(const Plan& plan,
    const ScanNode& scan,
    const std::vector<std::tuple<size_t, DataType>>& output_attrs) {
    auto table_id = scan.base_table_id;
    auto& input   = plan.inputs[table_id];
    return Table::copy_scan(input, output_attrs);
}

ExecuteResult execute_impl(const Plan& plan, size_t node_idx) {
    auto& node = plan.nodes[node_idx];
    return std::visit(
        [&](const auto& value) {
            using T = std::decay_t<decltype(value)>;
            if constexpr (std::is_same_v<T, JoinNode>) {
                return execute_hash_join(plan, value, node.output_attrs);
            } else {
                return execute_scan(plan, value, node.output_attrs);
            }
        },
        node.data);
}

ColumnarTable build_root_columnar_from_plan(const Plan &plan);

ColumnarTable execute(const Plan& plan, [[maybe_unused]] void* context) {
    // New flow:
    // If the plan root is a JoinNode we prefer to run the late-materialization
    // path that keeps VARCHAR columns as PackedStringRef during the join and
    // only materializes strings at the very end when populating pages.
    //
    // For other cases we fallback to the legacy execute_impl -> Table -> to_columnar.
    return build_root_columnar_from_plan(plan);
}

void* build_context() {
    return nullptr;
}

void destroy_context([[maybe_unused]] void* context) {}

} // namespace Contest


/* ---------------------------------------------------------------------------
 * Helper functions: integrate late_materialization with existing ExecuteResult
 * -------------------------------------------------------------------------*/

namespace Contest {

// try_extract<T> : attempt to extract a T from the Data variant.
template <class T>
static std::optional<T> try_extract(const Data& d) {
    std::optional<T> out;
    std::visit([&](auto&& v) {
        using V = std::decay_t<decltype(v)>;
        if constexpr (std::is_same_v<V, std::monostate>) {
            /* leave empty */ ;
        } else if constexpr (std::is_same_v<V, T>) {
            out = static_cast<T>(v);
        } else if constexpr (std::is_arithmetic_v<V> && std::is_arithmetic_v<T>) {
            out = static_cast<T>(v);
        } else if constexpr (std::is_same_v<T, std::string>) {
            if constexpr (std::is_convertible_v<V, std::string_view>) {
                out = std::string(v);
            }
        } else if constexpr (std::is_same_v<T, std::string_view>) {
            if constexpr (std::is_convertible_v<V, std::string_view>) {
                out = std::string_view(v);
            }
        }
    }, d);
    return out;
}

// Convert ExecuteResult into a Catalog table with one page per column.
static void build_catalog_from_execute_result(
    const ExecuteResult &rows,
    const std::vector<std::tuple<size_t, DataType>> &attrs,
    Catalog &catalog,
    uint8_t table_id)
{
    Table tab;
    tab.table_id = table_id;
    tab.columns.resize(attrs.size());

    size_t nrows = rows.size();

    for (size_t c = 0; c < attrs.size(); ++c) {
        DataType dtype = std::get<1>(attrs[c]);
        Column &col = tab.columns[c];
        col.is_int = (dtype != DataType::VARCHAR);

        if (col.is_int) {
            IntPage page;
            page.values.reserve(nrows);
            for (size_t r = 0; r < nrows; ++r) {
                const Data &cell = rows[r][c];
                if (dtype == DataType::INT32) {
                    auto val = try_extract<int32_t>(cell);
                    page.values.push_back(val.value_or(0));
                } else if (dtype == DataType::INT64) {
                    auto val = try_extract<int64_t>(cell);
                    page.values.push_back(static_cast<int32_t>(val.value_or(0)));
                } else if (dtype == DataType::FP64) {
                    auto val = try_extract<double>(cell);
                    page.values.push_back(static_cast<int32_t>(val.value_or(0.0)));
                } else {
                    page.values.push_back(0);
                }
            }
            col.int_pages.push_back(std::move(page));
        } else {
            VarcharPage page;
            page.values.reserve(nrows);
            for (size_t r = 0; r < nrows; ++r) {
                const Data &cell = rows[r][c];
                auto sval = try_extract<std::string>(cell);
                page.values.push_back(sval.value_or(std::string()));
            }
            col.str_pages.push_back(std::move(page));
        }
    }

    catalog.tables.emplace(table_id, std::move(tab));
}

// Convert ColumnarResult → ColumnarTable (materialize VARCHARs now).
static ColumnarTable columnar_table_from_columnar_result(
    const ColumnarResult &res,
    const std::vector<std::tuple<size_t, DataType>> &output_schema,
    const Catalog &catalog_for_materialization)
{
    ColumnarTable out;
    out.num_rows = res.num_rows;
    out.columns.reserve(output_schema.size());

    for (size_t c = 0; c < output_schema.size(); ++c) {
        DataType dtype = std::get<1>(output_schema[c]);
        Column col(dtype);

        if (dtype == DataType::INT32) {
            ColumnInserter<int32_t> ins(col);
            for (size_t r = 0; r < res.num_rows; ++r)
                ins.insert(res.int_cols[c][r]);
            ins.finalize();

        } else if (dtype == DataType::INT64) {
            ColumnInserter<int64_t> ins(col);
            for (size_t r = 0; r < res.num_rows; ++r)
                ins.insert(static_cast<int64_t>(res.int_cols[c][r]));
            ins.finalize();

        } else if (dtype == DataType::FP64) {
            ColumnInserter<double> ins(col);
            for (size_t r = 0; r < res.num_rows; ++r)
                ins.insert(static_cast<double>(res.int_cols[c][r]));
            ins.finalize();

        } else { // VARCHAR
            ColumnInserter<std::string> ins(col);
            for (size_t r = 0; r < res.num_rows; ++r) {
                const PackedStringRef &ref = res.str_refs[c][r];
                std::string s = materialize_string(catalog_for_materialization, ref);
                ins.insert(s);
            }
            ins.finalize();
        }

        out.columns.push_back(std::move(col));
    }

    return out;
}

// Build ColumnarTable for root using late materialization when possible.
static ColumnarTable build_root_columnar_from_plan(const Plan &plan) {
    auto &root_node = plan.nodes[plan.root];

    if (std::holds_alternative<JoinNode>(root_node.data)) {
        const JoinNode &jn = std::get<JoinNode>(root_node.data);

        ExecuteResult left_rows  = execute_impl(plan, jn.left);
        ExecuteResult right_rows = execute_impl(plan, jn.right);

        auto &left_node  = plan.nodes[jn.left];
        auto &right_node = plan.nodes[jn.right];

        const auto &left_types  = left_node.output_attrs;
        const auto &right_types = right_node.output_attrs;

        Catalog catalog;
        build_catalog_from_execute_result(left_rows, left_types, catalog, 0);
        build_catalog_from_execute_result(right_rows, right_types, catalog, 1);

        size_t left_cols = left_rows.empty() ? left_types.size() : left_rows[0].size();

        std::vector<uint8_t> outA_cols, outB_cols;
        for (const auto &attr : root_node.output_attrs) {
            size_t src_idx = std::get<0>(attr);
            if (src_idx < left_cols)
                outA_cols.push_back(uint8_t(src_idx));
            else
                outB_cols.push_back(uint8_t(src_idx - left_cols));
        }

        ColumnarResult cres = direct_hash_join_produce_columnar(
            catalog,
            0,
            uint8_t(jn.left_attr),
            outA_cols,
            1,
            uint8_t(jn.right_attr),
            outB_cols
        );

        return columnar_table_from_columnar_result(
            cres, root_node.output_attrs, catalog
        );
    }

    namespace views = ranges::views;
    auto ret       = execute_impl(plan, plan.root);
    auto ret_types = plan.nodes[plan.root].output_attrs
                   | views::transform([](const auto& v) { return std::get<1>(v); })
                   | ranges::to<std::vector<DataType>>();
    Table table{std::move(ret), std::move(ret_types)};
    return table.to_columnar();
}

} // namespace Contest

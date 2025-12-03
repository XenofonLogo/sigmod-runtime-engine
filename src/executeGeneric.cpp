// executeGeneric.cpp
// Generic single-executable engine with late materialization for root joins.
// Produces ColumnarTable directly
#include <algorithm>
#include <optional>
#include <type_traits>
#include <string>
#include <string_view>
#include <vector>
#include <unordered_map>
#include <stdexcept>
#include <utility>
#include <cstdint>
#include <iostream>
#include <hardware.h>
#include <plan.h>
#include <table.h>
#include "late_materialization.h"

namespace Contest
{
    using ExecuteResult = std::vector<std::vector<Data>>;

    // Forward declaration
    ExecuteResult execute_impl(const Plan &plan, size_t node_idx);

    // -------------------------------------------------------------------------
    // Simple, conservative extraction helper: convert Data variant -> T
    // Tries to be permissive for numeric conversions; returns std::nullopt on mismatch
    // -------------------------------------------------------------------------
    template <class T>
    static std::optional<T> try_extract(const Data &d)
    {
        std::optional<T> out;
        std::visit([&](auto &&v)
                   {
        using V = std::decay_t<decltype(v)>;
        if constexpr (std::is_same_v<V, std::monostate>) {
            // leave empty (NULL)
        } else if constexpr (std::is_same_v<V, T>) {
            out = static_cast<T>(v);
        } else if constexpr (std::is_arithmetic_v<V> && std::is_arithmetic_v<T>) {
            out = static_cast<T>(v);
        } else if constexpr (std::is_same_v<T, std::string>) {
            if constexpr (std::is_convertible_v<V, std::string_view>) {
                out = std::string(v);
            }
        } else {
            // type mismatch -> nullopt
        } }, d);
        return out;
    }

    // -------------------------------------------------------------------------
    // Convert an ExecuteResult (rows of Data) into a temporary Catalog table.
    // We place everything into single-page-per-column so PackedStringRef(page=0,offset=row)
    // will be valid for later materialization.
    // -------------------------------------------------------------------------
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

        for (size_t c = 0; c < attrs.size(); ++c)
        {
            DataType dtype = std::get<1>(attrs[c]);
            Column &col = tab.columns[c];
            col.is_int = (dtype != DataType::VARCHAR);
            if (col.is_int)
            {
                IntPage page;
                page.values.reserve(nrows);
                for (size_t r = 0; r < nrows; ++r)
                {
                    const Data &cell = rows[r][c];
                    if (dtype == DataType::INT32)
                    {
                        auto v = try_extract<int32_t>(cell);
                        page.values.push_back(v.value_or(0));
                    }
                    else if (dtype == DataType::INT64)
                    {
                        auto v = try_extract<int64_t>(cell);
                        page.values.push_back(static_cast<int32_t>(v.value_or(0)));
                    }
                    else if (dtype == DataType::FP64)
                    {
                        auto v = try_extract<double>(cell);
                        page.values.push_back(static_cast<int32_t>(v.value_or(0.0)));
                    }
                    else
                    {
                        page.values.push_back(0);
                    }
                }
                col.int_pages.push_back(std::move(page));
            }
            else
            {
                VarcharPage page;
                page.values.reserve(nrows);
                for (size_t r = 0; r < nrows; ++r)
                {
                    const Data &cell = rows[r][c];
                    auto sval = try_extract<std::string>(cell);
                    page.values.push_back(sval.value_or(std::string()));
                }
                col.str_pages.push_back(std::move(page));
            }
        }

        catalog.tables.emplace(table_id, std::move(tab));
    }

    // -------------------------------------------------------------------------
    // Convert ColumnarResult -> ColumnarTable using ColumnInserter<T>.
    // ColumnInserter comes from plan.h; it will write pages in the exact format
    // the contest expects.
    // -------------------------------------------------------------------------
    static ColumnarTable columnar_table_from_columnar_result(
        const ColumnarResult &res,
        const std::vector<std::tuple<size_t, DataType>> &output_schema,
        const Catalog &catalog_for_materialization)
    {
        ColumnarTable out;
        out.num_rows = res.num_rows;
        out.columns.reserve(output_schema.size());

        // For each output column, create a Column and fill it using ColumnInserter
        for (size_t c = 0; c < output_schema.size(); ++c)
        {
            DataType dtype = std::get<1>(output_schema[c]);
            Column col(dtype);

            if (dtype == DataType::INT32)
            {
                ColumnInserter<int32_t> ins(col);
                for (size_t r = 0; r < res.num_rows; ++r)
                {
                    ins.insert(res.int_cols[c][r]);
                }
                ins.finalize();
            }
            else if (dtype == DataType::INT64)
            {
                ColumnInserter<int64_t> ins(col);
                for (size_t r = 0; r < res.num_rows; ++r)
                {
                    ins.insert(static_cast<int64_t>(res.int_cols[c][r]));
                }
                ins.finalize();
            }
            else if (dtype == DataType::FP64)
            {
                ColumnInserter<double> ins(col);
                for (size_t r = 0; r < res.num_rows; ++r)
                {
                    ins.insert(static_cast<double>(res.int_cols[c][r]));
                }
                ins.finalize();
            }
            else
            { // VARCHAR
                ColumnInserter<std::string> ins(col);
                // res.str_refs[c] contains PackedStringRef for each row
                for (size_t r = 0; r < res.num_rows; ++r)
                {
                    const PackedStringRef &ref = res.str_refs[c][r];
                    // materialize from the small catalog we built earlier
                    std::string s = materialize_string(catalog_for_materialization, ref);
                    ins.insert(s);
                }
                ins.finalize();
            }

            out.columns.push_back(std::move(col));
        }

        return out;
    }

    // -------------------------------------------------------------------------
    // Build ColumnarTable for the plan root using late materialization when root is Join.
    // Otherwise fall back to legacy path.
    // -------------------------------------------------------------------------
    static ColumnarTable build_root_columnar_from_plan(const Plan &plan)
    {
        auto &root_node = plan.nodes[plan.root];

        if (std::holds_alternative<JoinNode>(root_node.data))
        {
            const JoinNode &jn = std::get<JoinNode>(root_node.data);

            // Execute children to get ExecuteResult
            ExecuteResult left_rows = execute_impl(plan, jn.left);
            ExecuteResult right_rows = execute_impl(plan, jn.right);

            auto &left_node = plan.nodes[jn.left];
            auto &right_node = plan.nodes[jn.right];

            const auto &left_types = left_node.output_attrs;
            const auto &right_types = right_node.output_attrs;

            // Build small catalogs for both children:
            Catalog catalog;
            build_catalog_from_execute_result(left_rows, left_types, catalog, /*table_id=*/0);
            build_catalog_from_execute_result(right_rows, right_types, catalog, /*table_id=*/1);

            // Determine which output columns come from left and which from right.
            size_t left_cols = left_types.size();

            std::vector<uint8_t> outA_cols, outB_cols;
            for (const auto &attr : root_node.output_attrs)
            {
                size_t src_idx = std::get<0>(attr);
                if (src_idx < left_cols)
                    outA_cols.push_back(static_cast<uint8_t>(src_idx));
                else
                    outB_cols.push_back(static_cast<uint8_t>(src_idx - left_cols));
            }

            // Call late-materialization join; this returns ColumnarResult which keeps
            // VARCHAR columns as PackedStringRef.
            ColumnarResult cres = direct_hash_join_produce_columnar(
                catalog,
                /*tableA=*/0,
                /*keyA_col=*/static_cast<uint8_t>(jn.left_attr),
                outA_cols,
                /*tableB=*/1,
                /*keyB_col=*/static_cast<uint8_t>(jn.right_attr),
                outB_cols);

            // Convert ColumnarResult -> ColumnarTable (materialize strings now)
            ColumnarTable final_table = columnar_table_from_columnar_result(
                cres, root_node.output_attrs, catalog);

            return final_table;
        }

        // Fallback legacy path:
        namespace views = ranges::views;
        auto ret = execute_impl(plan, plan.root);
        auto ret_types = plan.nodes[plan.root].output_attrs | views::transform([](const auto &v)
                                                                               { return std::get<1>(v); }) |
                         ranges::to<std::vector<DataType>>();
        Table table{std::move(ret), std::move(ret_types)};
        return table.to_columnar();
    }

    // -------------------------------------------------------------------------
    // Generic hash join used for internal (non-root) join nodes
    // (based on the simple original code, but left generic).
    // -------------------------------------------------------------------------
    struct JoinAlgorithm
    {
        bool build_left;
        ExecuteResult &left;
        ExecuteResult &right;
        ExecuteResult &results;
        size_t left_col, right_col;
        const std::vector<std::tuple<size_t, DataType>> &output_attrs;

        template <class T>
        auto run()
        {
            namespace views = ranges::views;
            std::unordered_map<T, std::vector<size_t>> hash_table;
            if (build_left)
            {
                for (auto &&[idx, record] : left | views::enumerate)
                {
                    std::visit([&](const auto &key)
                               {
                    using Tk = std::decay_t<decltype(key)>;
                    if constexpr (std::is_same_v<Tk, T>) {
                        if (auto itr = hash_table.find(key); itr == hash_table.end()) {
                            hash_table.emplace(key, std::vector<size_t>(1, idx));
                        } else {
                            itr->second.push_back(idx);
                        }
                    } else if constexpr (not std::is_same_v<Tk, std::monostate>) {
                        throw std::runtime_error("wrong type of field in join");
                    } }, record[left_col]);
                }
                for (auto &right_record : right)
                {
                    std::visit([&](const auto &key)
                               {
                    using Tk = std::decay_t<decltype(key)>;
                    if constexpr (std::is_same_v<Tk, T>) {
                        if (auto itr = hash_table.find(key); itr != hash_table.end()) {
                            for (auto left_idx: itr->second) {
                                auto&             left_record = left[left_idx];
                                std::vector<Data> new_record;
                                new_record.reserve(output_attrs.size());
                                for (auto [col_idx, _]: output_attrs) {
                                    if (col_idx < left_record.size()) {
                                        new_record.emplace_back(left_record[col_idx]);
                                    } else {
                                        new_record.emplace_back(
                                            right_record[col_idx - left_record.size()]);
                                    }
                                }
                                results.emplace_back(std::move(new_record));
                            }
                        }
                    } else if constexpr (not std::is_same_v<Tk, std::monostate>) {
                        throw std::runtime_error("wrong type of field in join");
                    } }, right_record[right_col]);
                }
            }
            else
            {
                for (auto &&[idx, record] : right | views::enumerate)
                {
                    std::visit([&](const auto &key)
                               {
                    using Tk = std::decay_t<decltype(key)>;
                    if constexpr (std::is_same_v<Tk, T>) {
                        if (auto itr = hash_table.find(key); itr == hash_table.end()) {
                            hash_table.emplace(key, std::vector<size_t>(1, idx));
                        } else {
                            itr->second.push_back(idx);
                        }
                    } else if constexpr (not std::is_same_v<Tk, std::monostate>) {
                        throw std::runtime_error("wrong type of field in join");
                    } }, record[right_col]);
                }
                for (auto &left_record : left)
                {
                    std::visit([&](const auto &key)
                               {
                    using Tk = std::decay_t<decltype(key)>;
                    if constexpr (std::is_same_v<Tk, T>) {
                        if (auto itr = hash_table.find(key); itr != hash_table.end()) {
                            for (auto right_idx: itr->second) {
                                auto&             right_record = right[right_idx];
                                std::vector<Data> new_record;
                                new_record.reserve(output_attrs.size());
                                for (auto [col_idx, _]: output_attrs) {
                                    if (col_idx < left_record.size()) {
                                        new_record.emplace_back(left_record[col_idx]);
                                    } else {
                                        new_record.emplace_back(
                                            right_record[col_idx - left_record.size()]);
                                    }
                                }
                                results.emplace_back(std::move(new_record));
                            }
                        }
                    } else if constexpr (not std::is_same_v<Tk, std::monostate>) {
                        throw std::runtime_error("wrong type of field in join");
                    } }, left_record[left_col]);
                }
            }
        }
    };

    // -------------------------------------------------------------------------
    // Legacy execute_hash_join for internal nodes (returns ExecuteResult).
    // -------------------------------------------------------------------------
    ExecuteResult execute_hash_join(const Plan &plan,
                                    const JoinNode &join,
                                    const std::vector<std::tuple<size_t, DataType>> &output_attrs)
    {
        auto left_idx = join.left;
        auto right_idx = join.right;
        auto &left_node = plan.nodes[left_idx];
        auto &right_node = plan.nodes[right_idx];
        auto &left_types = left_node.output_attrs;
        auto &right_types = right_node.output_attrs;
        auto left = execute_impl(plan, left_idx);
        auto right = execute_impl(plan, right_idx);
        std::vector<std::vector<Data>> results;

        JoinAlgorithm join_algorithm{.build_left = join.build_left,
                                     .left = left,
                                     .right = right,
                                     .results = results,
                                     .left_col = join.left_attr,
                                     .right_col = join.right_attr,
                                     .output_attrs = output_attrs};
        if (join.build_left)
        {
            switch (std::get<1>(left_types[join.left_attr]))
            {
            case DataType::INT32:
                join_algorithm.run<int32_t>();
                break;
            case DataType::INT64:
                join_algorithm.run<int64_t>();
                break;
            case DataType::FP64:
                join_algorithm.run<double>();
                break;
            case DataType::VARCHAR:
                join_algorithm.run<std::string>();
                break;
            default:
                throw std::runtime_error("unsupported join type");
            }
        }
        else
        {
            switch (std::get<1>(right_types[join.right_attr]))
            {
            case DataType::INT32:
                join_algorithm.run<int32_t>();
                break;
            case DataType::INT64:
                join_algorithm.run<int64_t>();
                break;
            case DataType::FP64:
                join_algorithm.run<double>();
                break;
            case DataType::VARCHAR:
                join_algorithm.run<std::string>();
                break;
            default:
                throw std::runtime_error("unsupported join type");
            }
        }

        return results;
    }

    // -------------------------------------------------------------------------
    // Scan implementation: convert ColumnarTable input into ExecuteResult.
    // This remains legacy/compatible: it uses Table::from_columnar helper to
    // produce a vector<vector<Data>> for downstream internal operators.
    // -------------------------------------------------------------------------
    ExecuteResult execute_scan(const Plan &plan,
                               const ScanNode &scan,
                               const std::vector<std::tuple<size_t, DataType>> &output_attrs)
    {
        auto table_id = scan.base_table_id;
        auto &input = plan.inputs[table_id];
        auto table = Table::from_columnar(input);
        std::vector<std::vector<Data>> results;
        for (auto &record : table.table())
        {
            std::vector<Data> new_record;
            new_record.reserve(output_attrs.size());
            for (auto [col_idx, _] : output_attrs)
            {
                new_record.emplace_back(record[col_idx]);
            }
            results.emplace_back(std::move(new_record));
        }
        return results;
    }

    // -------------------------------------------------------------------------
    // Core execute_impl dispatch (internal nodes -> produce ExecuteResult).
    // -------------------------------------------------------------------------
    ExecuteResult execute_impl(const Plan &plan, size_t node_idx)
    {
        auto &node = plan.nodes[node_idx];
        return std::visit(
            [&](const auto &value)
            {
                using T = std::decay_t<decltype(value)>;
                if constexpr (std::is_same_v<T, JoinNode>)
                {
                    return execute_hash_join(plan, value, node.output_attrs);
                }
                else
                {
                    return execute_scan(plan, value, node.output_attrs);
                }
            },
            node.data);
    }

    // -------------------------------------------------------------------------
    // Top-level execute() that returns ColumnarTable
    // - If root is a JoinNode: use late-materialization path (direct, no copies)
    // - Otherwise: fallback to legacy ExecuteResult -> Table -> to_columnar
    // -------------------------------------------------------------------------
    ColumnarTable execute(const Plan &plan, [[maybe_unused]] void *context)
    {
        return build_root_columnar_from_plan(plan);
    }

    void *build_context()
    {
        return nullptr;
    }

    void destroy_context([[maybe_unused]] void *context) {}

} // namespace Contest
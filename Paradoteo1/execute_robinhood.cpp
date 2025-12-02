#include <hardware.h>
#include <plan.h>
#include <table.h>
#include <optional>
#include <stdexcept>
#include <vector>
#include <tuple>
#include <string>
#include <variant>
#include <functional>
#include <type_traits>

namespace Contest
{
    using ExecuteResult = std::vector<std::vector<Data>>;

    ExecuteResult execute_impl(const Plan &plan, size_t node_idx);

    struct JoinAlgorithm
    {
        bool build_left;
        ExecuteResult &left;
        ExecuteResult &right;
        ExecuteResult &results;
        size_t left_col, right_col;
        const std::vector<std::tuple<size_t, DataType>> &output_attrs;

        template <typename T>
        void run()
        {
            // A robin-hood hash table bucket
            struct Bucket
            {
                std::optional<T> key;
                size_t psl = 0;              // probe-sequence-length
                std::vector<size_t> indices; // all rows sharing this key
            };

            // Normalize different key types into type T
            auto try_normalize = []<class Key>(const Key &key) -> std::optional<T>
            {
                if constexpr (std::is_same_v<Key, std::monostate>)
                    return std::nullopt;

                if constexpr (std::is_same_v<T, std::string>)
                {
                    if constexpr (std::is_convertible_v<Key, std::string_view>)
                        return std::string(key);
                    else if constexpr (std::is_arithmetic_v<Key>)
                        return std::to_string(key);
                    else
                        return std::nullopt;
                }
                else if constexpr (std::is_integral_v<T> || std::is_floating_point_v<T>)
                {
                    if constexpr (std::is_arithmetic_v<Key>)
                        return static_cast<T>(key);
                    else
                        return std::nullopt;
                }
                else
                {
                    if constexpr (std::is_same_v<Key, T>)
                        return key;
                    else
                        return std::nullopt;
                }
            };

            // Next power of two capacity
            auto next_power_of_two = [](size_t v)
            {
                if (v == 0)
                    return size_t(1);
                --v;
                for (size_t shift = 1; shift < sizeof(size_t) * 8; shift <<= 1)
                    v |= v >> shift;
                return ++v;
            };

            const auto &build_table = build_left ? left : right;
            const auto &probe_table = build_left ? right : left;
            const size_t build_idx_col = build_left ? left_col : right_col;
            const size_t probe_idx_col = build_left ? right_col : left_col;

            // Always pivot by left row width — output schema always left first, then right
            const size_t left_column_count = left.empty() ? 0 : left.front().size();

            size_t cap = next_power_of_two(build_table.size() * 2 + 1);
            std::vector<Bucket> table(cap);
            auto hash_fn = std::hash<T>{};

            // build phase — insert rows into robin-hood hash table
            for (size_t idx = 0; idx < build_table.size(); ++idx)
            {
                const auto &record = build_table[idx];

                // visit the cell that holds the join key (could be a variant)
                std::visit([&](const auto &raw_key)
                           {
                    if (auto nkey = try_normalize(raw_key); nkey.has_value())
                    {
                        T cur_key = *nkey;
                        size_t pos = hash_fn(cur_key) & (cap - 1);
                        size_t psl = 0;
                        std::vector<size_t> cur_indices{ idx };

                        while (true)
                        {
                            auto &b = table[pos];

                            // Empty slot -> place the key and indices
                            if (!b.key.has_value())
                            {
                                b.key = std::move(cur_key);
                                b.indices = std::move(cur_indices);
                                b.psl = psl;
                                break;
                            }

                            // Same key -> append row index
                            if (*b.key == cur_key)
                            {
                                b.indices.push_back(idx);
                                break;
                            }

                            // Robin-Hood: if current bucket has smaller PSL than ours, swap
                            if (b.psl < psl)
                            {
                                std::swap(cur_key, *b.key);
                                std::swap(cur_indices, b.indices);
                                std::swap(psl, b.psl);
                            }

                            // advance
                            pos = (pos + 1) & (cap - 1);
                            ++psl;
                        }
                    } }, record[build_idx_col]);
            }

            // probe phase — find matches
            for (const auto &probe_record : probe_table)
            {
                std::visit([&](const auto &raw_key)
                           {
                    if (auto nkey = try_normalize(raw_key); nkey.has_value())
                    {
                        T key = *nkey;
                        size_t pos = hash_fn(key) & (cap - 1);
                        size_t probe_count = 0;

                        while (true)
                        {
                            auto &b = table[pos];

                            // Empty or too short PSL → stop probing
                            if (!b.key.has_value() || b.psl < probe_count)
                                break;

                            // Match: emit (all) joined rows
                            if (*b.key == key)
                            {
                                for (auto build_idx : b.indices)
                                {
                                    const auto &build_record = build_table[build_idx];

                                    // explicit left/right mapping (fixes failures)
                                    const auto &left_record  = build_left ? build_record : probe_record;
                                    const auto &right_record = build_left ? probe_record : build_record;

                                    std::vector<Data> new_record;
                                    new_record.reserve(output_attrs.size());

                                    // Construct joined output row from output_attrs mapping
                                    for (auto [col_idx, _] : output_attrs)
                                    {
                                        if (col_idx < left_column_count)
                                            new_record.emplace_back(left_record[col_idx]);
                                        else
                                            new_record.emplace_back(right_record[col_idx - left_column_count]);
                                    }

                                    results.emplace_back(std::move(new_record));
                                }
                                break; // we found all entries for this key
                            }

                            pos = (pos + 1) & (cap - 1);
                            ++probe_count;
                        }
                    } }, probe_record[probe_idx_col]);
            }
        }
    };

    ExecuteResult execute_hash_join(const Plan &plan,
                                    const JoinNode &join,
                                    const std::vector<std::tuple<size_t, DataType>> &output_attrs)
    {
        auto left_idx = join.left;
        auto right_idx = join.right;
        auto left = execute_impl(plan, left_idx);
        auto right = execute_impl(plan, right_idx);
        std::vector<std::vector<Data>> results;

        JoinAlgorithm join_algorithm{
            .build_left = join.build_left,
            .left = left,
            .right = right,
            .results = results,
            .left_col = join.left_attr,
            .right_col = join.right_attr,
            .output_attrs = output_attrs};

        // Decide key type from the build side
        const auto &types = join.build_left
                                ? plan.nodes[left_idx].output_attrs
                                : plan.nodes[right_idx].output_attrs;
        size_t key_attr = join.build_left ? join.left_attr : join.right_attr;
        switch (std::get<1>(types[key_attr]))
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
            throw std::runtime_error("unsupported join key type");
        }

        return results;
    }

    ExecuteResult execute_scan(const Plan &plan,
                               const ScanNode &scan,
                               const std::vector<std::tuple<size_t, DataType>> &output_attrs)
    {
        return Table::copy_scan(plan.inputs[scan.base_table_id], output_attrs);
    }

    ExecuteResult execute_impl(const Plan &plan, size_t node_idx)
    {
        const auto &node = plan.nodes[node_idx];
        return std::visit([&](const auto &value) -> ExecuteResult
                          {
            using T = std::decay_t<decltype(value)>;
            if constexpr (std::is_same_v<T, JoinNode>)
                return execute_hash_join(plan, value, node.output_attrs);
            else
                return execute_scan(plan, value, node.output_attrs); }, node.data);
    }

    ColumnarTable execute(const Plan &plan, [[maybe_unused]] void *context)
    {
        // compute row-wise results
        auto rows = execute_impl(plan, plan.root);

        std::vector<DataType> types;
        types.reserve(plan.nodes[plan.root].output_attrs.size());
        for (const auto &attr : plan.nodes[plan.root].output_attrs)
        {
            types.push_back(std::get<1>(attr));
        }
        // construct table and convert to columnar
        Table table(std::move(rows), std::move(types));
        return table.to_columnar();
    }

    void *build_context() { return nullptr; }
    void destroy_context([[maybe_unused]] void *context) {}
}

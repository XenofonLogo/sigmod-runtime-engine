#include <hardware.h>
#include <plan.h>
#include <table.h>
#include <optional>
#include <ranges>

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
            struct Bucket
            {
                std::optional<T> key;
                size_t psl = 0;
                std::vector<size_t> indices;
            };

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

            size_t cap = next_power_of_two(build_table.size() * 2 + 1);
            std::vector<Bucket> table(cap);
            auto hash_fn = std::hash<T>{};

            // ---- Insert keys into hash table ----
            for (size_t idx = 0; idx < build_table.size(); ++idx)
            {
                const auto &record = build_table[idx];
                std::visit([&](const auto &key_val)
                           {
                    using K = std::decay_t<decltype(key_val)>;
                    if constexpr (!std::is_same_v<K, T> && !std::is_same_v<K, std::monostate>)
                        throw std::runtime_error("Build column has wrong type");
                    if constexpr (std::is_same_v<K, T>)
                    {
                        size_t pos = hash_fn(key_val) & (cap - 1);
                        size_t psl = 0;
                        T cur_key = key_val;
                        std::vector<size_t> cur_indices{idx};

                        while (true)
                        {
                            auto &b = table[pos];
                            if (!b.key.has_value())
                            {
                                b.key = std::move(cur_key);
                                b.indices = std::move(cur_indices);
                                b.psl = psl;
                                break;
                            }

                            if (*b.key == cur_key)
                            {
                                b.indices.push_back(idx);
                                break;
                            }

                            if (b.psl < psl)
                            {
                                std::swap(b.key, cur_key);
                                std::swap(b.indices, cur_indices);
                                std::swap(b.psl, psl);
                            }

                            pos = (pos + 1) & (cap - 1);
                            ++psl;
                        }
                    } }, record[build_idx_col]);
            }

            // ---- Probe phase ----
            for (const auto &probe_record : probe_table)
            {
                std::visit([&](const auto &key_val)
                           {
                    using K = std::decay_t<decltype(key_val)>;
                    if constexpr (!std::is_same_v<K, T> && !std::is_same_v<K, std::monostate>)
                        throw std::runtime_error("Probe column has wrong type");
                    if constexpr (std::is_same_v<K, T>)
                    {
                        size_t pos = hash_fn(key_val) & (cap - 1);
                        size_t probe_count = 0;

                        while (true)
                        {
                            auto &b = table[pos];
                            if (!b.key.has_value() || b.psl < probe_count)
                                break;
                            if (*b.key == key_val)
                            {
                                for (auto build_idx : b.indices)
                                {
                                    const auto &build_record = build_table[build_idx];
                                    std::vector<Data> new_record;
                                    new_record.reserve(output_attrs.size());

                                    for (auto [col_idx, _] : output_attrs)
                                    {
                                        if (col_idx < build_record.size())
                                            new_record.emplace_back(build_record[col_idx]);
                                        else
                                            new_record.emplace_back(probe_record[col_idx - build_record.size()]);
                                    }
                                    results.emplace_back(std::move(new_record));
                                }
                                break;
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
        auto &left_node = plan.nodes[left_idx];
        auto &right_node = plan.nodes[right_idx];
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

        auto &types = join.build_left ? left_node.output_attrs : right_node.output_attrs;
        switch (std::get<1>(types[join.build_left ? join.left_attr : join.right_attr]))
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
        return std::visit([&](const auto &value)
                          {
            using T = std::decay_t<decltype(value)>;
            if constexpr (std::is_same_v<T, JoinNode>)
                return execute_hash_join(plan, value, node.output_attrs);
            else
                return execute_scan(plan, value, node.output_attrs); }, node.data);
    }

    ColumnarTable execute(const Plan &plan, [[maybe_unused]] void *context)
    {
        namespace views = ranges::views;
        auto ret = execute_impl(plan, plan.root);
        auto types = plan.nodes[plan.root].output_attrs | views::transform([](auto &v)
                                                                           { return std::get<1>(v); }) |
                     ranges::to<std::vector<DataType>>();
        Table table{std::move(ret), std::move(types)};
        return table.to_columnar();
    }

    void *build_context() { return nullptr; }
    void destroy_context([[maybe_unused]] void *context) {}
}
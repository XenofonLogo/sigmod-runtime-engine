#include <hardware.h>
#include <plan.h>
#include <table.h>

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

        template <class T>
        auto run()
        {
            namespace views = ranges::views;
            // ---- Robin Hood hash table implementation ----
            struct Bucket
            {
                bool occupied = false;
                size_t psl = 0;
                T key{};
                std::vector<size_t> indices; // list of build-side row indices
            };

            auto next_power_of_two = [](size_t v)
            {
                if (v == 0)
                    return size_t(1);
                --v;
                v |= v >> 1;
                v |= v >> 2;
                v |= v >> 4;
                v |= v >> 8;
                v |= v >> 16;
                if (sizeof(size_t) > 4)
                    v |= v >> 32;
                return ++v;
            };

            size_t build_size = build_left ? left.size() : right.size();
            size_t cap = next_power_of_two(build_size * 2 + 1);
            std::vector<Bucket> table(cap);
            auto hash_fn = std::hash<T>{};

            auto rh_insert = [&](const T &key, size_t build_idx)
            {
                size_t h = hash_fn(key);
                size_t pos = h & (cap - 1);
                size_t probe = 0;

                T cur_key = key;
                std::vector<size_t> cur_indices = {build_idx};
                size_t cur_psl = probe;

                while (true)
                {
                    Bucket &b = table[pos];
                    if (!b.occupied)
                    {
                        b.occupied = true;
                        b.key = std::move(cur_key);
                        b.indices = std::move(cur_indices);
                        b.psl = cur_psl;
                        return;
                    }

                    if (b.key == cur_key)
                    {
                        b.indices.push_back(build_idx);
                        return;
                    }

                    if (b.psl < cur_psl)
                    {
                        std::swap(b.key, cur_key);
                        std::swap(b.indices, cur_indices);
                        std::swap(b.psl, cur_psl);
                    }

                    pos = (pos + 1) & (cap - 1);
                    ++cur_psl;
                }
            };

            auto rh_find = [&](const T &key) -> std::vector<size_t> *
            {
                size_t h = hash_fn(key);
                size_t pos = h & (cap - 1);
                size_t probe = 0;
                while (true)
                {
                    Bucket &b = table[pos];
                    if (!b.occupied)
                        return nullptr;
                    if (b.key == key)
                        return &b.indices;
                    if (b.psl < probe)
                        return nullptr;
                    pos = (pos + 1) & (cap - 1);
                    ++probe;
                }
            };

/*            // build & probe phases
            if (build_left)
            {
                for (auto &&[idx, record] : left | views::enumerate)
                {
                    std::visit([&](const auto &key)
                               {
                    using Tk = std::decay_t<decltype(key)>;
                    if constexpr (std::is_same_v<Tk, T>)
                        rh_insert(key, idx);
                    else if constexpr (!std::is_same_v<Tk, std::monostate>)
                        throw std::runtime_error("wrong type of field"); }, record[left_col]);
                }

                for (auto &right_record : right)
                {
                    std::visit([&](const auto &key)
                               {
                    using Tk = std::decay_t<decltype(key)>;
                    if constexpr (std::is_same_v<Tk, T>)
                    {
                        auto *vec_ptr = rh_find(key);
                        if (vec_ptr)
                        {
                            for (auto left_idx : *vec_ptr)
                            {
                                auto &left_record = left[left_idx];
                                std::vector<Data> new_record;
                                new_record.reserve(output_attrs.size());
                                for (auto [col_idx, _] : output_attrs)
                                {
                                    if (col_idx < left_record.size())
                                        new_record.emplace_back(left_record[col_idx]);
                                    else
                                        new_record.emplace_back(right_record[col_idx - left_record.size()]);
                                }
                                results.emplace_back(std::move(new_record));
                            }
                        }
                    }
                    else if constexpr (!std::is_same_v<Tk, std::monostate>)
                        throw std::runtime_error("wrong type of field"); }, right_record[right_col]);
                }
            }
            else
            {
                for (auto &&[idx, record] : right | views::enumerate)
                {
                    std::visit([&](const auto &key)
                               {
                    using Tk = std::decay_t<decltype(key)>;
                    if constexpr (std::is_same_v<Tk, T>)
                        rh_insert(key, idx);
                    else if constexpr (!std::is_same_v<Tk, std::monostate>)
                        throw std::runtime_error("wrong type of field"); }, record[right_col]);
                }

                for (auto &left_record : left)
                {
                    std::visit([&](const auto &key)
                               {
                    using Tk = std::decay_t<decltype(key)>;
                    if constexpr (std::is_same_v<Tk, T>)
                    {
                        auto *vec_ptr = rh_find(key);
                        if (vec_ptr)
                        {
                            for (auto right_idx : *vec_ptr)
                            {
                                auto &right_record = right[right_idx];
                                std::vector<Data> new_record;
                                new_record.reserve(output_attrs.size());
                                for (auto [col_idx, _] : output_attrs)
                                {
                                    if (col_idx < left_record.size())
                                        new_record.emplace_back(left_record[col_idx]);
                                    else
                                        new_record.emplace_back(right_record[col_idx - left_record.size()]);
                                }
                                results.emplace_back(std::move(new_record));
                            }
                        }
                    }
                    else if constexpr (!std::is_same_v<Tk, std::monostate>)
                        throw std::runtime_error("wrong type of field"); }, left_record[left_col]);
                }
            }
        }
    };
    */
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
            }
        }

        return results;
    }

    ExecuteResult execute_scan(const Plan &plan,
                               const ScanNode &scan,
                               const std::vector<std::tuple<size_t, DataType>> &output_attrs)
    {
        auto table_id = scan.base_table_id;
        auto &input = plan.inputs[table_id];
        return Table::copy_scan(input, output_attrs);
    }

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

    ColumnarTable execute(const Plan &plan, [[maybe_unused]] void *context)
    {
        namespace views = ranges::views;
        auto ret = execute_impl(plan, plan.root);
        auto ret_types = plan.nodes[plan.root].output_attrs | views::transform([](const auto &v)
                                                                               { return std::get<1>(v); }) |
                         ranges::to<std::vector<DataType>>();
        Table table{std::move(ret), std::move(ret_types)};
        return table.to_columnar();
    }

    void *build_context()
    {
        return nullptr;
    }

    void destroy_context([[maybe_unused]] void *context) {}

} // namespace Contest

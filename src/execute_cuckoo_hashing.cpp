#include <plan.h>
#include <table.h>

namespace Contest {

using ExecuteResult = std::vector<std::vector<Data>>;

ExecuteResult execute_impl(const Plan& plan, size_t node_idx);

struct CuckooMapData {
    using V = std::vector<size_t>; // Vector of row indices
    size_t capacity = 0;           // Current hash table capacity
    std::vector<char> occupied;    // Marks if a slot is used
    std::vector<Data> keys;        // Keys stored in hash table
    std::vector<V> vals;           // Lists of matching row indices

    CuckooMapData() = default;

    // Compute hash value for various Data types
    static size_t hash_data(const Data& d) {
        return std::visit(
            [](const auto& v) -> size_t {
                using T = std::decay_t<decltype(v)>;
                if constexpr (std::is_same_v<T, int32_t>) {
                    return std::hash<int32_t>{}(v);
                } else if constexpr (std::is_same_v<T, int64_t>) {
                    return std::hash<int64_t>{}(v);
                } else if constexpr (std::is_same_v<T, double>) {
                    return std::hash<double>{}(v);
                } else if constexpr (std::is_same_v<T, std::string>) {
                    return std::hash<std::string>{}(v);
                } else {
                    return 0;
                }
            },
            d);
    }

    // Initialize table capacity
    void reset(size_t cap) {
        capacity = 1;
        while (capacity < cap) capacity <<= 1;
        occupied.assign(capacity, 0);
        keys.assign(capacity, Data{});
        vals.clear();
        vals.resize(capacity);
    }

    size_t h1_hash(const Data& k) const { return hash_data(k) & (capacity - 1); }
    size_t h2_hash(const Data& k) const {
        auto x = hash_data(k);
        x ^= (x >> 33) ^ 0x9e3779b97f4a7c15ULL; // Mix bits for better distribution
        return x & (capacity - 1);
    }

    // Find matching key and return pointer to its value list
    V* find(const Data& k) {
        size_t p1 = h1_hash(k);
        if (occupied[p1] && keys[p1] == k) return &vals[p1];
        size_t p2 = h2_hash(k);
        if (occupied[p2] && keys[p2] == k) return &vals[p2];
        return nullptr;
    }

    // Double table size and reinsert all entries
    void rehash_grow() {
        std::vector<std::pair<Data, V>> entries;
        for (size_t i = 0; i < capacity; ++i) {
            if (occupied[i]) entries.emplace_back(keys[i], std::move(vals[i]));
        }
        reset(capacity * 2);
        for (auto &e: entries) {
            insert_bulk(std::move(e.first), std::move(e.second));
        }
    }

    // Insert key and vector of row indices with cuckoo displacement
    void insert_bulk(Data&& k, V&& v) {
        size_t p1 = h1_hash(k);
        if (!occupied[p1]) {
            keys[p1] = std::move(k);
            vals[p1] = std::move(v);
            occupied[p1] = 1;
            return;
        }
        if (keys[p1] == k) {
            auto& vec = vals[p1];
            vec.insert(vec.end(), v.begin(), v.end());
            return;
        }
        size_t p2 = h2_hash(k);
        if (!occupied[p2]) {
            keys[p2] = std::move(k);
            vals[p2] = std::move(v);
            occupied[p2] = 1;
            return;
        }
        if (keys[p2] == k) {
            auto& vec = vals[p2];
            vec.insert(vec.end(), v.begin(), v.end());
            return;
        }

        Data curk = std::move(k);
        V curv = std::move(v);
        size_t pos = p1;
        const int MAX_KICKS = 512;
        for (int kick = 0; kick < MAX_KICKS; ++kick) {
            if (!occupied[pos]) {
                keys[pos] = std::move(curk);
                vals[pos] = std::move(curv);
                occupied[pos] = 1;
                return;
            }
            if (keys[pos] == curk) {
                vals[pos].insert(vals[pos].end(), curv.begin(), curv.end());
                return;
            }
            std::swap(keys[pos], curk);
            std::swap(vals[pos], curv);
            size_t a1 = h1_hash(curk);
            size_t a2 = h2_hash(curk);
            pos = (pos == a1) ? a2 : a1; // Alternate between hash positions
        }
        rehash_grow(); // Grow table if too many displacements
        insert_bulk(std::move(curk), std::move(curv));
    }

    // Insert single index associated with a key
    void insert(const Data& k, size_t idx) {
        if (std::get_if<std::monostate>(&k)) {
            return; // Treat NULL as non-joinable
        }
        if (auto p = find(k)) { p->push_back(idx); return; }
        size_t p = h1_hash(k);
        const int MAX_KICKS = 512;
        Data curk = k;
        V curv; curv.push_back(idx);
        for (int kick = 0; kick < MAX_KICKS; ++kick) {
            if (!occupied[p]) {
                keys[p] = curk;
                vals[p] = std::move(curv);
                occupied[p] = 1;
                return;
            }
            if (keys[p] == curk) {
                vals[p].push_back(idx);
                return;
            }
            std::swap(keys[p], curk);
            std::swap(vals[p], curv);
            size_t a1 = h1_hash(curk);
            size_t a2 = h2_hash(curk);
            p = (p == a1) ? a2 : a1;
        }
        rehash_grow();
        insert_bulk(std::move(curk), std::move(curv));
    }
};

ExecuteResult execute_hash_join(const Plan&          plan,
    const JoinNode&                                  join,
    const std::vector<std::tuple<size_t, DataType>>& output_attrs) {
    auto left_idx    = join.left;
    auto right_idx   = join.right;
    auto& left_node   = plan.nodes[left_idx];
    auto& right_node  = plan.nodes[right_idx];
    auto& left_types  = left_node.output_attrs;
    auto& right_types = right_node.output_attrs;
    auto left        = execute_impl(plan, left_idx);
    auto right       = execute_impl(plan, right_idx);
    std::vector<std::vector<Data>> results;

    // Choose build and probe sides based on join.build_left flag
    const auto& build_rows = join.build_left ? left : right;
    const auto& probe_rows = join.build_left ? right : left;
    size_t build_col = join.build_left ? join.left_attr : join.right_attr;
    size_t probe_col = join.build_left ? join.right_attr : join.left_attr;

    CuckooMapData cmap;
    cmap.reset(std::max<size_t>(16, build_rows.size() * 2 + 1)); // Initialize hash map

    // Merge two records into one output row
    auto merge_records = [&](const std::vector<Data>& lrec, const std::vector<Data>& rrec) {
        std::vector<Data> new_record;
        new_record.reserve(output_attrs.size());
        size_t left_width = left.front().size();
        for (auto [col_idx, _]: output_attrs) {
            if (col_idx < left_width) new_record.emplace_back(lrec[col_idx]);
            else new_record.emplace_back(rrec[col_idx - left_width]);
        }
        results.emplace_back(std::move(new_record));
    };

    auto do_for_type = [&](auto dummy) {
        using K = decltype(dummy);
        // Build phase: insert build side keys
        for (size_t i = 0; i < build_rows.size(); ++i) {
            const auto& rec = build_rows[i];
            const Data& key = rec[build_col];
            if (std::get_if<std::monostate>(&key)) continue;
            std::visit([&](const auto& kf) {
                using Tk = std::decay_t<decltype(kf)>;
                if constexpr (std::is_same_v<Tk, K>) {
                    Data norm_key = Data(K(kf));
                    cmap.insert(norm_key, i);
                } else if constexpr (not std::is_same_v<Tk, std::monostate>) {
                    throw std::runtime_error("wrong type of field");
                }
            }, key);
        }
        // Probe phase: find matching keys and merge rows
        for (size_t j = 0; j < probe_rows.size(); ++j) {
            const auto& prec = probe_rows[j];
            const Data& key = prec[probe_col];
            if (std::get_if<std::monostate>(&key)) continue;
            std::visit([&](const auto& kf) {
                using Tk = std::decay_t<decltype(kf)>;
                if constexpr (std::is_same_v<Tk, K>) {
                    Data norm_key = Data(K(kf));
                    if (auto vec = cmap.find(norm_key)) {
                        for (auto bi: *vec) {
                            if (join.build_left) merge_records(build_rows[bi], probe_rows[j]);
                            else merge_records(probe_rows[j], build_rows[bi]);
                        }
                    }
                } else if constexpr (not std::is_same_v<Tk, std::monostate>) {
                    throw std::runtime_error("wrong type of field");
                }
            }, key);
        }
    };

    // Dispatch join by key data type
    const auto& build_node = plan.nodes[join.build_left ? join.left : join.right];
    auto build_type = std::get<1>(build_node.output_attrs[ build_col ]);
    switch (build_type) {
    case DataType::INT32:   do_for_type(int32_t{}); break;
    case DataType::INT64:   do_for_type(int64_t{}); break;
    case DataType::FP64:    do_for_type(double{}); break;
    case DataType::VARCHAR: do_for_type(std::string{}); break;
    }

    return results;
}

ExecuteResult execute_scan(const Plan&               plan,
    const ScanNode&                                  scan,
    const std::vector<std::tuple<size_t, DataType>>& output_attrs) {
    auto table_id = scan.base_table_id;
    auto& input   = plan.inputs[table_id];
    auto table    = Table::from_columnar(input);
    std::vector<std::vector<Data>> results;
    for (auto& record: table.table()) {
        std::vector<Data> new_record;
        new_record.reserve(output_attrs.size());
        for (auto [col_idx, _]: output_attrs) {
            new_record.emplace_back(record[col_idx]);
        }
        results.emplace_back(std::move(new_record));
    }
    return results;
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

ColumnarTable execute(const Plan& plan, [[maybe_unused]] void* context) {
    auto rows = execute_impl(plan, plan.root);
    namespace views = ranges::views;
    std::sort(rows.begin(), rows.end()); // Sort result rows
    auto ret_types = plan.nodes[plan.root].output_attrs
                     | views::transform([](const auto& v) { return std::get<1>(v); })
                     | ranges::to<std::vector<DataType>>();
    Table table{std::move(rows), std::move(ret_types)};
    return table.to_columnar(); // Convert back to columnar format
}

void* build_context() {
    return nullptr;
}

void destroy_context([[maybe_unused]] void* context) {}

} // namespace Contest

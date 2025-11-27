#ifndef CUCKOO_MAP_H
#define CUCKOO_MAP_H

#include <vector>
#include <functional>
#include <optional>
#include <random>
#include <utility>
#include <stdexcept>

template <typename Key, typename Value, typename Hash = std::hash<Key>>
class CuckooMap {
private:
    struct Bucket {
        Key key;
        Value value;
        bool occupied = false;
    };

    std::vector<Bucket> table1;
    std::vector<Bucket> table2;
    size_t table_size;
    size_t num_elements = 0;
    Hash hasher;
    std::mt19937 gen{std::random_device{}()};
    static constexpr int max_iterations = 100;

    size_t hash1(const Key& key) const {
        return hasher(key) % table_size;
    }

    size_t hash2(const Key& key) const {
        return (hasher(key) >> 16) % table_size;
    }

    // Attempt insertion and swap displaced elements into parameters on failure.
    bool insert_core(Key& key, Value& value) {
        for (int i = 0; i < max_iterations; ++i) {
            size_t h1 = hash1(key);
            if (!table1[h1].occupied) {
                table1[h1] = {std::move(key), std::move(value), true};
                num_elements++;
                return true;
            }
            std::swap(key, table1[h1].key);
            std::swap(value, table1[h1].value);

            size_t h2 = hash2(key);
            if (!table2[h2].occupied) {
                table2[h2] = {std::move(key), std::move(value), true};
                num_elements++;
                return true;
            }
            std::swap(key, table2[h2].key);
            std::swap(value, table2[h2].value);
        }
        // Parameters now hold the displaced element after insertion failure.
        return false; 
    }

    void rehash() {
        std::vector<std::pair<Key, Value>> all_elements;
        all_elements.reserve(num_elements);
        for (const auto& bucket : table1) {
            if (bucket.occupied) {
                all_elements.push_back({bucket.key, bucket.value});
            }
        }
        for (const auto& bucket : table2) {
            if (bucket.occupied) {
                all_elements.push_back({bucket.key, bucket.value});
            }
        }

        int attempts = 0;
        while (true) {
            // Limit rehash attempts to prevent infinite loops.
            if (++attempts > 5) { 
                 throw std::runtime_error("CuckooMap: Failed to rehash. Check hash functions.");
            }

            table_size *= 2;
            table1.assign(table_size, Bucket{});
            table2.assign(table_size, Bucket{});
            num_elements = 0;

            bool rehash_success = true;
            for (auto& pair : all_elements) {
                Key k = std::move(pair.first);
                Value v = std::move(pair.second);
                if (!insert_core(k, v)) {
                    // Expand tables and retry if insertion fails during rehash.
                    rehash_success = false;
                    break;
                }
            }

            if (rehash_success) {
                return;
            }
        }
    }

public:
    CuckooMap(size_t size = 1024) : table_size(size) {
        table1.resize(table_size);
        table2.resize(table_size);
    }

    bool find(const Key& key, Value& value) {
        size_t h1 = hash1(key);
        if (table1[h1].occupied && table1[h1].key == key) {
            value = table1[h1].value;
            return true;
        }

        size_t h2 = hash2(key);
        if (table2[h2].occupied && table2[h2].key == key) {
            value = table2[h2].value;
            return true;
        }

        return false;
    }

    bool insert(Key key, Value value) {
        // Trigger rehash when the load factor reaches 50%.
        if (num_elements >= table_size) { 
            rehash();
        }

        Key current_key = std::move(key);
        Value current_value = std::move(value);

        if (insert_core(current_key, current_value)) {
            return true;
        }

        // Rehash and retry insertion with the displaced element upon failure.
        while (true) {
            rehash();
            if (insert_core(current_key, current_value)) {
                return true;
            }
        }
    }

    void upsert(const Key& key, Value value) {
        size_t h1 = hash1(key);
        if (table1[h1].occupied && table1[h1].key == key) {
            table1[h1].value = std::move(value);
            return;
        }

        size_t h2 = hash2(key);
        if (table2[h2].occupied && table2[h2].key == key) {
            table2[h2].value = std::move(value);
            return;
        }

        insert(key, std::move(value));
    }
};

#endif // CUCKOO_MAP_H

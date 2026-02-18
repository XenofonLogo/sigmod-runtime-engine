#pragma once

#include <vector>
#include <map>
#include <algorithm>
#include <cstdint>
#include <functional>
#include <cmath>
#include <utility>
#include <stdexcept>

#include "hash_common.h" 
#include "hashtable_interface.h" 
namespace Contest {



template<typename Key>
class CuckooBackend {
private:
    // Use common structures via alias
    using Entry = HashEntry<Key>;
    using IndexInfo = KeyIndexInfo<Key>;

    // Storage array for all entries (Key-to-Many logic)
    std::vector<Entry> _storage;

    // The two hash tables
    std::vector<IndexInfo> _table1;
    std::vector<IndexInfo> _table2;
    size_t _capacity = 0; // Size of each table
    size_t _initial_capacity = 0; // Initial capacity for reuse

    // Cycle check limit during insertion
    static constexpr size_t MAX_DISPLACEMENTS = 200; 
    // Maximum number of consecutive rehash attempts
    static constexpr size_t MAX_REHASH_ATTEMPTS = 5; 
    
    // ----------------- Hash Functions -----------------
    
    // Hash Function 1: basic std::hash
    size_t hash_fn1(const Key& k) const {
        return std::hash<Key>{}(k) % _capacity;
    }

    // Hash Function 2: alternative hash (uses different seed/mixing)
    size_t hash_fn2(const Key& k) const {
        // Simple mixing for the second hash function
        size_t h = std::hash<Key>{}(k);
        // Use a different multiplication factor for mixing
        return ((h * 0x9e3779b9) ^ (h >> 16)) % _capacity; 
    }

    // Helper function to find the two possible positions
    size_t find_home1(const Key& k) const {
        if (_capacity == 0) return 0;
        return hash_fn1(k);
    }

    size_t find_home2(const Key& k) const {
        if (_capacity == 0) return 0;
        return hash_fn2(k);
    }

    // ----------------- Cuckoo Insertion Logic -----------------

    /**
     * @brief Inserts an IndexInfo into one of the two tables.
     * Handles displacements (kicks) and cycle detection.
     * @param info The IndexInfo to insert.
     * @return true If the insertion succeeded (an empty slot was found).
     * @return false If a cycle was detected and a rehash is required.
     */
    bool insert_key_info(IndexInfo info) {
        IndexInfo current_info = info;
        // Attempt insertion for up to MAX_DISPLACEMENTS steps
        for (size_t displacements = 0; displacements < MAX_DISPLACEMENTS; ++displacements) {
            
            // --- Table 1: always try first ---
            size_t pos1 = find_home1(current_info.key);
            if (!_table1[pos1].is_valid) {
                _table1[pos1] = current_info;
                _table1[pos1].is_valid = true;
                return true; // Success!
            }
            // Displacement: swap current_info with the element at pos1
            std::swap(current_info, _table1[pos1]);


            // --- Table 2: alternative position ---
            size_t pos2 = find_home2(current_info.key);
            if (!_table2[pos2].is_valid) {
                _table2[pos2] = current_info;
                _table2[pos2].is_valid = true;
                return true; // Success!
            }
            // Displacement: swap current_info with the element at pos2
            std::swap(current_info, _table2[pos2]);

            // The displaced element is now in current_info
            // and continues in the next round (again trying Table 1 first).
        }
        
        // If the displacement limit is exceeded, we consider a cycle detected.
        return false; // Failure: Rehash required
    }

    /**
     * @brief Doubles the table size and reinitializes.
     * @param current_capacity The current capacity.
     */
    void prepare_rehash(size_t current_capacity) {
        // 1. Double capacity
        _capacity = current_capacity * 2 + 1; 
        
        // 2. Reinitialize tables (empty tables)
        _table1.assign(_capacity, IndexInfo{});
        _table2.assign(_capacity, IndexInfo{});
    }

public:
    // ------------------------------------------------------------
    // 3. Executor Interface
    // ------------------------------------------------------------

    CuckooBackend() = default;

    /**
     * @brief Builds the Cuckoo Hash Table from the build-side entries.
     */
    void build_from_entries(const std::vector<std::pair<Key, size_t>>& entries) {
        if (entries.empty()) {
            _capacity = 0;
            _table1.clear();
            _table2.clear();
            _storage.clear();
            return;
        }

        // 1. Preprocessing: group entries and build _storage
        // (This is done only once, regardless of rehash attempts)
        std::map<Key, std::vector<size_t>> grouped_entries;
        for (const auto& p : entries) {
            grouped_entries[p.first].push_back(p.second);
        }

        _storage.reserve(entries.size());
        size_t current_storage_index = 0;
        
        std::vector<IndexInfo> index_infos_to_insert;
        index_infos_to_insert.reserve(grouped_entries.size());

        for (const auto& pair : grouped_entries) {
            const Key& key = pair.first;
            const std::vector<size_t>& row_ids = pair.second;

            IndexInfo info;
            info.key = key;
            info.start_index = current_storage_index;
            info.count = row_ids.size();
            info.is_valid = true;
            index_infos_to_insert.push_back(info);

            for (size_t row_id : row_ids) {
                _storage.emplace_back(Entry{key, static_cast<uint32_t>(row_id)});
            }
            current_storage_index += row_ids.size();
        }

        // 2. Initial capacity
        double load_factor = 0.45; 
        _initial_capacity = static_cast<size_t>(std::ceil(index_infos_to_insert.size() / load_factor));
        if (_initial_capacity == 0) _initial_capacity = 1;

        // 3. Insertion loop with rehash handling
        size_t current_rehash_attempt = 0;
        
        while (current_rehash_attempt < MAX_REHASH_ATTEMPTS) {
            
            // Compute new capacity
            if (current_rehash_attempt == 0) {
                _capacity = _initial_capacity;
            } else {
                // Double the current capacity (not the initial)
                _capacity = _capacity * 2 + 1;
            }
            
            // Prepare tables with the new capacity
            _table1.assign(_capacity, IndexInfo{});
            _table2.assign(_capacity, IndexInfo{});
            
            bool insertion_successful = true;
            for (const auto& info : index_infos_to_insert) {
                if (!insert_key_info(info)) {
                        // Insertion failure (cycle): Rehash required
                    insertion_successful = false;
                    break;
                }
            }
            
            if (insertion_successful) {
                return; // Successful build
            }
            
            current_rehash_attempt++;
        }
        
        // If MAX_REHASH_ATTEMPTS is exceeded
        throw std::runtime_error("Cuckoo Hashing failed to find a valid placement after multiple rehash attempts.");
    }

    /**
     * @brief Performs a probe search for a key.
     */
    std::pair<const Entry*, size_t> probe(const Key& k) const {
        if (_capacity == 0) {
            return {nullptr, 0};
        }

        // 1. Check Table 1
        size_t pos1 = find_home1(k);
        const IndexInfo& info1 = _table1[pos1];
        if (info1.is_valid && info1.key == k) {
            return {&_storage[info1.start_index], info1.count};
        }

        // 2. Check Table 2
        size_t pos2 = find_home2(k);
        const IndexInfo& info2 = _table2[pos2];
        if (info2.is_valid && info2.key == k) {
            return {&_storage[info2.start_index], info2.count};
        }

        // 3. Not found
        return {nullptr, 0};
    }
};
}
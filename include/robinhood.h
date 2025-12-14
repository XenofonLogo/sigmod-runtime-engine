#pragma once

#include <vector>
#include <map>
#include <algorithm>
#include <cstdint>
#include <functional>
#include <cmath>
#include <utility> 
#include <stdexcept> 
#include "hashtable_interface.h"
#include "hash_common.h"
#include <unordered_map>

namespace Contest {

template<typename Key>
class RobinHoodBackend {
private:
    using Entry = HashEntry<Key>;
    using IndexInfo = KeyIndexInfo<Key>;

    std::vector<Entry> _storage;
    std::vector<IndexInfo> _table;
    size_t _capacity = 0;
    size_t _initial_capacity = 0;
    static constexpr size_t MAX_REHASH_ATTEMPTS = 5;

    // Συνάρτηση κατακερματισμού (hash function)
    size_t hash_fn(const Key& k) const {
        // Χρησιμοποιούμε μια βελτιωμένη συνάρτηση hash (π.χ. FNV-1a ή παρόμοια ανάμειξη)
        // για καλύτερη διασπορά και μείωση των συγκρούσεων (clustering).
        // FNV-1a style prime/xor mixing (optimised for integer keys)
        uint32_t hash = 2166136261U;
        // FNV_PRIME_32
        hash ^= static_cast<uint32_t>(k);
        hash *= 16777619U;
        // FNV_PRIME_32

        // Εξασφαλίζουμε ότι το αποτέλεσμα είναι εντός της χωρητικότητας
        return static_cast<size_t>(hash) % _capacity;
    }

    // Υπολογίζει το Probe Sequence Length (PSL)
    
    size_t psl(size_t current_slot_idx, size_t home_slot_idx) const {
        // Υπολογίζει την απόσταση (στην κυκλική δομή)
        if (current_slot_idx >= home_slot_idx) {
            return current_slot_idx - home_slot_idx;
        } else {
            // Κυκλική απόσταση
            return current_slot_idx + (_capacity - home_slot_idx);
        }
    }

    // Βοηθητική συνάρτηση για την εύρεση της αρχικής θέσης (home slot)
    size_t find_home(const Key& k) const {
        if (_capacity == 0) return 0;
        return hash_fn(k);
    }

    // Εισαγωγή ενός στοιχείου στον πίνακα κατακερματισμού με Robin Hood displacement
    // RETURNS: true on success, false if insertion fails (table full/cycle)
    bool insert_index_info(const IndexInfo& info) {
        size_t home = find_home(info.key);
        size_t current_slot = home;
        IndexInfo current_info = info;
        size_t attempts = 0;
        while (_table[current_slot].is_valid) {
            if (attempts >= _capacity) {
                return false;
                // Insertion failed, need to rehash
            }

            size_t insert_psl = psl(current_slot, find_home(current_info.key));
            size_t displaced_psl = psl(current_slot, find_home(_table[current_slot].key));

            if (insert_psl > displaced_psl) { // Robin Hood: Ανταλλαγή
                std::swap(current_info, _table[current_slot]);
            }

            current_slot = (current_slot + 1) % _capacity;
            attempts++;
        }

        _table[current_slot] = current_info;
        _table[current_slot].is_valid = true;
        return true; // Insertion successful
    }

public:
    
    // 3. Διεπαφή Executor
    

    RobinHoodBackend() = default;
  
     // Κατασκευάζει το Robin Hood Hash Table από τις εγγραφές της build side.
     
    void build_from_entries(const std::vector<std::pair<Key, size_t>>& entries) {
        if (entries.empty()) {
             _capacity = 0;
             _table.clear(); _storage.clear();
             return;
        }

        // 1. Προεπεξεργασία: Ομαδοποίηση εγγραφών
        std::map<Key, std::vector<size_t>> grouped_entries;
        for (const auto& p : entries) {
            grouped_entries[p.first].push_back(p.second);
        }

        // 2. Δημιουργία της _storage και του vector IndexInfo
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
                _storage.emplace_back(Entry{key, row_id});
            }
            current_storage_index += row_ids.size();
        }

        // 3. Ρυθμίσεις αρχικής χωρητικότητας
        double load_factor = 0.75;
        _initial_capacity = static_cast<size_t>(std::ceil(index_infos_to_insert.size() / load_factor));
        if (_initial_capacity == 0) _initial_capacity = 1;

        _capacity = _initial_capacity;

        size_t current_rehash_attempt = 0;
        // 4. Βρόχος εισαγωγής με χειρισμό Rehash
        while (current_rehash_attempt < MAX_REHASH_ATTEMPTS) {

            // Re-calculate capacity: exponential growth
            if (current_rehash_attempt > 0) {
                 _capacity = _capacity * 2 + 1;
            }

            _table.assign(_capacity, IndexInfo{});
            // Clear and resize table

            bool insertion_successful = true;
            for (const auto& info : index_infos_to_insert) {
                if (!insert_index_info(info)) {
                    insertion_successful = false;
                    break;
                }
            }

            if (insertion_successful) {
                return;
                // SUCCESS!
            }

            current_rehash_attempt++;
        }

        throw std::runtime_error("Robin Hood Hashing failed to find a valid placement after multiple rehash attempts.");
    }

    
     // Πραγματοποιεί αναζήτηση (probe) για ένα κλειδί.
    
    std::pair<const Entry*, size_t> probe(const Key& k) const {
        if (_capacity == 0 || _table.empty()) {
            return {nullptr, 0};
        }

        size_t home = find_home(k);
        size_t current_slot = home;

        size_t k_psl;
        do {
            if (!_table[current_slot].is_valid) {
                return {nullptr, 0};
            }

            k_psl = psl(current_slot, home);
            size_t displaced_psl = psl(current_slot, find_home(_table[current_slot].key));

            if (k_psl > displaced_psl) {
                return {nullptr, 0};
            }

            if (_table[current_slot].key == k) {
                return {
                    &_storage[_table[current_slot].start_index],
                    _table[current_slot].count
                };
            }

            current_slot = (current_slot + 1) % _capacity;
        } while (current_slot != home);

        return {nullptr, 0};
    }
};

} // namespace Contest
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
    // Χρήση των κοινών δομών με alias
    using Entry = HashEntry<Key>;
    using IndexInfo = KeyIndexInfo<Key>;

    // Πίνακας για την αποθήκευση όλων των εγγραφών (Key-to-Many logic)
    std::vector<Entry> _storage;

    // Οι δύο πίνακες κατακερματισμού
    std::vector<IndexInfo> _table1;
    std::vector<IndexInfo> _table2;
    size_t _capacity = 0; // Μέγεθος κάθε πίνακα
    size_t _initial_capacity = 0; // Αρχική χωρητικότητα για επαναχρησιμοποίηση

    // Όριο για τον έλεγχο κύκλου κατά την εισαγωγή
    static constexpr size_t MAX_DISPLACEMENTS = 200; 
    // Μέγιστος αριθμός διαδοχικών προσπαθειών rehash
    static constexpr size_t MAX_REHASH_ATTEMPTS = 5; 
    
    // ----------------- Hash Functions -----------------
    
    // Hash Function 1: Βασική std::hash
    size_t hash_fn1(const Key& k) const {
        return std::hash<Key>{}(k) % _capacity;
    }

    // Hash Function 2: Εναλλακτική hash (χρησιμοποιεί διαφορετικό seed/ανάμειξη)
    size_t hash_fn2(const Key& k) const {
        // Απλή ανάμειξη για δεύτερη hash function
        size_t h = std::hash<Key>{}(k);
        // Χρησιμοποιούμε έναν διαφορετικό παράγοντα πολλαπλασιασμού
        return ((h * 0x9e3779b9) ^ (h >> 16)) % _capacity; 
    }

    // Βοηθητική συνάρτηση για την εύρεση των δύο πιθανών θέσεων
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
     * @brief Εισάγει ένα IndexInfo σε έναν από τους δύο πίνακες.
     * Χειρίζεται τις μετατοπίσεις (kicks) και τον έλεγχο κύκλου.
     * * @param info Το IndexInfo προς εισαγωγή.
     * @return true Εάν η εισαγωγή ήταν επιτυχής (βρέθηκε κενό slot).
     * @return false Εάν βρέθηκε κύκλος και απαιτείται rehash.
     */
    bool insert_key_info(IndexInfo info) {
        IndexInfo current_info = info;
        // Προσπαθούμε να εισάγουμε για MAX_DISPLACEMENTS βήματα
        for (size_t displacements = 0; displacements < MAX_DISPLACEMENTS; ++displacements) {
            
            // --- Πίνακας 1: Πάντα η πρώτη προσπάθεια ---
            size_t pos1 = find_home1(current_info.key);
            if (!_table1[pos1].is_valid) {
                _table1[pos1] = current_info;
                _table1[pos1].is_valid = true;
                return true; // Επιτυχία!
            }
            // Εκτόπιση: Swap current_info με το στοιχείο που βρίσκεται στη pos1
            std::swap(current_info, _table1[pos1]);


            // --- Πίνακας 2: Εναλλακτική θέση ---
            size_t pos2 = find_home2(current_info.key);
            if (!_table2[pos2].is_valid) {
                _table2[pos2] = current_info;
                _table2[pos2].is_valid = true;
                return true; // Επιτυχία!
            }
            // Εκτόπιση: Swap current_info με το στοιχείο που βρίσκεται στη pos2
            std::swap(current_info, _table2[pos2]);

            // Το στοιχείο που εκτοπίστηκε τώρα βρίσκεται στο current_info
            // και συνεχίζει στον επόμενο γύρο (πάλι προσπαθώντας πρώτα στον Πίνακα 1).
        }
        
        // Αν ξεπεραστεί το όριο μετατοπίσεων, θεωρούμε ότι υπάρχει κύκλος.
        return false; // Αποτυχία: Χρειάζεται Rehash
    }

    /**
     * @brief Διπλασιάζει το μέγεθος του πίνακα και επαναρχικοποιεί.
     * @param current_capacity Η τρέχουσα χωρητικότητα.
     */
    void prepare_rehash(size_t current_capacity) {
        // 1. Διπλασιασμός χωρητικότητας
        _capacity = current_capacity * 2 + 1; 
        
        // 2. Επαναρχικοποίηση πινάκων (καθαροί πίνακες)
        _table1.assign(_capacity, IndexInfo{});
        _table2.assign(_capacity, IndexInfo{});
    }

public:
    // ------------------------------------------------------------
    // 3. Διεπαφή Executor
    // ------------------------------------------------------------

    CuckooBackend() = default;

    /**
     * @brief Κατασκευάζει το Cuckoo Hash Table από τις εγγραφές της build side.
     */
    void build_from_entries(const std::vector<std::pair<Key, size_t>>& entries) {
        if (entries.empty()) {
            _capacity = 0;
            _table1.clear();
            _table2.clear();
            _storage.clear();
            return;
        }

        // 1. Προεπεξεργασία: Ομαδοποίηση εγγραφών και δημιουργία _storage
        // (Αυτό γίνεται μόνο μια φορά, ανεξάρτητα από τα rehash attempts)
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
                _storage.emplace_back(Entry{key, row_id});
            }
            current_storage_index += row_ids.size();
        }

        // 2. Αρχική χωρητικότητα
        double load_factor = 0.45; 
        _initial_capacity = static_cast<size_t>(std::ceil(index_infos_to_insert.size() / load_factor));
        if (_initial_capacity == 0) _initial_capacity = 1;

        // 3. Βρόχος εισαγωγής με χειρισμό Rehash
        size_t current_rehash_attempt = 0;
        
        while (current_rehash_attempt < MAX_REHASH_ATTEMPTS) {
            
            // Υπολογισμός νέας χωρητικότητας
            if (current_rehash_attempt == 0) {
                _capacity = _initial_capacity;
            } else {
                // Διπλασιασμός της τρέχουσας χωρητικότητας (όχι της αρχικής)
                _capacity = _capacity * 2 + 1;
            }
            
            // Προετοιμασία πινάκων με νέα χωρητικότητα
            _table1.assign(_capacity, IndexInfo{});
            _table2.assign(_capacity, IndexInfo{});
            
            bool insertion_successful = true;
            for (const auto& info : index_infos_to_insert) {
                if (!insert_key_info(info)) {
                    // Αποτυχία εισαγωγής (κύκλος): Χρειάζεται Rehash
                    insertion_successful = false;
                    break;
                }
            }
            
            if (insertion_successful) {
                return; // Επιτυχής κατασκευή
            }
            
            current_rehash_attempt++;
        }
        
        // Αν ξεπεραστεί το MAX_REHASH_ATTEMPTS
        throw std::runtime_error("Cuckoo Hashing failed to find a valid placement after multiple rehash attempts.");
    }

    /**
     * @brief Πραγματοποιεί αναζήτηση (probe) για ένα κλειδί.
     */
    std::pair<const Entry*, size_t> probe(const Key& k) const {
        if (_capacity == 0) {
            return {nullptr, 0};
        }

        // 1. Έλεγχος Πίνακα 1
        size_t pos1 = find_home1(k);
        const IndexInfo& info1 = _table1[pos1];
        if (info1.is_valid && info1.key == k) {
            return {&_storage[info1.start_index], info1.count};
        }

        // 2. Έλεγχος Πίνακα 2
        size_t pos2 = find_home2(k);
        const IndexInfo& info2 = _table2[pos2];
        if (info2.is_valid && info2.key == k) {
            return {&_storage[info2.start_index], info2.count};
        }

        // 3. Δεν βρέθηκε
        return {nullptr, 0};
    }
};
}
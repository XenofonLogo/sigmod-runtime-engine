
// Υλοποίηση του Robin Hood Hash Table για χρήση στον Hash Join Executor.
// Χρησιμοποιεί ανοικτή διευθυνσιοδότηση (open addressing).
// ------------------------------------------------------------

#pragma once

#include <vector>
#include <map>
#include <algorithm>
#include <cstdint>
#include <functional>
#include <cmath>
#include <utility> // For std::pair
 #include "hashtable_interface.h"
#include "hash_common.h" // NOW CONTAINS HashEntry and KeyIndexInfo

namespace Contest {

template<typename Key>
class RobinHoodBackend {
private:
    // This line now correctly pulls the definition from hash_common.h
    using Entry = HashEntry<Key>;
    // This line now correctly pulls the definition from hash_common.h
    using IndexInfo = KeyIndexInfo<Key>;
    // Πίνακας για την αποθήκευση όλων των εγγραφών της build side, ομαδοποιημένες
    // ανά κλειδί.
    std::vector<Entry> _storage;

    // Ο πίνακας κατακερματισμού (hash table) για την ανοικτή διευθυνσιοδότηση.
    std::vector<IndexInfo> _table;
    size_t _capacity = 0;

    // Συνάρτηση κατακερματισμού (hash function)
    size_t hash_fn(const Key& k) const {
        // Πρέπει να εξασφαλίσουμε ότι το _capacity > 0
        return std::hash<Key>{}(k) % _capacity;
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
    void insert_index_info(const IndexInfo& info) {
        size_t home = find_home(info.key);
        size_t current_slot = home;
        IndexInfo current_info = info;

        while (_table[current_slot].is_valid) {
            // Το PSL του στοιχείου που πάει να εισαχθεί στη θέση current_slot
            size_t insert_psl = psl(current_slot, find_home(current_info.key));

            // Το PSL του στοιχείου που βρίσκεται ήδη στη θέση current_slot
            size_t displaced_psl = psl(current_slot, find_home(_table[current_slot].key));

            if (insert_psl > displaced_psl) { // Robin Hood: Ανταλλαγή
                // Αν το εισαγόμενο στοιχείο (current_info) είναι πιο μακριά από το home του, το ανταλλάσσουμε.
                std::swap(current_info, _table[current_slot]);
                // Το εκτοπισμένο στοιχείο (τώρα στο current_info) συνεχίζει την εισαγωγή
            }
            // Προχωράμε στην επόμενη θέση (κυκλικά)
            current_slot = (current_slot + 1) % _capacity;
        }
        // Βρήκαμε άδεια θέση: εισάγουμε το στοιχείο που έχει απομείνει στο current_info
        _table[current_slot] = current_info;
        _table[current_slot].is_valid = true;
    }

public:
    // ------------------------------------------------------------
    // 3. Διεπαφή Executor
    // ------------------------------------------------------------

    // Χρησιμοποιούμε default constructor για απλή αρχικοποίηση.
    RobinHoodBackend() = default;

    /**
     * @brief Κατασκευάζει το Robin Hood Hash Table από τις εγγραφές της build side.
     *
     * @param entries Τα κλειδιά (Key) και οι δείκτες γραμμών (row_id) της build side.
     */
    void build_from_entries(const std::vector<std::pair<Key, size_t>>& entries) {
        if (entries.empty()) return;

        // 1. Προεπεξεργασία: Ομαδοποίηση εγγραφών (Key -> [row_ids])
        // Χρησιμοποιούμε std::map για να πάρουμε τα μοναδικά κλειδιά και τις row_ids.
        std::map<Key, std::vector<size_t>> grouped_entries;
        for (const auto& p : entries) {
            grouped_entries[p.first].push_back(p.second);
        }

        // 2. Δημιουργία της _storage και του προσωρινού vector IndexInfo
        _storage.reserve(entries.size());
        size_t current_storage_index = 0;
        
        std::vector<IndexInfo> index_infos_to_insert;
        index_infos_to_insert.reserve(grouped_entries.size());

        for (const auto& pair : grouped_entries) {
            const Key& key = pair.first;
            const std::vector<size_t>& row_ids = pair.second;

            // Δημιουργία IndexInfo για το κλειδί
            IndexInfo info;
            info.key = key; // Αποθήκευση του κλειδιού
            info.start_index = current_storage_index;
            info.count = row_ids.size();
            info.is_valid = true;
            index_infos_to_insert.push_back(info);

            // Προσθήκη των HashEntry στον _storage
            for (size_t row_id : row_ids) {
                _storage.emplace_back(Entry{key, row_id});
            }
            current_storage_index += row_ids.size();
        }
        
        // 3. Αρχικοποίηση του Robin Hood Hash Table (_table)
        // Load factor 0.75 είναι συνήθης για ανοικτή διευθυνσιοδότηση.
        double load_factor = 0.75;
        _capacity = static_cast<size_t>(std::ceil(index_infos_to_insert.size() / load_factor));
        // Εξασφάλιση ότι το capacity είναι τουλάχιστον 1 αν έχουμε data.
        if (_capacity == 0) _capacity = 1;

        _table.resize(_capacity);
        
        // 4. Εισαγωγή των IndexInfo στον Robin Hood πίνακα
        for (const auto& info : index_infos_to_insert) {
            insert_index_info(info);
        }
    }

    /**
     * @brief Πραγματοποιεί αναζήτηση (probe) για ένα κλειδί.
     *
     * @param k Το κλειδί αναζήτησης (από την Probe side).
     * @return std::pair<const Entry*, size_t> Ένας δείκτης στο πρώτο HashEntry
     * της ομάδας και το πλήθος (μήκος) των εγγραφών.
     */
    std::pair<const Entry*, size_t> probe(const Key& k) const {
        if (_capacity == 0 || _table.empty()) {
            return {nullptr, 0};
        }

        size_t home = find_home(k);
        size_t current_slot = home;

        // Robin Hood: Αναζήτηση με early exit
        // Το PSL του κλειδιού που ψάχνουμε, αν βρισκόταν εδώ.
        size_t k_psl;
        
        do {
            if (!_table[current_slot].is_valid) {
                // Βρήκαμε κενή θέση: το κλειδί δεν υπάρχει
                return {nullptr, 0};
            }

            // Το PSL του κλειδιού που ψάχνουμε, αν βρισκόταν στη θέση current_slot.
            k_psl = psl(current_slot, home);

            // Υπολογισμός PSL του στοιχείου που βρίσκεται ήδη στη θέση.
            size_t displaced_psl = psl(current_slot, find_home(_table[current_slot].key));

            if (k_psl > displaced_psl) {
                // Αν το κλειδί μας έχει μεγαλύτερο PSL (είναι πιο μακριά από το home του)
                // από το στοιχείο που είναι αποθηκευμένο, σημαίνει ότι το στοιχείο που
                // ψάχνουμε θα είχε ανταλλαχθεί και θα βρισκόταν σε θέση με μικρότερο PSL. Άρα δεν υπάρχει.
                return {nullptr, 0};
            }
            
            if (_table[current_slot].key == k) {
                // Βρέθηκε το κλειδί.
                return {
                    &_storage[_table[current_slot].start_index], 
                    _table[current_slot].count
                };
            }

            // Συνεχίζουμε γραμμικά.
            current_slot = (current_slot + 1) % _capacity;

        } while (current_slot != home); // Σταματάμε μετά από έναν πλήρη κύκλο (για ασφάλεια)

        // Δεν βρέθηκε
        return {nullptr, 0};
    }
};

} // namespace Contest
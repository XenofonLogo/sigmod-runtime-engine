// Φύλακας πολλαπλής ένταξης: αποτρέπει διπλή εισαγωγή του header
#pragma once

// Βασικά headers STL που απαιτούνται για τα containers/τύπους μεγέθους
#include <vector>
#include <cstdint>
#include <cstddef>
#include <type_traits>
#include <stdexcept>
#include <algorithm>

// Τοπικά headers για τους ορισμούς των πλάνων, hash, bloom, allocators και ρυθμίσεων
#include "plan.h"
#include "hash_common.h"
#include "hash_functions.h"
#include "bloom_filter.h"

namespace Contest {
// TupleEntry: μία πλειάδα με το κλειδί και το row_id της
template<typename Key, typename Hasher = Hash::Hasher32>
struct TupleEntry {
    Key key;          // Το αποθηκευμένο κλειδί
    uint32_t row_id;  // Ο αύξων αριθμός γραμμής στο input
};

// FlatUnchainedHashTable: υλοποίηση flat open-addressing directory με prefix sums
template<typename Key, typename Hasher = Hash::Hasher32>
class FlatUnchainedHashTable {
public:
    using entry_type = TupleEntry<Key>;               // Τύπος αποθηκευμένης πλειάδας
    using entry_allocator = std::allocator<entry_type>; // Χρήση απλού allocator (slab δεν προσφέρει κάτι εδώ)

    // Κατασκευαστής: λαμβάνει προαιρετικά hasher και μέγεθος directory ως δύναμη του 2
    explicit FlatUnchainedHashTable(Hasher hasher = Hasher(), std::size_t directory_power = 10)
        : hasher_(hasher)
    {
        dir_size_ = 1ull << directory_power;   // Μέγεθος directory (2^power)
        dir_mask_ = dir_size_ - 1;             // Μάσκα για γρήγορο modulo
        shift_ = 64 - directory_power;         // Shift για να κρατήσουμε τα υψηλά bits του hash
        
        // Δέσμευση μνήμης με επιπλέον 2 θέσεις ώστε να υπάρχει valid δείκτης στο [-1]
        directory_buffer_.assign(dir_size_ + 2, 0);
        directory_offsets_ = directory_buffer_.data() + 1; // Μετατόπιση για να μπορούμε να προσπελάσουμε το [-1]
        directory_offsets_[-1] = 0; // Το -1 δείχνει στην αρχή του πίνακα tuples
        
        bloom_filters_.assign(dir_size_, 0);   // Μηδενισμός bloom filters

        counts_.assign(dir_size_, 0);          // Προκατανομή μετρητών
        write_ptrs_.assign(dir_size_, 0);      // Προκατανομή δεικτών εγγραφής
    }

    // Προκατανομή μνήμης και δυναμική προσαρμογή μεγέθους directory
    void reserve(std::size_t tuples_capacity) {
        tuples_.reserve(tuples_capacity); // Δέσμευση χωρητικότητας για tuples

        // Όρια μεγέθους directory για ισορροπία μνήμης/απόδοσης
        constexpr std::size_t kMinDirSize = 1ull << 10; // Ελάχιστο 1024 slots
        constexpr std::size_t kMaxDirSize = 1ull << 18; // Μέγιστο 262,144 slots
        constexpr std::size_t kTargetBucket = 8;        // Στόχος μέσες καταχωρήσεις ανά slot

        // Βοηθητική lambda για εύρεση επόμενης δύναμης του 2
        auto next_pow2 = [](std::size_t v) {
            std::size_t p = 1;
            while (p < v) p <<= 1;
            return p;
        };

        // Υπολογισμός επιθυμητού μεγέθους directory
        std::size_t desired = tuples_capacity / kTargetBucket;
        if (desired < kMinDirSize) desired = kMinDirSize; // Εφαρμογή κατώτατου ορίου
        desired = next_pow2(desired);                    // Στρογγυλοποίηση σε δύναμη του 2
        if (desired > kMaxDirSize) desired = kMaxDirSize; // Εφαρμογή ανώτατου ορίου

        // Εάν αλλάζει μέγεθος, ανακατασκευή των buffers
        if (desired != dir_size_) {
            dir_size_ = desired;
            dir_mask_ = dir_size_ - 1; // Νέα μάσκα

            // Επαναϋπολογισμός shift από το μέγεθος του directory
            std::size_t bits = 0;
            std::size_t tmp = dir_size_;
            while (tmp > 1) { bits++; tmp >>= 1; }
            shift_ = 64 - bits;

            // Επαναδέσμευση/μηδενισμός βοηθητικών δομών
            directory_buffer_.assign(dir_size_ + 2, 0);
            directory_offsets_ = directory_buffer_.data() + 1;
            directory_offsets_[-1] = 0;
            bloom_filters_.assign(dir_size_, 0);

            counts_.assign(dir_size_, 0);
            write_ptrs_.assign(dir_size_, 0);
        }
    }

    /*
     * build_from_entries():
     * 
     * Υλοποίηση του Σχήματος 2 (Figure 2) από την εκφώνηση:
     * 
     * 1. Υπολογισμός hash για κάθε tuple
     * 2. Μέτρηση tuples ανά θέση directory (γραμμές 4-8)
     * 3. Υπολογισμός offsets (prefix sum, γραμμές 10-18)
     * 4. Αντιγραφή tuples στη σωστή θέση (γραμμές 19-24)
     * 5. Ενημέρωση bloom filters
     */
    // Κατασκευή hash table από vector entries (optimized διαδρομή)
    void build_from_entries(const std::vector<Contest::HashEntry<Key>>& entries) {
        // Αν δεν υπάρχουν entries, καθαρίζουμε και φεύγουμε
        if (entries.empty()) {
            std::fill(directory_offsets_, directory_offsets_ + dir_size_, 0);
            std::fill(bloom_filters_.begin(), bloom_filters_.end(), 0);
            tuples_.clear();
            return;
        }

        // Μηδενισμός directory και bloom πριν την κατασκευή
        std::fill(directory_offsets_, directory_offsets_ + dir_size_, 0);
        std::fill(bloom_filters_.begin(), bloom_filters_.end(), 0);

        // Βήμα 1: count (πόσα tuples ανά slot)
        if (counts_.size() != dir_size_) counts_.assign(dir_size_, 0);
        else std::fill(counts_.begin(), counts_.end(), 0);

        for (std::size_t i = 0; i < entries.size(); ++i) {
            uint64_t h = compute_hash(entries[i].key);     // Υπολογισμός hash
            std::size_t slot = (h >> shift_) & dir_mask_;   // Εξαγωγή prefix για index
            counts_[slot]++;                               // Αύξηση μετρητή slot
            bloom_filters_[slot] |= Bloom::make_tag_from_hash(h); // Ενημέρωση bloom tag
        }

        // Βήμα 2: prefix sums -> END pointers ανά slot
        uint32_t cumulative = 0;
        for (std::size_t i = 0; i < dir_size_; ++i) {
            cumulative += counts_[i];
            directory_offsets_[i] = cumulative; // Το END pointer του slot
        }

        // Βήμα 3: δέσμευση μνήμης για τα tuples
        tuples_.assign(cumulative, entry_type{});

        // Βήμα 4: αντιγραφή tuples στις σωστές θέσεις
        if (write_ptrs_.size() != dir_size_) write_ptrs_.assign(dir_size_, 0);
        write_ptrs_[0] = 0; // Το πρώτο slot ξεκινά από 0
        for (std::size_t i = 1; i < dir_size_; ++i) {
            write_ptrs_[i] = directory_offsets_[i - 1]; // Αρχή κάθε slot = END προηγούμενου
        }

        for (std::size_t i = 0; i < entries.size(); ++i) {
            uint64_t h = compute_hash(entries[i].key);   // Hash για το tuple
            std::size_t slot = (h >> shift_) & dir_mask_; // Προσδιορισμός slot
            uint32_t pos = write_ptrs_[slot]++;          // Εύρεση θέσης εγγραφής
            tuples_[pos].key = entries[i].key;           // Αντιγραφή κλειδιού
            tuples_[pos].row_id = entries[i].row_id;     // Αντιγραφή row id
        }
        // Στο τέλος, τα tuples είναι συνεχόμενα ανά slot και ταξινομημένα κατά prefix
    }

    // Γρήγορη διαδρομή: χτίσιμο κατευθείαν από zero-copy INT32 στήλη (χωρίς ενδιάμεσο vector)
    void build_from_zero_copy_int32(const Column* src_column,
                                    const std::vector<std::size_t>& page_offsets,
                                    std::size_t num_rows) {
        // Αν δεν έχουμε δεδομένα ή κακή είσοδο, καθαρίζουμε και σταματάμε
        if (num_rows == 0 || src_column == nullptr || page_offsets.size() < 2) {
            std::fill(directory_offsets_, directory_offsets_ + dir_size_, 0);
            std::fill(bloom_filters_.begin(), bloom_filters_.end(), 0);
            tuples_.clear();
            return;
        }

        // Έλεγχος ότι ο τύπος κλειδιού είναι (u)int32
        static_assert(std::is_same_v<Key, int32_t> || std::is_same_v<Key, uint32_t>,
                      "build_from_zero_copy_int32 only supports (u)int32 keys");

        tuples_.reserve(num_rows); // Προκατανομή χώρου

        // Μηδενισμός directory και bloom
        std::fill(directory_offsets_, directory_offsets_ + dir_size_, 0);
        std::fill(bloom_filters_.begin(), bloom_filters_.end(), 0);

        if (counts_.size() != dir_size_) counts_.assign(dir_size_, 0);
        else std::fill(counts_.begin(), counts_.end(), 0);

        const std::size_t npages = page_offsets.size() - 1; // Πλήθος σελίδων
        // Φάση 1: count + bloom ανά slot
        for (std::size_t page_idx = 0; page_idx < npages; ++page_idx) {
            const std::size_t base = page_offsets[page_idx];     // Αρχή σελίδας
            const std::size_t end = page_offsets[page_idx + 1];   // Τέλος σελίδας
            const std::size_t n = end - base;                     // Πλήθος στοιχείων στη σελίδα
            auto* page = src_column->pages[page_idx]->data;       // Δείκτης σε raw page
            auto* data = reinterpret_cast<const int32_t*>(page + 4); // Παράκαμψη header 4 bytes
            for (std::size_t slot_i = 0; slot_i < n; ++slot_i) {
                const Key key = static_cast<Key>(data[slot_i]);   // Ανάγνωση κλειδιού
                const uint64_t h = compute_hash(key);             // Hash υπολογισμός
                const std::size_t slot = (h >> shift_) & dir_mask_; // Εξαγωγή slot
                counts_[slot]++;                                  // Μέτρηση
                bloom_filters_[slot] |= Bloom::make_tag_from_hash(h); // Ενημέρωση bloom
            }
        }

        // Φάση 2: prefix sums -> END pointers
        uint32_t cumulative = 0;
        for (std::size_t i = 0; i < dir_size_; ++i) {
            cumulative += counts_[i];
            directory_offsets_[i] = cumulative;  // END pointer για slot i
        }

        // Φάση 3: δέσμευση μνήμης για tuples
        tuples_.assign(cumulative, entry_type{});

        // Φάση 4: εγγραφή tuples στις θέσεις τους
        if (write_ptrs_.size() != dir_size_) write_ptrs_.assign(dir_size_, 0);
        write_ptrs_[0] = 0;
        for (std::size_t i = 1; i < dir_size_; ++i) write_ptrs_[i] = directory_offsets_[i - 1];

        for (std::size_t page_idx = 0; page_idx < npages; ++page_idx) {
            const std::size_t base = page_offsets[page_idx];
            const std::size_t end = page_offsets[page_idx + 1];
            const std::size_t n = end - base;
            auto* page = src_column->pages[page_idx]->data;
            auto* data = reinterpret_cast<const int32_t*>(page + 4);
            for (std::size_t slot_i = 0; slot_i < n; ++slot_i) {
                const Key key = static_cast<Key>(data[slot_i]);
                const uint64_t h = compute_hash(key);
                const std::size_t slot = (h >> shift_) & dir_mask_;
                const uint32_t pos = write_ptrs_[slot]++;           // Θέση εγγραφής
                tuples_[pos].key = key;                             // Αποθήκευση κλειδιού
                tuples_[pos].row_id = static_cast<uint32_t>(base + slot_i); // Αποθήκευση row id
            }
        }
    }

    // Αναζήτηση (probe): επιστρέφει pointer σε συνεχόμενο εύρος και μήκος
    const entry_type* probe(const Key& key, std::size_t& len) const {
        uint64_t h = compute_hash(key);                  // Υπολογισμός hash
        std::size_t slot = (h >> shift_) & dir_mask_;    // Εύρεση slot

        // Γρήγορο φιλτράρισμα με bloom
        uint16_t tag = Bloom::make_tag_from_hash(h);
        if (!Bloom::maybe_contains(bloom_filters_[slot], tag)) {
            len = 0;             // Καμία πιθανή αντιστοιχία
            return nullptr;      // Επιστροφή άδειου
        }

        // Υπολογισμός εύρους πλειάδων για το slot: [begin, end)
        uint32_t begin = (slot == 0) ? 0 : directory_offsets_[slot - 1];
        uint32_t end = directory_offsets_[slot];
        len = end - begin;       // Μήκος αποτελεσμάτων
        if (len == 0) return nullptr; // Άδειο bucket

        // Επιστροφή δείκτη στην αρχή του εύρους
        return &tuples_[begin];
    }

    // Επιστρέφει πλήθος αποθηκευμένων tuples
    std::size_t size() const { return tuples_.size(); }
    
    // Debug helpers: μέγεθος directory και εκτίμηση μνήμης
    std::size_t directory_size() const { return dir_size_; }
    std::size_t memory_usage() const {
        return tuples_.size() * sizeof(entry_type) +
               dir_size_ * sizeof(uint32_t) +
               bloom_filters_.size() * sizeof(uint16_t);
    }

private:
    // Υπολογισμός hash για Key: ειδική διαδρομή για int32/uint32, αλλιώς std::hash
    uint64_t compute_hash(const Key& k) const {
        if constexpr (std::is_same_v<Key, int32_t> || std::is_same_v<Key, uint32_t>) {
            return hasher_(static_cast<int32_t>(k));
        } else {
            return static_cast<uint64_t>(std::hash<Key>{}(k));
        }
    }

    Hasher hasher_; // Συνάρτηση κατακερματισμού
    
    // Κύρια αποθήκευση tuples (συνεχής μνήμη, ταξινομημένα κατά prefix)
    std::vector<entry_type, entry_allocator> tuples_;
    
    // Directory με υποστήριξη δείκτη [-1]
    std::vector<uint32_t> directory_buffer_; // Πραγματικό buffer
    uint32_t* directory_offsets_;            // Δείκτης με μετατόπιση (offset +1)
    
    std::vector<uint16_t> bloom_filters_;    // Bloom tags ανά slot

    // Επαναχρησιμοποιήσιμα buffers για μετρήσεις/γραφές
    std::vector<uint32_t> counts_;
    std::vector<uint32_t> write_ptrs_;
    
    // Παράμετροι directory
    std::size_t dir_size_;
    std::size_t dir_mask_;
    std::size_t shift_;
};

// Alias για backward compatibility
template<typename Key, typename Hasher = Hash::Hasher32>
using UnchainedHashTable = FlatUnchainedHashTable<Key, Hasher>;

} // namespace Contest

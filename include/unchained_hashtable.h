#pragma once
#include <vector>
#include <cstdint>
#include <cstddef>
#include <type_traits>
#include <stdexcept>
#include <algorithm>
#include "hash_functions.h"
#include "bloom_filter.h"
#include "hash_common.h"
#include "plan.h"

/*
 * TupleEntry:
 * Απλή δομή που αποθηκεύει:
 * - key: το κλειδί της πλειάδας
 * - row_id: τη θέση της πλειάδας στο αρχικό input
 *
 * Χρησιμοποιείται στο ενιαίο buffer των tuples.
 * 
 * ΣΗΜΑΝΤΙΚΟ: Χρησιμοποιούμε uint32_t για row_id (όχι size_t)
 * ώστε να είναι συμβατό με το HashEntry struct και να αποφύγουμε
 * memory corruption κατά το reinterpret_cast στο wrapper.
 */
template<typename Key, typename Hasher = Hash::Hasher32>
struct TupleEntry {
    Key key;
    uint32_t row_id;
};

/*
 * DirectoryEntry:
 * Περιέχει το εύρος [begin_idx, end_idx) των πλειάδων
 * που μοιράζονται το ίδιο prefix του hash.
 *
 * Επίσης αποθηκεύει ένα 16-bit bloom filter για γρήγορο φιλτράρισμα.
 */
struct DirectoryEntry {
    std::size_t begin_idx;  // δείκτης έναρξης στο contiguous buffer
    std::size_t end_idx;    // δείκτης τέλους στο buffer
    uint16_t bloom;         // 16-bit bloom filter (4 bits per tuple)
};

/*
 * UnchainedHashTable:
 * Υλοποιεί τον "unchained" πίνακα κατακερματισμού όπως περιγράφεται στο paper.
 *
 * Βασικές ιδέες:
 * - Όλες οι πλειάδες αποθηκεύονται σε ένα ενιαίο contiguous buffer
 * - Οι πλειάδες ταξινομούνται ανά prefix hash
 * - Το directory κρατά εύρος indices + bloom filter
 * - Οι ανιχνεύσεις (probes) κάνουν:
 *      1. Prefix lookup (O(1))
 *      2. Bloom filter rejection (πολύ φθηνή πράξη)
 *      3. Γραμμική σάρωση στο bucket σε πολύ μικρό range
 *
 * Δεν υπάρχουν chains (linked lists) → αποφυγή pointer chasing.
 * Δεν υπάρχουν διαρροές κλειδιών (όπως open addressing).
 */
template<typename Key, typename Hasher = Hash::Hasher32>
class UnchainedHashTable {
public:
    using entry_type = TupleEntry<Key>;
    using dir_entry  = DirectoryEntry;

    /*
     * directory_power = log2(directory_size)
     * π.χ. directory_power=10 → 2^10 = 1024 buckets
     */
    explicit UnchainedHashTable(Hasher hasher = Hasher(), std::size_t directory_power = 10)
        : hasher_(hasher)
    {
        dir_size_ = 1ull << directory_power;
        dir_mask_ = dir_size_ - 1;
        directory_.assign(dir_size_, {0,0,0});
    }

    /*
     * reserve():
     * Αυξάνει το directory size αν περιμένουμε πολλά tuples.
     * Πάντα στρογγυλοποιεί σε δύναμη του 2 όπως απαιτεί το paper.
     */
    void reserve(std::size_t tuples_capacity) {
        tuples_.reserve(tuples_capacity);

        // Performance-oriented directory sizing:
        // keep the directory power-of-two, but cap it to avoid massive memory and slow clears.
        // Target average bucket length around ~8.
        constexpr std::size_t kMinDirSize = 1ull << 10; // 1024
        constexpr std::size_t kMaxDirSize = 1ull << 18; // 262,144
        constexpr std::size_t kTargetBucket = 8;

        auto next_pow2 = [](std::size_t v) {
            std::size_t p = 1;
            while (p < v) p <<= 1;
            return p;
        };

        std::size_t desired = tuples_capacity / kTargetBucket;
        if (desired < kMinDirSize) desired = kMinDirSize;
        desired = next_pow2(desired);
        if (desired > kMaxDirSize) desired = kMaxDirSize;

        if (desired != dir_size_) {
            dir_size_ = desired;
            dir_mask_ = dir_size_ - 1;
            directory_.assign(dir_size_, {0,0,0});
        }
    }

    /*
     * build_from_entries():
     *
     * 1. Υπολογίζει hash για κάθε key
     * 2. Μετρά buckets (prefix counts)
     * 3. Υπολογίζει prefix sums → προσδιορίζει τα ranges στο buffer
     * 4. Γεμίζει το tuples_ buffer
     * 5. Προσθέτει bloom bits στο directory
     * 6. Γεμίζει τους δείκτες begin/end
     *
     * Μετά το build, το buffer είναι *πλήρως συμπαγές* και *ομαδοποιημένο* κατά hash prefix.
     */
    void build_from_entries(const std::vector<std::pair<Key,std::size_t>>& entries) {
        if (entries.empty()) {
            for (auto &d : directory_) d = {0,0,0};
            tuples_.clear();
            return;
        }

        // Reset bloom bits every build (begin/end will be overwritten below).
        for (auto &d : directory_) d.bloom = 0;

        // Υπολογισμός hash values
        std::vector<uint64_t> hashes;
        hashes.reserve(entries.size());
        for (auto &e: entries)
            hashes.push_back(compute_hash(e.first));

        // Count per prefix bucket
        std::vector<std::size_t> counts(dir_size_, 0);
        for (auto &h: hashes) {
            std::size_t prefix = (h >> 16) & dir_mask_;
            counts[prefix]++;
        }

        // Prefix sums -> begin indices
        std::vector<std::size_t> offsets(dir_size_, 0);
        std::size_t total = 0;
        for (std::size_t i = 0; i < dir_size_; ++i) {
            offsets[i] = total;
            total += counts[i];
        }

        // Ακριβής κατανομή buffer
        tuples_.assign(total, entry_type{});

        // Συμπλήρωση tuples & bloom
        std::vector<std::size_t> write_ptr = offsets;
        for (std::size_t i = 0; i < entries.size(); ++i) {
            uint64_t h = hashes[i];
            std::size_t prefix = (h >> 16) & dir_mask_;
            std::size_t pos = write_ptr[prefix]++;

            tuples_[pos].key = entries[i].first;
            tuples_[pos].row_id = entries[i].second;

            directory_[prefix].bloom |= Bloom::make_tag_from_hash(h);
        }

        // 6) Καταχώρηση begin/end για κάθε bucket
        for (std::size_t i = 0; i < dir_size_; ++i) {
            directory_[i].begin_idx = offsets[i];
            directory_[i].end_idx   = (i+1 < dir_size_) ? offsets[i+1] : total;
        }
    }

    /*
     * build_from_entries (overload for HashEntry):
     * Adapter για να δέχεται HashEntry από το wrapper interface.
     */
    void build_from_entries(const std::vector<Contest::HashEntry<Key>>& entries) {
        std::vector<std::pair<Key, std::size_t>> pairs;
        pairs.reserve(entries.size());
        for (const auto& e : entries) {
            pairs.emplace_back(e.key, static_cast<std::size_t>(e.row_id));
        }
        build_from_entries(pairs);
    }

    /*
     * build_from_zero_copy_int32():
     * Fast path: build directly from a zero-copy INT32 column (no nulls).
     * Simplified version (non-parallel) for backward compatibility.
     */
    void build_from_zero_copy_int32(const Column* src_column,
                                    const std::vector<std::size_t>& page_offsets,
                                    std::size_t num_rows) {
        if (num_rows == 0 || src_column == nullptr || page_offsets.size() < 2) {
            for (auto &d : directory_) d = {0,0,0};
            tuples_.clear();
            return;
        }

        static_assert(std::is_same_v<Key, int32_t> || std::is_same_v<Key, uint32_t>,
                      "build_from_zero_copy_int32 only supports (u)int32 keys");

        tuples_.reserve(num_rows);

        // Reset bloom bits
        for (auto &d : directory_) d.bloom = 0;

        // PHASE 1: count + bloom
        std::vector<std::size_t> counts(dir_size_, 0);
        const std::size_t npages = page_offsets.size() - 1;
        
        for (std::size_t page_idx = 0; page_idx < npages; ++page_idx) {
            const std::size_t base = page_offsets[page_idx];
            const std::size_t end = page_offsets[page_idx + 1];
            const std::size_t n = end - base;
            auto* page = src_column->pages[page_idx]->data;
            auto* data = reinterpret_cast<const int32_t*>(page + 4);
            
            for (std::size_t slot_i = 0; slot_i < n; ++slot_i) {
                const Key key = static_cast<Key>(data[slot_i]);
                const uint64_t h = compute_hash(key);
                const std::size_t prefix = (h >> 16) & dir_mask_;
                counts[prefix]++;
                directory_[prefix].bloom |= Bloom::make_tag_from_hash(h);
            }
        }

        // PHASE 2: prefix sums
        std::vector<std::size_t> offsets(dir_size_, 0);
        std::size_t total = 0;
        for (std::size_t i = 0; i < dir_size_; ++i) {
            offsets[i] = total;
            total += counts[i];
        }

        // PHASE 3: allocate tuples
        tuples_.assign(total, entry_type{});

        // PHASE 4: write tuples
        std::vector<std::size_t> write_ptr = offsets;
        for (std::size_t page_idx = 0; page_idx < npages; ++page_idx) {
            const std::size_t base = page_offsets[page_idx];
            const std::size_t end = page_offsets[page_idx + 1];
            const std::size_t n = end - base;
            auto* page = src_column->pages[page_idx]->data;
            auto* data = reinterpret_cast<const int32_t*>(page + 4);
            
            for (std::size_t slot_i = 0; slot_i < n; ++slot_i) {
                const Key key = static_cast<Key>(data[slot_i]);
                const uint64_t h = compute_hash(key);
                const std::size_t prefix = (h >> 16) & dir_mask_;
                const std::size_t pos = write_ptr[prefix]++;
                tuples_[pos].key = key;
                tuples_[pos].row_id = static_cast<uint32_t>(base + slot_i);
            }
        }

        // PHASE 5: set directory ranges
        for (std::size_t i = 0; i < dir_size_; ++i) {
            directory_[i].begin_idx = offsets[i];
            directory_[i].end_idx = (i+1 < dir_size_) ? offsets[i+1] : total;
        }
    }

    /*
     * build_from_rows:
     * Utility wrapper όταν τα δεδομένα είναι row-store.
     */
    void build_from_rows(const std::vector<std::vector<Key>>& rows, std::size_t key_col) {
        std::vector<std::pair<Key,std::size_t>> entries;
        entries.reserve(rows.size());
        for (std::size_t r = 0; r < rows.size(); ++r) {
            entries.emplace_back(rows[r][key_col], r);
        }
        build_from_entries(entries);
    }

    /*
     * probe():
     * 1. Υπολογίζει hash του key
     * 2. Βρίσκει το prefix bucket
     * 3. Ελέγχει bloom filter (απόρριψη φθηνά)
     * 4. Επιστρέφει pointer στο range + length
     *
     * Σημείωση:
     * Δεν κάνει exact match — αυτό γίνεται με probe_exact().
     */
    const entry_type* probe(const Key& key, std::size_t& len) const {
        uint64_t h = compute_hash(key);
        std::size_t prefix = (h >> 16) & dir_mask_;
        const dir_entry &d = directory_[prefix];

        if (d.begin_idx >= d.end_idx) { len = 0; return nullptr; }

        uint16_t tag = Bloom::make_tag_from_hash(h);
        if (!Bloom::maybe_contains(d.bloom, tag)) {
            len = 0;
            return nullptr;
        }

        len = d.end_idx - d.begin_idx;
        return (len == 0) ? nullptr : &tuples_[d.begin_idx];
    }

    // Removed unused probe_exact() helper (not referenced anywhere)

    std::size_t size() const { return tuples_.size(); }

private:
    /*
     * compute_hash():
     * Αν Key = 32-bit integer → χρησιμοποιεί τον Hasher (Fibonacci hashing)
     * Αλλιώς fallback σε std::hash.
     */
    uint64_t compute_hash(const Key& k) const {
        if constexpr (std::is_same_v<Key, int32_t> || std::is_same_v<Key, uint32_t>) {
            return hasher_(static_cast<int32_t>(k));
        } else {
            return static_cast<uint64_t>(std::hash<Key>{}(k));
        }
    }

    Hasher hasher_;
    std::vector<entry_type> tuples_;     // contiguous buffer με όλα τα tuples
    std::vector<dir_entry> directory_;   // directory με ranges + bloom
    std::size_t dir_size_;               // πλήθος buckets
    std::size_t dir_mask_;               // bitmask για prefix
};

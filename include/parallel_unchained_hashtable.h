#pragma once
#include <vector>
#include <cstdint>
#include <cstddef>
#include <type_traits>
#include <stdexcept>
#include <algorithm>
#include "plan.h"
#include "hash_common.h"
#include "hash_functions.h"
#include "bloom_filter.h"

/*
 * TupleEntry:
 * Απλή δομή που αποθηκεύει:
 * - key: το κλειδί της πλειάδας
 * - row_id: τη θέση της πλειάδας στο αρχικό input
 */
template<typename Key, typename Hasher = Hash::Hasher32>
struct TupleEntry {
    Key key;
    uint32_t row_id;
};

/*
 * FlatUnchainedHashTable:
 * 
 * Βελτιωμένη έκδοση με flat storage και prefix sums (Σχήμα 2).
 *
 * ΒΑΣΙΚΕΣ ΔΙΑΦΟΡΕΣ:
 * =================
 * 
 * ΠΑΛΙΑ ΔΟΜΗ (18 bytes per directory entry):
 * ------------------------------------------
 * struct DirectoryEntry {
 *     size_t begin_idx;  // 8 bytes
 *     size_t end_idx;    // 8 bytes
 *     uint16_t bloom;    // 2 bytes
 * };
 * 
 * ΝΕΑ ΔΟΜΗ (6 bytes per directory entry):
 * ---------------------------------------
 * vector<uint32_t> directory_offsets_;  // 4 bytes - prefix sums
 * vector<uint16_t> bloom_filters_;      // 2 bytes - bloom filters
 * 
 * ΟΦΕΛΗ:
 * ------
 * - 3x λιγότερη μνήμη για directory
 * - Cache-friendly: sequential access
 * - Instant O(1) range calculation: [offsets[i], offsets[i+1])
 * - Σύμφωνα με το paper (Figure 2)
 */
template<typename Key, typename Hasher = Hash::Hasher32>
class FlatUnchainedHashTable {
public:
    using entry_type = TupleEntry<Key>;

    explicit FlatUnchainedHashTable(Hasher hasher = Hasher(), std::size_t directory_power = 10)
        : hasher_(hasher)
    {
        dir_size_ = 1ull << directory_power;
        dir_mask_ = dir_size_ - 1;
        shift_ = 64 - directory_power;
        
        // Allocate directory arrays
        // +1 για το sentinel value στο τέλος (offsets[dir_size_] = total tuples)
        directory_offsets_.assign(dir_size_ + 1, 0);
        bloom_filters_.assign(dir_size_, 0);

        counts_.assign(dir_size_, 0);
        write_ptrs_.assign(dir_size_, 0);
    }

    void reserve(std::size_t tuples_capacity) {
        tuples_.reserve(tuples_capacity);

        // Performance-oriented directory sizing:
        // Keep directory power-of-two, but cap it to avoid massive memory and slow clears.
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

            // Recalculate shift
            std::size_t bits = 0;
            std::size_t tmp = dir_size_;
            while (tmp > 1) { bits++; tmp >>= 1; }
            shift_ = 64 - bits;

            directory_offsets_.assign(dir_size_ + 1, 0);
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
     * 2. Count: Μέτρηση tuples ανά directory slot (lines 4-8)
     * 3. Prefix Sum: Υπολογισμός offsets (lines 10-18)
     * 4. Copy: Αντιγραφή tuples στη σωστή θέση (lines 19-24)
     * 5. Bloom: Ενημέρωση bloom filters
     */
    void build_from_entries(const std::vector<Contest::HashEntry<Key>>& entries) {
        if (entries.empty()) {
            std::fill(directory_offsets_.begin(), directory_offsets_.end(), 0);
            std::fill(bloom_filters_.begin(), bloom_filters_.end(), 0);
            tuples_.clear();
            return;
        }

        // Reset directory state (bloom bits + offsets).
        std::fill(directory_offsets_.begin(), directory_offsets_.end(), 0);
        std::fill(bloom_filters_.begin(), bloom_filters_.end(), 0);

        // ===============================================================
        // PHASE 2: COUNT - Μέτρηση tuples ανά directory slot
        // (Figure 2, lines 4-8)
        // ===============================================================
        if (counts_.size() != dir_size_) counts_.assign(dir_size_, 0);
        else std::fill(counts_.begin(), counts_.end(), 0);

        for (std::size_t i = 0; i < entries.size(); ++i) {
            uint64_t h = compute_hash(entries[i].key);
            std::size_t slot = (h >> shift_) & dir_mask_;
            
            // Count (line 6)
            counts_[slot]++;
            
            // Bloom filter (line 7)
            bloom_filters_[slot] |= Bloom::make_tag_from_hash(h);
        }

        // ===============================================================
        // PHASE 3: PREFIX SUM - Υπολογισμός offsets
        // (Figure 2, lines 10-18)
        // ===============================================================
        
        // Exclusive prefix sum
        // directory_offsets_[i] = αριθμός tuples πριν το slot i
        uint32_t cumulative = 0;
        for (std::size_t i = 0; i < dir_size_; ++i) {
            directory_offsets_[i] = cumulative;
            cumulative += counts_[i];
        }
        // Sentinel value: total tuples
        directory_offsets_[dir_size_] = cumulative;

        // ===============================================================
        // PHASE 4: ALLOCATE - Δέσμευση μνήμης για tuples
        // ===============================================================
        tuples_.assign(cumulative, entry_type{});

        // ===============================================================
        // PHASE 5: COPY - Αντιγραφή tuples στη σωστή θέση
        // (Figure 2, lines 19-24)
        // ===============================================================
        
        // Write pointers: που να γράψουμε το επόμενο tuple ανά slot
        // FIXED: Use same type as directory_offsets_
        if (write_ptrs_.size() != dir_size_) write_ptrs_.assign(dir_size_, 0);
        for (std::size_t i = 0; i < dir_size_; ++i) write_ptrs_[i] = directory_offsets_[i];

        for (std::size_t i = 0; i < entries.size(); ++i) {
            uint64_t h = compute_hash(entries[i].key);
            std::size_t slot = (h >> shift_) & dir_mask_;
            
            // Βρες την επόμενη διαθέσιμη θέση για αυτό το slot (line 20-21)
            uint32_t pos = write_ptrs_[slot]++;
            
            // Γράψε το tuple (line 22)
            tuples_[pos].key = entries[i].key;
            tuples_[pos].row_id = entries[i].row_id;
        }
        
        // Τώρα τα tuples είναι sorted by hash prefix και contiguous!
    }

    // Fast path: build directly from a zero-copy INT32 column (no nulls) without materializing
    // an intermediate vector of (key,row_id) pairs.
    void build_from_zero_copy_int32(const Column* src_column,
                                    const std::vector<std::size_t>& page_offsets,
                                    std::size_t num_rows) {
        if (num_rows == 0 || src_column == nullptr || page_offsets.size() < 2) {
            std::fill(directory_offsets_.begin(), directory_offsets_.end(), 0);
            std::fill(bloom_filters_.begin(), bloom_filters_.end(), 0);
            tuples_.clear();
            return;
        }

        static_assert(std::is_same_v<Key, int32_t> || std::is_same_v<Key, uint32_t>,
                      "build_from_zero_copy_int32 only supports (u)int32 keys");

        tuples_.reserve(num_rows);

        // Reset directory state (bloom bits + offsets).
        std::fill(directory_offsets_.begin(), directory_offsets_.end(), 0);
        std::fill(bloom_filters_.begin(), bloom_filters_.end(), 0);

        if (counts_.size() != dir_size_) counts_.assign(dir_size_, 0);
        else std::fill(counts_.begin(), counts_.end(), 0);

        const std::size_t npages = page_offsets.size() - 1;
        // PHASE 1: count + bloom
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
                counts_[slot]++;
                bloom_filters_[slot] |= Bloom::make_tag_from_hash(h);
            }
        }

        // PHASE 2: prefix sums
        uint32_t cumulative = 0;
        for (std::size_t i = 0; i < dir_size_; ++i) {
            directory_offsets_[i] = cumulative;
            cumulative += counts_[i];
        }
        directory_offsets_[dir_size_] = cumulative;

        // PHASE 3: allocate tuples
        tuples_.assign(cumulative, entry_type{});

        // PHASE 4: write tuples
        if (write_ptrs_.size() != dir_size_) write_ptrs_.assign(dir_size_, 0);
        for (std::size_t i = 0; i < dir_size_; ++i) write_ptrs_[i] = directory_offsets_[i];

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
                const uint32_t pos = write_ptrs_[slot]++;
                tuples_[pos].key = key;
                tuples_[pos].row_id = static_cast<uint32_t>(base + slot_i);
            }
        }
    }

    /*
     * probe():
     * 
     * ΒΕΛΤΙΩΜΕΝΗ ΕΚΔΟΣΗ:
     * - Instant O(1) range calculation: [offsets[slot], offsets[slot+1])
     * - No pointer chasing
     * - Cache-friendly sequential scan
     */
    const entry_type* probe(const Key& key, std::size_t& len) const {
        uint64_t h = compute_hash(key);
        std::size_t slot = (h >> shift_) & dir_mask_;

        // Bloom filter rejection (πολύ φθηνό)
        uint16_t tag = Bloom::make_tag_from_hash(h);
        if (!Bloom::maybe_contains(bloom_filters_[slot], tag)) {
            len = 0;
            return nullptr;
        }

        // INSTANT range calculation (NO BRANCHING)
        uint32_t begin = directory_offsets_[slot];
        uint32_t end = directory_offsets_[slot + 1];
        
        len = end - begin;
        
        if (len == 0) {
            return nullptr;
        }

        // Return pointer to contiguous range
        return &tuples_[begin];
    }

    /*
     * probe_exact():
     * Helper για exact key matching
     */
    std::vector<std::size_t> probe_exact(const Key& key) const {
        std::size_t len;
        const entry_type* base = probe(key, len);

        std::vector<std::size_t> result;
        if (!base) return result;

        for (std::size_t i = 0; i < len; ++i) {
            if (base[i].key == key) {
                result.push_back(base[i].row_id);
            }
        }

        return result;
    }

    std::size_t size() const { return tuples_.size(); }
    
    // Debugging helpers
    std::size_t directory_size() const { return dir_size_; }
    std::size_t memory_usage() const {
        return tuples_.size() * sizeof(entry_type) +
               directory_offsets_.size() * sizeof(uint32_t) +
               bloom_filters_.size() * sizeof(uint16_t);
    }

private:
    uint64_t compute_hash(const Key& k) const {
        if constexpr (std::is_same_v<Key, int32_t> || std::is_same_v<Key, uint32_t>) {
            return hasher_(static_cast<int32_t>(k));
        } else {
            return static_cast<uint64_t>(std::hash<Key>{}(k));
        }
    }

    Hasher hasher_;
    
    // FLAT STORAGE - Optimized layout
    std::vector<entry_type> tuples_;            // Contiguous tuples (sorted by prefix)
    std::vector<uint32_t> directory_offsets_;   // Prefix sums (size = dir_size + 1)
    std::vector<uint16_t> bloom_filters_;       // Bloom filters (size = dir_size)

    // Reused scratch buffers to avoid per-build allocations.
    std::vector<uint32_t> counts_;              // Counts per slot
    std::vector<uint32_t> write_ptrs_;          // Write pointers per slot
    
    std::size_t dir_size_;   // Number of directory slots
    std::size_t dir_mask_;   // Bitmask for prefix extraction
    std::size_t shift_;      // Shift amount for prefix extraction
};

// Alias για backward compatibility
template<typename Key, typename Hasher = Hash::Hasher32>
using UnchainedHashTable = FlatUnchainedHashTable<Key, Hasher>;

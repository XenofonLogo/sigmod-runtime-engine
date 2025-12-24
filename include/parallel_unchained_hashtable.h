#pragma once
#include <vector>
#include <cstdint>
#include <cstddef>
#include <type_traits>
#include <stdexcept>
#include <algorithm>
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
    std::size_t row_id;
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
    }

    void reserve(std::size_t tuples_capacity) {
        std::size_t desired = 1;
        while (desired < tuples_capacity && desired < (1ull << 30)) {
            desired <<= 1;
        }

        if (desired > dir_size_) {
            dir_size_ = desired;
            dir_mask_ = dir_size_ - 1;
            
            // Recalculate shift
            std::size_t bits = 0;
            std::size_t tmp = dir_size_;
            while (tmp > 1) { bits++; tmp >>= 1; }
            shift_ = 64 - bits;
            
            directory_offsets_.assign(dir_size_ + 1, 0);
            bloom_filters_.assign(dir_size_, 0);
        }
        tuples_.reserve(tuples_capacity);
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
    void build_from_entries(const std::vector<std::pair<Key, std::size_t>>& entries) {
        if (entries.empty()) {
            std::fill(directory_offsets_.begin(), directory_offsets_.end(), 0);
            std::fill(bloom_filters_.begin(), bloom_filters_.end(), 0);
            tuples_.clear();
            return;
        }

        // ===============================================================
        // PHASE 1: Compute hashes
        // ===============================================================
        std::vector<uint64_t> hashes;
        hashes.reserve(entries.size());
        for (const auto& e : entries) {
            hashes.push_back(compute_hash(e.first));
        }

        // ===============================================================
        // PHASE 2: COUNT - Μέτρηση tuples ανά directory slot
        // (Figure 2, lines 4-8)
        // ===============================================================
        std::vector<uint32_t> counts(dir_size_, 0);
        
        for (std::size_t i = 0; i < hashes.size(); ++i) {
            uint64_t h = hashes[i];
            std::size_t slot = (h >> shift_) & dir_mask_;
            
            // Count (line 6)
            counts[slot]++;
            
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
            cumulative += counts[i];
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
        std::vector<uint32_t> write_ptrs(dir_size_);
        for (std::size_t i = 0; i < dir_size_; ++i) {
            write_ptrs[i] = directory_offsets_[i];
        }
        
        for (std::size_t i = 0; i < entries.size(); ++i) {
            uint64_t h = hashes[i];
            std::size_t slot = (h >> shift_) & dir_mask_;
            
            // Βρες την επόμενη διαθέσιμη θέση για αυτό το slot (line 20-21)
            uint32_t pos = write_ptrs[slot]++;
            
            // Γράψε το tuple (line 22)
            tuples_[pos].key = entries[i].first;
            tuples_[pos].row_id = entries[i].second;
        }
        
        // Τώρα τα tuples είναι sorted by hash prefix και contiguous!
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
    
    std::size_t dir_size_;   // Number of directory slots
    std::size_t dir_mask_;   // Bitmask for prefix extraction
    std::size_t shift_;      // Shift amount for prefix extraction
};

// Alias για backward compatibility
template<typename Key, typename Hasher = Hash::Hasher32>
using UnchainedHashTable = FlatUnchainedHashTable<Key, Hasher>;

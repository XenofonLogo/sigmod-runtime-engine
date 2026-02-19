// Include guard: prevents double inclusion of this header
#pragma once

// Standard STL headers required for containers/size types
#include <vector>
#include <cstdint>
#include <cstddef>
#include <type_traits>
#include <stdexcept>
#include <algorithm>

// Local headers for plan definitions, hashing, bloom helpers, allocators and settings
#include "plan.h"
#include "hash_common.h"
#include "hash_functions.h"
#include "bloom_filter.h"

namespace Contest {
// TupleEntry: a tuple containing the key and its row id
template<typename Key, typename Hasher = Hash::Hasher32>
struct TupleEntry {
    Key key;          // The stored key
    uint32_t row_id;  // The row id index in the input
};

// FlatUnchainedHashTable: flat, prefix-sum based unchained hash table implementation
template<typename Key, typename Hasher = Hash::Hasher32>
class FlatUnchainedHashTable {
public:
    using entry_type = TupleEntry<Key>;               // Type of stored tuple
    using entry_allocator = std::allocator<entry_type>; // Use simple allocator (slab not used here)

    // Constructor: optionally accepts a hasher and the directory size as a power of two
    explicit FlatUnchainedHashTable(Hasher hasher = Hasher(), std::size_t directory_power = 10)
        : hasher_(hasher)
    {
        dir_size_ = 1ull << directory_power;   // Directory size (2^power)
        dir_mask_ = dir_size_ - 1;             // Mask for fast modulo
        shift_ = 64 - directory_power;         // Shift to keep the high bits of the hash
        
        // Allocate memory with two extra slots so we can safely index [-1]
        directory_buffer_.assign(dir_size_ + 2, 0);
        directory_offsets_ = directory_buffer_.data() + 1; // Offset so we can index [-1]
        directory_offsets_[-1] = 0; // [-1] points to the start of the tuples array

        bloom_filters_.assign(dir_size_, 0);   // Zero bloom filters

        counts_.assign(dir_size_, 0);          // Preallocate counters
        write_ptrs_.assign(dir_size_, 0);      // Preallocate write pointers
    }

    // Reserve capacity and dynamically adjust the directory size
    void reserve(std::size_t tuples_capacity) {
        tuples_.reserve(tuples_capacity); // Reserve capacity for tuples

        // Directory size bounds to balance memory and performance
        constexpr std::size_t kMinDirSize = 1ull << 10; // Minimum 1024 slots
        constexpr std::size_t kMaxDirSize = 1ull << 18; // Maximum 262,144 slots
        constexpr std::size_t kTargetBucket = 8;        // Target average entries per slot

        // Helper lambda to find next power of two
        auto next_pow2 = [](std::size_t v) {
            std::size_t p = 1;
            while (p < v) p <<= 1;
            return p;
        };

        // Compute desired directory size
        std::size_t desired = tuples_capacity / kTargetBucket;
        if (desired < kMinDirSize) desired = kMinDirSize; // Apply minimum bound
        desired = next_pow2(desired);                    // Round up to power of two
        if (desired > kMaxDirSize) desired = kMaxDirSize; // Apply maximum bound

        // If size changes, rebuild auxiliary buffers
        if (desired != dir_size_) {
            dir_size_ = desired;
            dir_mask_ = dir_size_ - 1; // New mask

            // Recompute shift from the directory size
            std::size_t bits = 0;
            std::size_t tmp = dir_size_;
            while (tmp > 1) { bits++; tmp >>= 1; }
            shift_ = 64 - bits;

            // Re-allocate / zero auxiliary structures
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
     * Implements the build steps (see Figure 2):
     *
     * 1. Compute hash for every tuple
     * 2. Count tuples per directory slot
     * 3. Compute offsets (prefix sums)
     * 4. Copy tuples into their final positions
     * 5. Update bloom filters
     */
    // Build hash table from a vector of entries (optimized path)
    void build_from_entries(const std::vector<Contest::HashEntry<Key>>& entries) {
        // If there are no entries, clear and return
        if (entries.empty()) {
            std::fill(directory_offsets_, directory_offsets_ + dir_size_, 0);
            std::fill(bloom_filters_.begin(), bloom_filters_.end(), 0);
            tuples_.clear();
            return;
        }

        // Zero directory and bloom filters before building
        std::fill(directory_offsets_, directory_offsets_ + dir_size_, 0);
        std::fill(bloom_filters_.begin(), bloom_filters_.end(), 0);

        // Step 1: count (number of tuples per slot)
        if (counts_.size() != dir_size_) counts_.assign(dir_size_, 0);
        else std::fill(counts_.begin(), counts_.end(), 0);

        for (std::size_t i = 0; i < entries.size(); ++i) {
            uint64_t h = compute_hash(entries[i].key);     // Compute hash
            std::size_t slot = (h >> shift_) & dir_mask_;   // Extract prefix for index
            counts_[slot]++;                               // Increment slot counter
            bloom_filters_[slot] |= Bloom::make_tag_from_hash(h); // Update bloom tag
        }

        // Step 2: prefix sums -> END pointers per slot
        uint32_t cumulative = 0;
        for (std::size_t i = 0; i < dir_size_; ++i) {
            cumulative += counts_[i];
            directory_offsets_[i] = cumulative; // The END pointer of the slot
        }

        // Step 3: allocate memory for tuples
        tuples_.assign(cumulative, entry_type{});

        // Step 4: copy tuples into their target positions
        if (write_ptrs_.size() != dir_size_) write_ptrs_.assign(dir_size_, 0);
        write_ptrs_[0] = 0; // First slot starts at 0
        for (std::size_t i = 1; i < dir_size_; ++i) {
            write_ptrs_[i] = directory_offsets_[i - 1]; // Start of each slot = END of previous
        }

        for (std::size_t i = 0; i < entries.size(); ++i) {
            uint64_t h = compute_hash(entries[i].key);   // Hash for the tuple
            std::size_t slot = (h >> shift_) & dir_mask_; // Determine slot
            uint32_t pos = write_ptrs_[slot]++;          // Find write position
            tuples_[pos].key = entries[i].key;           // Copy key
            tuples_[pos].row_id = entries[i].row_id;     // Copy row id
        }
        // At the end, tuples are contiguous per slot and ordered by prefix
    }

    // Fast path: build directly from a zero-copy INT32 column (no intermediate vector)
    void build_from_zero_copy_int32(const Column* src_column,
                                    const std::vector<std::size_t>& page_offsets,
                                    std::size_t num_rows) {
        // If no data or bad input, clear and return
        if (num_rows == 0 || src_column == nullptr || page_offsets.size() < 2) {
            std::fill(directory_offsets_, directory_offsets_ + dir_size_, 0);
            std::fill(bloom_filters_.begin(), bloom_filters_.end(), 0);
            tuples_.clear();
            return;
        }

        // Ensure the key type is (u)int32
        static_assert(std::is_same_v<Key, int32_t> || std::is_same_v<Key, uint32_t>,
                      "build_from_zero_copy_int32 only supports (u)int32 keys");

        tuples_.reserve(num_rows); // Reserve space

        // Zero directory and bloom
        std::fill(directory_offsets_, directory_offsets_ + dir_size_, 0);
        std::fill(bloom_filters_.begin(), bloom_filters_.end(), 0);

        if (counts_.size() != dir_size_) counts_.assign(dir_size_, 0);
        else std::fill(counts_.begin(), counts_.end(), 0);

        const std::size_t npages = page_offsets.size() - 1; // Number of pages
        // Phase 1: count + bloom per slot
        for (std::size_t page_idx = 0; page_idx < npages; ++page_idx) {
            const std::size_t base = page_offsets[page_idx];     // Page start
            const std::size_t end = page_offsets[page_idx + 1];   // Page end
            const std::size_t n = end - base;                     // Number of elements in the page
            auto* page = src_column->pages[page_idx]->data;       // Pointer to raw page
            auto* data = reinterpret_cast<const int32_t*>(page + 4); // Skip 4-byte header
            for (std::size_t slot_i = 0; slot_i < n; ++slot_i) {
                const Key key = static_cast<Key>(data[slot_i]);   // Read key
                const uint64_t h = compute_hash(key);             // Compute hash
                const std::size_t slot = (h >> shift_) & dir_mask_; // Extract slot
                counts_[slot]++;                                  // Count
                bloom_filters_[slot] |= Bloom::make_tag_from_hash(h); // Update bloom
            }
        }

        // Phase 2: prefix sums -> END pointers
        uint32_t cumulative = 0;
        for (std::size_t i = 0; i < dir_size_; ++i) {
            cumulative += counts_[i];
            directory_offsets_[i] = cumulative;  // END pointer for slot i
        }

        // Phase 3: allocate memory for tuples
        tuples_.assign(cumulative, entry_type{});

        // Phase 4: write tuples into their target positions
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
                const uint32_t pos = write_ptrs_[slot]++;           // Write position
                tuples_[pos].key = key;                             // Store key
                tuples_[pos].row_id = static_cast<uint32_t>(base + slot_i); // Store row id
            }
        }
    }

    // Probe: returns pointer to a contiguous range and its length
    const entry_type* probe(const Key& key, std::size_t& len) const {
        uint64_t h = compute_hash(key);                  // Compute hash
        std::size_t slot = (h >> shift_) & dir_mask_;    // Find slot

        // Fast bloom filter check
        uint16_t tag = Bloom::make_tag_from_hash(h);
        if (!Bloom::maybe_contains(bloom_filters_[slot], tag)) {
            len = 0;             // No possible matches
            return nullptr;      // Return empty
        }

        // Compute tuple range for the slot: [begin, end)
        uint32_t begin = (slot == 0) ? 0 : directory_offsets_[slot - 1];
        uint32_t end = directory_offsets_[slot];
        len = end - begin;       // Length of results
        if (len == 0) return nullptr; // Empty bucket

        // Return pointer to the start of the range
        return &tuples_[begin];
    }

    // Return number of stored tuples
    std::size_t size() const { return tuples_.size(); }
    
    // Debug helpers: directory size and estimated memory usage
    std::size_t directory_size() const { return dir_size_; }
    std::size_t memory_usage() const {
        return tuples_.size() * sizeof(entry_type) +
               dir_size_ * sizeof(uint32_t) +
               bloom_filters_.size() * sizeof(uint16_t);
    }

private:
    // Compute hash for Key: specialized path for int32/uint32, otherwise std::hash
    uint64_t compute_hash(const Key& k) const {
        if constexpr (std::is_same_v<Key, int32_t> || std::is_same_v<Key, uint32_t>) {
            return hasher_(static_cast<int32_t>(k));
        } else {
            return static_cast<uint64_t>(std::hash<Key>{}(k));
        }
    }

    Hasher hasher_; // Hash function

    // Main tuple storage (contiguous memory, prefix-ordered)
    std::vector<entry_type, entry_allocator> tuples_;

    // Directory with support for a [-1] pointer
    std::vector<uint32_t> directory_buffer_; // Actual buffer
    uint32_t* directory_offsets_;            // Offset pointer (offset +1)

    std::vector<uint16_t> bloom_filters_;    // Bloom tags per slot

    // Reusable buffers for counts/writes
    std::vector<uint32_t> counts_;
    std::vector<uint32_t> write_ptrs_;

    // Directory parameters
    std::size_t dir_size_;
    std::size_t dir_mask_;
    std::size_t shift_;
};

// Alias for backward compatibility
template<typename Key, typename Hasher = Hash::Hasher32>
using UnchainedHashTable = FlatUnchainedHashTable<Key, Hasher>;

} // namespace Contest

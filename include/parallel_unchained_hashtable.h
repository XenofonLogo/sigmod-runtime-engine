#pragma once
#include <vector>
#include <cstdint>
#include <cstddef>
#include <type_traits>
#include <stdexcept>
#include <algorithm>
#include <thread>
#include <cstdlib>
#include <memory>
#include "plan.h"
#include "hash_common.h"
#include "hash_functions.h"
#include "bloom_filter.h"
#include "three_level_slab.h"
#include "temp_allocator.h"
#include "partition_hash_builder.h"
#include "project_config.h"

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
    using entry_allocator = std::allocator<entry_type>;  // Use standard allocator (slab has no perf benefit)

    explicit FlatUnchainedHashTable(Hasher hasher = Hasher(), std::size_t directory_power = 10)
        : hasher_(hasher)
    {
        dir_size_ = 1ull << directory_power;
        dir_mask_ = dir_size_ - 1;
        shift_ = 64 - directory_power;
        
        // Allocate directory with extra slots for directory[-1] entry (PDF requirement 8.3)
        // Buffer has dir_size + 2 slots: [-1], [0], [1], ..., [dir_size-1], [dir_size]
        directory_buffer_.assign(dir_size_ + 2, 0);
        directory_offsets_ = directory_buffer_.data() + 1;  // Shift pointer so [-1] is valid
        directory_offsets_[-1] = 0;  // Points to start of tuple storage
        
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
     * 2. Count: Μέτρηση tuples ανά directory slot (lines 4-8)
     * 3. Prefix Sum: Υπολογισμός offsets (lines 10-18)
     * 4. Copy: Αντιγραφή tuples στη σωστή θέση (lines 19-24)
     * 5. Bloom: Ενημέρωση bloom filters
     */
    void build_from_entries(const std::vector<Contest::HashEntry<Key>>& entries) {
        // Mode selection: STRICT uses partition build, OPTIMIZED uses simple build
        if (Contest::use_strict_project() &&
            entries.size() >= required_partition_build_min_rows()) {
            build_from_entries_partitioned_parallel(entries);
            return;
        }
        if (entries.empty()) {
            std::fill(directory_offsets_, directory_offsets_ + dir_size_, 0);
            std::fill(bloom_filters_.begin(), bloom_filters_.end(), 0);
            tuples_.clear();
            return;
        }

        // Reset directory state (bloom bits + offsets).
        std::fill(directory_offsets_, directory_offsets_ + dir_size_, 0);
        std::fill(bloom_filters_.begin(), bloom_filters_.end(), 0);

        // ===============================================================
        // PHASE 1: COUNT - Μέτρηση tuples ανά directory slot
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
        // PHASE 2: PREFIX SUM - Υπολογισμός offsets
        // (Figure 2, lines 10-18)
        // PDF Requirement 8.2: Directory entries store END pointers
        // ===============================================================
        
        // Inclusive prefix sum (END pointers)
        // directory_offsets_[i] = cumulative count of tuples through slot i (END of slot i)
        // Range for slot i: [directory_offsets_[i-1], directory_offsets_[i])
        uint32_t cumulative = 0;
        for (std::size_t i = 0; i < dir_size_; ++i) {
            cumulative += counts_[i];
            directory_offsets_[i] = cumulative;  // END pointer
        }
        // Note: directory_offsets_[-1] = 0 (start of storage, set in constructor)

        // ===============================================================
        // PHASE 3: ALLOCATE - Δέσμευση μνήμης για tuples
        // ===============================================================
        tuples_.assign(cumulative, entry_type{});

        // ===============================================================
        // PHASE 4: COPY - Αντιγραφή tuples στη σωστή θέση
        // (Figure 2, lines 19-24)
        // ===============================================================
        
        // Write pointers: START of each slot (previous entry's END)
        if (write_ptrs_.size() != dir_size_) write_ptrs_.assign(dir_size_, 0);
        write_ptrs_[0] = 0;  // First slot starts at 0
        for (std::size_t i = 1; i < dir_size_; ++i) {
            write_ptrs_[i] = directory_offsets_[i - 1];  // Start = previous END
        }

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
        // Mode selection: STRICT uses partition build, OPTIMIZED uses simple build
        if (Contest::use_strict_project() &&
            num_rows >= required_partition_build_min_rows()) {
            build_from_zero_copy_int32_partitioned_parallel(src_column, page_offsets, num_rows);
            return;
        }
        if (num_rows == 0 || src_column == nullptr || page_offsets.size() < 2) {
            std::fill(directory_offsets_, directory_offsets_ + dir_size_, 0);
            std::fill(bloom_filters_.begin(), bloom_filters_.end(), 0);
            tuples_.clear();
            return;
        }

        static_assert(std::is_same_v<Key, int32_t> || std::is_same_v<Key, uint32_t>,
                      "build_from_zero_copy_int32 only supports (u)int32 keys");

        tuples_.reserve(num_rows);

        // Reset directory state (bloom bits + offsets).
        std::fill(directory_offsets_, directory_offsets_ + dir_size_, 0);
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

        // PHASE 2: prefix sums (END pointer semantics)
        uint32_t cumulative = 0;
        for (std::size_t i = 0; i < dir_size_; ++i) {
            cumulative += counts_[i];
            directory_offsets_[i] = cumulative;  // END pointer
        }

        // PHASE 3: allocate tuples
        tuples_.assign(cumulative, entry_type{});

        // PHASE 4: write tuples (write pointers are START = previous END)
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
                const uint32_t pos = write_ptrs_[slot]++;
                tuples_[pos].key = key;
                tuples_[pos].row_id = static_cast<uint32_t>(base + slot_i);
            }
        }
    }

    /*
     * probe():
     * 
     * ΒΕΛΤΙΩΜΕΝΗ ΕΚΔΟΣΗ με END pointer semantics (PDF requirement 8.2):
     * - Directory stores END pointers
     * - Range: [directory_offsets_[slot-1], directory_offsets_[slot])
     * - directory_offsets_[-1] = 0 (start of storage)
     * - Instant O(1) range calculation
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
        // Range: [previous END, current END)
        uint32_t begin = (slot == 0) ? 0 : directory_offsets_[slot - 1];
        uint32_t end = directory_offsets_[slot];
        
        len = end - begin;
        
        if (len == 0) {
            return nullptr;
        }

        // Return pointer to contiguous range
        return &tuples_[begin];
    }

    static std::size_t required_partition_build_min_rows() {
        // Env vars not used anymore; always apply required algorithm when STRICT.
        return std::size_t(0);
    }

    using TmpEntry = Contest::TmpEntry<Key>;
    using Chunk = Contest::Chunk<Key>;
    using ChunkList = Contest::ChunkList<Key>;
    using TempAlloc = Contest::TempAlloc;

    static inline Chunk* alloc_chunk(TempAlloc& alloc) {
        return Contest::alloc_chunk<Key>(alloc);
    }

    static inline void chunklist_push(ChunkList& list, const TmpEntry& e, TempAlloc& alloc) {
        Contest::chunklist_push<Key>(list, e, alloc);
    }

    // Required algorithm: phase-based partition -> gather/count -> prefix sums -> one-writer-per-partition copy.
    void build_from_entries_partitioned_parallel(const std::vector<Contest::HashEntry<Key>>& entries) {
        if (entries.empty()) {
            std::fill(directory_offsets_, directory_offsets_ + dir_size_, 0);
            std::fill(bloom_filters_.begin(), bloom_filters_.end(), 0);
            tuples_.clear();
            return;
        }

        std::size_t hw = std::thread::hardware_concurrency();
        if (!hw) hw = 4;
        const std::size_t n = entries.size();
        const std::size_t nthreads = (n < 2048) ? 1 : hw;

        // Phase 1: partition into per-thread chunk lists (per directory slot).
        std::vector<std::vector<ChunkList>> lists(nthreads, std::vector<ChunkList>(dir_size_));
        // 3-Level Slab Allocator:
        // Level 1: Global (via operator new in TempAlloc)
        // Level 2: Thread-local allocator (one TempAlloc per thread)
        // Level 3: Partition chunks (allocated from thread's TempAlloc)
        std::vector<std::unique_ptr<TempAlloc>> allocs;
        allocs.reserve(nthreads);
        for (std::size_t t = 0; t < nthreads; ++t) {
            allocs.emplace_back(std::make_unique<TempAlloc>());
        }

        const std::size_t block = (n + nthreads - 1) / nthreads;
        std::vector<std::thread> threads;
        threads.reserve(nthreads);

        for (std::size_t t = 0; t < nthreads; ++t) {
            const std::size_t begin = t * block;
            const std::size_t end = std::min(n, begin + block);
            if (begin >= end) break;
            threads.emplace_back([&, t, begin, end]() {
                auto& my_lists = lists[t];
                TempAlloc& my_alloc = *allocs[t];  // One allocator per thread
                for (std::size_t i = begin; i < end; ++i) {
                    const uint64_t h = compute_hash(entries[i].key);
                    const std::size_t slot = (h >> shift_) & dir_mask_;
                    const uint16_t tag = Bloom::make_tag_from_hash(h);
                    // Allocate partition chunks from thread-local allocator
                    chunklist_push(my_lists[slot], TmpEntry{entries[i].key, entries[i].row_id, tag}, my_alloc);
                }
            });
        }
        for (auto& th : threads) th.join();

        // Phase 2: one-writer-per-partition counts and blooms.
        if (counts_.size() != dir_size_) counts_.assign(dir_size_, 0);
        else std::fill(counts_.begin(), counts_.end(), 0);
        std::fill(bloom_filters_.begin(), bloom_filters_.end(), 0);

        threads.clear();
        const std::size_t workers = nthreads;
        for (std::size_t t = 0; t < workers; ++t) {
            threads.emplace_back([&, t]() {
                for (std::size_t slot = t; slot < dir_size_; slot += workers) {
                    uint32_t c = 0;
                    uint16_t bloom = 0;
                    for (std::size_t src = 0; src < nthreads; ++src) {
                        for (Chunk* ch = lists[src][slot].head; ch; ch = ch->next) {
                            c += ch->size;
                            for (uint32_t i = 0; i < ch->size; ++i) bloom |= ch->items[i].tag;
                        }
                    }
                    counts_[slot] = c;
                    bloom_filters_[slot] = bloom;
                }
            });
        }
        for (auto& th : threads) th.join();

        // Prefix sums (serial; barrier already happened) - END pointer semantics
        uint32_t cumulative = 0;
        for (std::size_t i = 0; i < dir_size_; ++i) {
            cumulative += counts_[i];
            directory_offsets_[i] = cumulative;  // END pointer
        }

        tuples_.assign(cumulative, entry_type{});

        // Phase 3: one-writer-per-partition copy.
        threads.clear();
        for (std::size_t t = 0; t < workers; ++t) {
            threads.emplace_back([&, t]() {
                for (std::size_t slot = t; slot < dir_size_; slot += workers) {
                    // Start position = previous END (or 0 for first slot)
                    uint32_t pos = (slot == 0) ? 0 : directory_offsets_[slot - 1];
                    for (std::size_t src = 0; src < nthreads; ++src) {
                        for (Chunk* ch = lists[src][slot].head; ch; ch = ch->next) {
                            for (uint32_t i = 0; i < ch->size; ++i) {
                                tuples_[pos].key = ch->items[i].key;
                                tuples_[pos].row_id = ch->items[i].row_id;
                                ++pos;
                            }
                        }
                    }
                }
            });
        }
        for (auto& th : threads) th.join();
    }

    void build_from_zero_copy_int32_partitioned_parallel(const Column* src_column,
                                                         const std::vector<std::size_t>& page_offsets,
                                                         std::size_t num_rows) {
        if (num_rows == 0 || src_column == nullptr || page_offsets.size() < 2) {
            std::fill(directory_offsets_, directory_offsets_ + dir_size_, 0);
            std::fill(bloom_filters_.begin(), bloom_filters_.end(), 0);
            tuples_.clear();
            return;
        }

        std::size_t hw = std::thread::hardware_concurrency();
        if (!hw) hw = 4;
        const std::size_t nthreads = (num_rows < 2048) ? 1 : hw;
        std::vector<std::vector<ChunkList>> lists(nthreads, std::vector<ChunkList>(dir_size_));
        // 3-Level Slab Allocator (same as build_from_entries_partitioned_parallel)
        std::vector<std::unique_ptr<TempAlloc>> allocs;
        allocs.reserve(nthreads);
        for (std::size_t t = 0; t < nthreads; ++t) {
            allocs.emplace_back(std::make_unique<TempAlloc>());
        }

        const std::size_t block = (num_rows + nthreads - 1) / nthreads;
        std::vector<std::thread> threads;
        threads.reserve(nthreads);

        for (std::size_t t = 0; t < nthreads; ++t) {
            const std::size_t begin_row = t * block;
            const std::size_t end_row = std::min(num_rows, begin_row + block);
            if (begin_row >= end_row) break;
            threads.emplace_back([&, t, begin_row, end_row]() {
                auto& my_lists = lists[t];
                TempAlloc& my_alloc = *allocs[t];  // One allocator per thread

                // Find starting page by binary search.
                std::size_t page_idx = 0;
                if (begin_row >= page_offsets[1]) {
                    std::size_t left = 0, right = page_offsets.size() - 1;
                    while (left < right - 1) {
                        std::size_t mid = (left + right) / 2;
                        if (begin_row < page_offsets[mid]) right = mid;
                        else left = mid;
                    }
                    page_idx = left;
                }

                std::size_t base = page_offsets[page_idx];
                std::size_t next = page_offsets[page_idx + 1];
                auto* page = src_column->pages[page_idx]->data;
                auto* data = reinterpret_cast<const int32_t*>(page + 4);

                for (std::size_t row = begin_row; row < end_row; ++row) {
                    while (row >= next) {
                        ++page_idx;
                        base = page_offsets[page_idx];
                        next = page_offsets[page_idx + 1];
                        page = src_column->pages[page_idx]->data;
                        data = reinterpret_cast<const int32_t*>(page + 4);
                    }
                    const Key key = static_cast<Key>(data[row - base]);
                    const uint64_t h = compute_hash(key);
                    const std::size_t slot = (h >> shift_) & dir_mask_;
                    const uint16_t tag = Bloom::make_tag_from_hash(h);
                    // Allocate partition chunks from thread-local allocator
                    chunklist_push(my_lists[slot], TmpEntry{key, static_cast<uint32_t>(row), tag}, my_alloc);
                }
            });
        }
        for (auto& th : threads) th.join();

        if (counts_.size() != dir_size_) counts_.assign(dir_size_, 0);
        else std::fill(counts_.begin(), counts_.end(), 0);
        std::fill(bloom_filters_.begin(), bloom_filters_.end(), 0);

        threads.clear();
        const std::size_t workers = nthreads;
        for (std::size_t t = 0; t < workers; ++t) {
            threads.emplace_back([&, t]() {
                for (std::size_t slot = t; slot < dir_size_; slot += workers) {
                    uint32_t c = 0;
                    uint16_t bloom = 0;
                    for (std::size_t src = 0; src < nthreads; ++src) {
                        for (Chunk* ch = lists[src][slot].head; ch; ch = ch->next) {
                            c += ch->size;
                            for (uint32_t i = 0; i < ch->size; ++i) bloom |= ch->items[i].tag;
                        }
                    }
                    counts_[slot] = c;
                    bloom_filters_[slot] = bloom;
                }
            });
        }
        for (auto& th : threads) th.join();

        uint32_t cumulative = 0;
        for (std::size_t i = 0; i < dir_size_; ++i) {
            cumulative += counts_[i];
            directory_offsets_[i] = cumulative;  // END pointer
        }

        tuples_.assign(cumulative, entry_type{});

        threads.clear();
        for (std::size_t t = 0; t < workers; ++t) {
            threads.emplace_back([&, t]() {
                for (std::size_t slot = t; slot < dir_size_; slot += workers) {
                    // Start position = previous END (or 0 for first slot)
                    uint32_t pos = (slot == 0) ? 0 : directory_offsets_[slot - 1];
                    for (std::size_t src = 0; src < nthreads; ++src) {
                        for (Chunk* ch = lists[src][slot].head; ch; ch = ch->next) {
                            for (uint32_t i = 0; i < ch->size; ++i) {
                                tuples_[pos].key = ch->items[i].key;
                                tuples_[pos].row_id = ch->items[i].row_id;
                                ++pos;
                            }
                        }
                    }
                }
            });
        }
        for (auto& th : threads) th.join();
    }

    // Removed unused probe_exact() helper (not referenced anywhere)

    std::size_t size() const { return tuples_.size(); }
    
    // Debugging helpers
    std::size_t directory_size() const { return dir_size_; }
    std::size_t memory_usage() const {
        return tuples_.size() * sizeof(entry_type) +
               dir_size_ * sizeof(uint32_t) +
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
    
    // FLAT STORAGE - Optimized layout (tuples_ uses slab-aware allocator with runtime toggle)
    std::vector<entry_type, entry_allocator> tuples_; // Contiguous tuples (sorted by prefix)
    
    // Directory with directory[-1] support (PDF requirement 8.3)
    std::vector<uint32_t> directory_buffer_;    // Storage buffer (size = dir_size + 2)
    uint32_t* directory_offsets_;               // Shifted pointer (allows directory_offsets_[-1])
    
    std::vector<uint16_t> bloom_filters_;       // Bloom filters (size = dir_size)

    // Reused scratch buffers to avoid per-build allocations.
    std::vector<uint32_t> counts_;              // Counts per slot
    std::vector<uint32_t> write_ptrs_;          // Write pointers per slot (serial path)
    
    std::size_t dir_size_;   // Number of directory slots
    std::size_t dir_mask_;   // Bitmask for prefix extraction
    std::size_t shift_;      // Shift amount for prefix extraction
};

// Alias για backward compatibility
template<typename Key, typename Hasher = Hash::Hasher32>
using UnchainedHashTable = FlatUnchainedHashTable<Key, Hasher>;

// unchained_hashtable.h
#pragma once
#include <vector>
#include <cstdint>
#include <cstddef>
#include <type_traits>
#include <stdexcept>
#include <algorithm>
#include "hash_functions.h"
#include "bloom_filter.h"

// Minimal unchained hashtable: directory of ranges + contiguous buffer of tuples
// Single partition only. 16-bit bloom per directory entry. Template-based.

template<typename Key, typename Hasher = Hash::Hasher32>
struct TupleEntry {
    Key key;
    std::size_t row_id;
};

struct DirectoryEntry {
    // begin/end are pointers into tuples vector; if empty, begin==end
    // we store as indices to make single-header safe
    std::size_t begin_idx;
    std::size_t end_idx;  // one-past-end
    uint16_t bloom;
};

template<typename Key, typename Hasher = Hash::Hasher32>
class UnchainedHashTable {
public:
    using entry_type = TupleEntry<Key>;
    using dir_entry  = DirectoryEntry;

    explicit UnchainedHashTable(Hasher hasher = Hasher(), std::size_t directory_power = 10)
        : hasher_(hasher) {
        // directory_power: initial log2(directory_size). We will grow if needed in reserve().
        dir_size_ = 1ull << directory_power;
        dir_mask_ = dir_size_ - 1;
        directory_.assign(dir_size_, {0,0,0});
    }

    // Reserve capacity for approximate number of tuples. directory size will be at least
    // the next power-of-two >= dir_size_hint (so prefix space).
    void reserve(std::size_t tuples_capacity) {
        // make directory size approx tuples_capacity rounded to power-of-two if larger than current
        std::size_t desired_dir = 1;
        while (desired_dir < tuples_capacity && desired_dir < (1ull<<30)) desired_dir <<= 1;
        if (desired_dir > dir_size_) {
            dir_size_ = desired_dir;
            dir_mask_ = dir_size_ - 1;
            directory_.assign(dir_size_, {0,0,0});
        }
        tuples_.reserve(tuples_capacity);
    }

    // Build from vector of (key,rowid). This is the recommended usage for clarity.
    void build_from_entries(const std::vector<std::pair<Key,std::size_t>>& entries) {
        if (entries.empty()) {
            // empty: zero out directory
            for (auto &d : directory_) { d.begin_idx = d.end_idx = 0; d.bloom = 0; }
            tuples_.clear();
            return;
        }

        // 1) compute hashes and prefix counts
        std::vector<uint64_t> hashes;
        hashes.reserve(entries.size());
        for (auto &e: entries) {
            hashes.push_back(hasher_(static_cast<int32_t>(e.first)));
        }

        // count per prefix
        std::vector<std::size_t> counts(dir_size_, 0);
        for (auto &h: hashes) {
            std::size_t prefix = static_cast<std::size_t>((h >> 16) & dir_mask_);
            counts[prefix]++;
        }

        // 2) prefix sums -> offsets
        std::vector<std::size_t> offsets(dir_size_);
        std::size_t total = 0;
        for (std::size_t i = 0; i < dir_size_; ++i) {
            offsets[i] = total;
            total += counts[i];
        }

        // 3) allocate tuple buffer exactly
        tuples_.assign(total, entry_type{Key(), 0});

        // 4) second pass: fill tuples & bloom
        std::vector<std::size_t> write_ptr = offsets;
        for (std::size_t i = 0; i < entries.size(); ++i) {
            const auto &pr = entries[i];
            uint64_t h = hashes[i];
            std::size_t prefix = static_cast<std::size_t>((h >> 16) & dir_mask_);
            std::size_t pos = write_ptr[prefix]++;
            tuples_[pos].key = pr.first;
            tuples_[pos].row_id = pr.second;

            // add bloom tag
            uint16_t tag = Bloom::make_tag_from_hash(h);
            directory_[prefix].bloom |= tag;
        }

        // 5) populate directory begin/end indices
        for (std::size_t i = 0; i < dir_size_; ++i) {
            directory_[i].begin_idx = offsets[i];
            directory_[i].end_idx   = (i + 1 < dir_size_) ? offsets[i+1] : total;
            // if count==0 begin==end==offsets[i] so empty
        }
    }

    // Alternative build: from 2D rows where key is at key_col. This matches some simple row-store tests.
    // Note: expects Key to be convertible from the stored value type. For simplicity we accept rows as vector<Key>.
    void build_from_rows(const std::vector<std::vector<Key>>& rows, std::size_t key_col) {
        std::vector<std::pair<Key,std::size_t>> entries;
        entries.reserve(rows.size());
        for (std::size_t r = 0; r < rows.size(); ++r) {
            if (key_col >= rows[r].size()) throw std::out_of_range("key_col out of row bounds");
            entries.emplace_back(rows[r][key_col], r);
        }
        build_from_entries(entries);
    }

    // Probe: given key, return pointer (index) to start and length via out param len.
    // If none, returns (nullptr, len=0) equivalently returns begin index==end index.
    const entry_type* probe(const Key& key, std::size_t &len) const {
        uint64_t h = compute_hash(key);
        std::size_t prefix = static_cast<std::size_t>((h >> 16) & dir_mask_);
        const dir_entry &de = directory_[prefix];
        if (de.begin_idx >= de.end_idx) { len = 0; return nullptr; } // empty bucket

        uint16_t tag = Bloom::make_tag_from_hash(h);
        if (!Bloom::maybe_contains(de.bloom, tag)) { len = 0; return nullptr; }

        // possible matches: perform linear scan in bucket
        std::size_t begin = de.begin_idx;
        std::size_t end   = de.end_idx;
        len = end - begin;
        return (len==0) ? nullptr : &tuples_[begin];
    }

    // Return exact matching entries (vector of row_ids) by scanning bucket and comparing keys
    std::vector<std::size_t> probe_exact(const Key& key) const {
        std::size_t len;
        const entry_type* base = probe(key, len);
        std::vector<std::size_t> out;
        if (!base || len == 0) return out;

        for (std::size_t i = 0; i < len; ++i) {
            if (base[i].key == key) out.push_back(base[i].row_id);
        }
        return out;
    }

    std::size_t size() const noexcept { return tuples_.size(); }
    std::size_t dir_size() const noexcept { return dir_size_; }

private:
    uint64_t compute_hash(const Key& k) const {
        // if Key is int32_t use Hasher directly, else fallback to std::hash
        if constexpr (std::is_same_v<Key, int32_t> || std::is_same_v<Key, uint32_t>) {
            return hasher_(static_cast<int32_t>(k));
        } else {
            // generic fallback
            return static_cast<uint64_t>(std::hash<Key>{}(k));
        }
    }

    Hasher hasher_;
    std::vector<entry_type> tuples_;
    std::vector<dir_entry> directory_;
    std::size_t dir_size_;
    std::size_t dir_mask_;
};


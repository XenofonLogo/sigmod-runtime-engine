#pragma once
#include <vector>
#include <cstdint>
#include <cstddef>
#include <type_traits>
#include <stdexcept>
#include <algorithm>
#include "hash_functions.h"
#include "bloom_filter.h"

template<typename Key, typename Hasher = Hash::Hasher32>
struct TupleEntry {
    Key key;
    std::size_t row_id;
};

struct DirectoryEntry {
    std::size_t begin_idx;
    std::size_t end_idx;
    uint16_t bloom;
};

template<typename Key, typename Hasher = Hash::Hasher32>
class UnchainedHashTable {
public:
    using entry_type = TupleEntry<Key>;
    using dir_entry  = DirectoryEntry;

    explicit UnchainedHashTable(Hasher hasher = Hasher(), std::size_t directory_power = 10)
        : hasher_(hasher)
    {
        dir_size_ = 1ull << directory_power;
        dir_mask_ = dir_size_ - 1;
        directory_.assign(dir_size_, {0,0,0});
    }

    void reserve(std::size_t tuples_capacity) {
        std::size_t desired = 1;
        while (desired < tuples_capacity && desired < (1ull << 30)) desired <<= 1;

        if (desired > dir_size_) {
            dir_size_ = desired;
            dir_mask_ = dir_size_ - 1;
            directory_.assign(dir_size_, {0,0,0});
        }
        tuples_.reserve(tuples_capacity);
    }

    void build_from_entries(const std::vector<std::pair<Key,std::size_t>>& entries) {
        if (entries.empty()) {
            for (auto &d : directory_) d = {0,0,0};
            tuples_.clear();
            return;
        }

        std::vector<uint64_t> hashes;
        hashes.reserve(entries.size());
        for (auto &e: entries)
            hashes.push_back(compute_hash(e.first));

        std::vector<std::size_t> counts(dir_size_, 0);
        for (auto &h: hashes) {
            std::size_t prefix = (h >> 16) & dir_mask_;
            counts[prefix]++;
        }

        std::vector<std::size_t> offsets(dir_size_, 0);
        std::size_t total = 0;
        for (std::size_t i = 0; i < dir_size_; ++i) {
            offsets[i] = total;
            total += counts[i];
        }

        tuples_.assign(total, entry_type{});

        std::vector<std::size_t> write_ptr = offsets;
        for (std::size_t i = 0; i < entries.size(); ++i) {
            uint64_t h = hashes[i];
            std::size_t prefix = (h >> 16) & dir_mask_;
            std::size_t pos = write_ptr[prefix]++;
            tuples_[pos].key = entries[i].first;
            tuples_[pos].row_id = entries[i].second;

            directory_[prefix].bloom |= Bloom::make_tag_from_hash(h);
        }

        for (std::size_t i = 0; i < dir_size_; ++i) {
            directory_[i].begin_idx = offsets[i];
            directory_[i].end_idx   = (i+1 < dir_size_) ? offsets[i+1] : total;
        }
    }

    void build_from_rows(const std::vector<std::vector<Key>>& rows, std::size_t key_col) {
        std::vector<std::pair<Key,std::size_t>> entries;
        entries.reserve(rows.size());
        for (std::size_t r = 0; r < rows.size(); ++r) {
            entries.emplace_back(rows[r][key_col], r);
        }
        build_from_entries(entries);
    }

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

    std::vector<std::size_t> probe_exact(const Key& key) const {
        std::size_t len;
        const entry_type* base = probe(key, len);

        std::vector<std::size_t> result;
        if (!base) return result;

        for (std::size_t i = 0; i < len; ++i)
            if (base[i].key == key)
                result.push_back(base[i].row_id);

        return result;
    }

    std::size_t size() const { return tuples_.size(); }

private:
    uint64_t compute_hash(const Key& k) const {
        if constexpr (std::is_same_v<Key, int32_t> || std::is_same_v<Key, uint32_t>) {
            return hasher_(static_cast<int32_t>(k));
        } else {
            return static_cast<uint64_t>(std::hash<Key>{}(k));
        }
    }

    Hasher hasher_;
    std::vector<entry_type> tuples_;
    std::vector<dir_entry> directory_;
    std::size_t dir_size_;
    std::size_t dir_mask_;
};

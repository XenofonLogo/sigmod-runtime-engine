
#pragma once


#include "hashtable_interface.h" 
#include "robinhood.h" 
#include <memory>

namespace Contest {

template <typename Key>
class RobinHoodHashTableWrapper : public IHashTable<Key> {
private:
    RobinHoodBackend<Key> backend_;

public:
    // Implementation of IHashTable methods
    void reserve(size_t capacity) override {
        // The RobinHoodBackend doesn't have a direct 'reserve' method, 
        // but it will handle capacity calculation in build_from_entries.
        // We can leave this empty or throw, but for compatibility, we leave it empty.
        // If the backend required pre-allocation, the logic would go here.
    }

    bool build_from_zero_copy_int32(const Column* src_column,
                                    const std::vector<std::size_t>& page_offsets,
                                    std::size_t num_rows) override {
        // RobinHood backend doesn't support zero-copy directly, so we materialize entries
        if (num_rows == 0 || src_column == nullptr || page_offsets.size() < 2) {
            return false;
        }

        std::vector<HashEntry<Key>> entries;
        entries.reserve(num_rows);

        const std::size_t npages = page_offsets.size() - 1;
        for (std::size_t page_idx = 0; page_idx < npages; ++page_idx) {
            const std::size_t base = page_offsets[page_idx];
            const std::size_t end = page_offsets[page_idx + 1];
            const std::size_t n = end - base;
            auto* page = src_column->pages[page_idx]->data;
            auto* data = reinterpret_cast<const int32_t*>(page + 4);

            for (std::size_t i = 0; i < n; ++i) {
                entries.push_back({static_cast<Key>(data[i]), static_cast<uint32_t>(base + i)});
            }
        }

        build_from_entries(entries);
        return true;
    }

    void build_from_entries(const std::vector<HashEntry<Key>>& entries) override {
        // Backend expects (key,row_id) pairs.
        // Adapter layer: convert HashEntry -> pair. This is not the hot path in xenofon1 (default is unchained).
        std::vector<std::pair<Key, size_t>> pairs;
        pairs.reserve(entries.size());
        for (const auto &e : entries) pairs.emplace_back(e.key, static_cast<size_t>(e.row_id));
        backend_.build_from_entries(pairs);
    }

    const HashEntry<Key>* probe(const Key& key, size_t& len) const override {
        // The RobinHoodBackend returns a std::pair, so we unwrap it.
        auto result = backend_.probe(key);
        
        // Update the reference argument (len) with the count from the pair.
        len = result.second; 
        
        // Return the pointer to the HashEntry
        return result.first; 
    }
};


template <typename Key>
std::unique_ptr<IHashTable<Key>> create_hashtable() {
    
    return std::make_unique<RobinHoodHashTableWrapper<Key>>();
}

} // namespace Contest
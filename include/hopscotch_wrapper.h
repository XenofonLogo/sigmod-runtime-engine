#pragma once


#include "hashtable_interface.h" 
#include "hopscotch.h" 
#include <memory>
#include <stdexcept>

namespace Contest {

template <typename Key>
class HopscotchHashTableWrapper : public IHashTable<Key> {
private:
    HopscotchBackend<Key> backend_;

public:
    // IHashTable Interface Implementation
    
    // Hopscotch Backend doesn't have a direct reserve, implementation goes into build.
    void reserve(size_t capacity) override {
        // Ignored, as Hopscotch manages capacity internally during build
    }

    bool build_from_zero_copy_int32(const Column* src_column,
                                    const std::vector<std::size_t>& page_offsets,
                                    std::size_t num_rows) override {
        // Hopscotch backend doesn't support zero-copy directly, so we materialize entries
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
        // (GR) Adapter layer: μετατροπή HashEntry -> pair για legacy backend.
        // Κρατάμε το interface ενιαίο ώστε ο executor να είναι βελτιστοποιημένος.
        std::vector<std::pair<Key, size_t>> pairs;
        pairs.reserve(entries.size());
        for (const auto &e : entries) pairs.emplace_back(e.key, static_cast<size_t>(e.row_id));
        backend_.build_from_entries(pairs);
    }

    const HashEntry<Key>* probe(const Key& key, size_t& len) const override {
        // The HopscotchBackend returns a std::pair, so we unwrap it.
        auto result = backend_.probe(key);
        
        // Update the reference argument (len) with the count from the pair.
        len = result.second; 
        
        // Return the pointer to the HashEntry
        return result.first; 
    }
};


template <typename Key>
std::unique_ptr<IHashTable<Key>> create_hashtable() {
    // FIX: This is where we plug in the Hopscotch implementation
    return std::make_unique<HopscotchHashTableWrapper<Key>>();
}

} // namespace Contest
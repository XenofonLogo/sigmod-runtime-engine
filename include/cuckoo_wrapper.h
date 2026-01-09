// cuckoo_wrapper.h
#pragma once

#include "hashtable_interface.h" 
#include "cuckoo.h" 
#include <memory>
#include <stdexcept>

namespace Contest {

template <typename Key>
class CuckooHashTableWrapper : public IHashTable<Key> {
private:
    CuckooBackend<Key> backend_;

public:
    // IHashTable Interface Implementation
    
    // Cuckoo Backend doesn't have a direct reserve, implementation goes into build.
    void reserve(size_t capacity) override {
        // Ignored or can be used to set an initial capacity hint if implemented in backend
    }

    void build_from_entries(const std::vector<HashEntry<Key>>& entries) override {
        std::vector<std::pair<Key, size_t>> pairs;
        pairs.reserve(entries.size());
        for (const auto &e : entries) pairs.emplace_back(e.key, static_cast<size_t>(e.row_id));
        backend_.build_from_entries(pairs);
    }

    const HashEntry<Key>* probe(const Key& key, size_t& len) const override {
        // The CuckooBackend returns a std::pair, so we unwrap it.
        auto result = backend_.probe(key);
        
        // Update the reference argument (len) with the count from the pair.
        len = result.second; 
        
        // Return the pointer to the HashEntry
        return result.first; 
    }
};

template <typename Key>
std::unique_ptr<IHashTable<Key>> create_hashtable() {
   
    return std::make_unique<CuckooHashTableWrapper<Key>>();
}

} // namespace Contest
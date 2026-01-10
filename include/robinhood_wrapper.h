
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

    void build_from_entries(const std::vector<HashEntry<Key>>& entries) override {
        // Backend expects (key,row_id) pairs.
        // (GR) Adapter layer: HashEntry -> pair. Δεν είναι το hot-path στο xenofon1 (default είναι unchained).
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
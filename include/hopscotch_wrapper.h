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

    void build_from_entries(const std::vector<std::pair<Key, size_t>>& entries) override {
        // Delegate directly to the HopscotchBackend's build method
        backend_.build_from_entries(entries);
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
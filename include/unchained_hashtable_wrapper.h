
#pragma once

#include "hashtable_interface.h"
#include "parallel_unchained_hashtable.h" 
#include "hash_functions.h"
#include "hash_common.h"
#include <memory>

namespace Contest {

template <typename Key>
class UnchainedHashTableWrapper : public IHashTable<Key> {
private:
    UnchainedHashTable<Key> table_;

public:
    // Implementation of IHashTable methods
    void reserve(size_t capacity) override {
        table_.reserve(capacity);
    }

    void build_from_entries(const std::vector<std::pair<Key, size_t>>& entries) override {
       
        table_.build_from_entries(entries);
    }

    const HashEntry<Key>* probe(const Key& key, size_t& len) const override {
        //  Get the pointer to the internal entry type
        const auto* internal_bucket = table_.probe(key, len);

        
        return reinterpret_cast<const HashEntry<Key>*>(internal_bucket);
    }
};


template <typename Key>
std::unique_ptr<IHashTable<Key>> create_hashtable() {
    return std::make_unique<UnchainedHashTableWrapper<Key>>();
}

} // namespace Contest
// unchained_hashtable_wrapper.h
#pragma once

#include "hashtable_interface.h"
#include "unchained_hashtable.h" // Keep the original header here!
#include "hash_functions.h"
#include "hash_common.h"
#include <memory>

namespace Contest {



public:
    // Implementation of IHashTable methods
    void reserve(size_t capacity) override {
        table_.reserve(capacity);
    }

    void build_from_entries(const std::vector<std::pair<Key, size_t>>& entries) override {
        // The wrapper needs to convert the std::pair to the HashEntry used by the internal table
        // We will assume the entries in the wrapper are compatible with the internal table's entry
        // If UnchainedHashTable uses a different internal structure, you'd need to adapt here.
        // Assuming your original `UnchainedHashTable<Key>` internally handles the `std::pair<Key, size_t>`
        // or uses a structure equivalent to `HashEntry<Key>`.
        // Based on your original code: `table.build_from_entries(entries);` suggests it takes a vector of pairs.
        table_.build_from_entries(entries);
    }

    const HashEntry<Key>* probe(const Key& key, size_t& len) const override {
        // 1. Get the pointer to the internal entry type
        const auto* internal_bucket = table_.probe(key, len);

        // 2. Safely cast the internal pointer type to the external interface type (HashEntry<Key>*)
        // This is safe because both HashEntry and TupleEntry/entry_type should have
        // the same Key and row_id members as their first two members.
        // NOTE: This relies on the memory layout being identical.
        return reinterpret_cast<const HashEntry<Key>*>(internal_bucket);
    }
};


template <typename Key>
std::unique_ptr<IHashTable<Key>> create_hashtable() {
    return std::make_unique<UnchainedHashTableWrapper<Key>>();
}

} // namespace Contest
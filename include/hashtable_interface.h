// hashtable_interface.h
#pragma once

#include <vector>
#include <utility>
#include <cstddef>
#include <stdexcept>
#include "hash_common.h"
namespace Contest {



// Abstract base class for the hash table interface
template <typename Key>
class IHashTable {
public:
    virtual ~IHashTable() = default;

    // Reserves space for the expected number of entries.
    virtual void reserve(size_t capacity) = 0;

    // Builds the hash table from a vector of (key, row_id) entries.
    virtual void build_from_entries(const std::vector<HashEntry<Key>>& entries) = 0;

    // Probes the hash table for a key.
    // len is set to the number of entries found for the key.
    // Returns a pointer to the start of the bucket/chain, or nullptr if not found.
    virtual const HashEntry<Key>* probe(const Key& key, size_t& len) const = 0;
};

template <typename Key>
// Declaration only:
std::unique_ptr<IHashTable<Key>> create_hashtable();

} // namespace Contest
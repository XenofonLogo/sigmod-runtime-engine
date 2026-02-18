// hashtable_interface.h
#pragma once

#include <vector>
#include <utility>
#include <cstddef>
#include <stdexcept>
#include "hash_common.h"

// Column is defined in the engine headers (global namespace).
struct Column;

namespace Contest {



// Abstract base class for the hash table interface
template <typename Key>
class IHashTable {
public:
    virtual ~IHashTable() = default;

    // Reserves space for the expected number of entries.
    virtual void reserve(size_t capacity) = 0;

    // Builds the hash table from a vector of (key, row_id) entries.
    // We use struct HashEntry instead of std::pair to:
    // - have a stable memory layout and a smaller row_id (uint32_t)
    // - avoid unnecessary conversions/allocations in wrapper adapters
    virtual void build_from_entries(const std::vector<HashEntry<Key>>& entries) = 0;

    // Optional fast-path for INT32 columns with no NULLs.
    // Implementations that don't support it should keep the default (return false).
    virtual bool build_from_zero_copy_int32(const Column* /*src_column*/,
                                           const std::vector<std::size_t>& /*page_offsets*/,
                                           std::size_t /*num_rows*/) {
        return false;
    }

    // Probes the hash table for a key.
    // len is set to the number of entries found for the key.
    // Returns a pointer to the start of the bucket/chain, or nullptr if not found.
    virtual const HashEntry<Key>* probe(const Key& key, size_t& len) const = 0;
};

template <typename Key>
// Declaration only:
std::unique_ptr<IHashTable<Key>> create_hashtable();

} // namespace Contest
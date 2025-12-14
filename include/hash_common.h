// hash_common.h
#pragma once

#include <cstddef>
#include <cstdint> // For fixed-width types like int32_t

namespace Contest {

// 1. Common Entry Structure (Payload)
// This must be identical to the one used by IHashTable
template <typename Key>
struct HashEntry {
    Key key;
    size_t row_id; // The row ID from the build side
};

// 2. Index/Metadata Structure (Specific to Robin Hood, but defined here for clarity)
// This holds the information stored in the hash table array slots.
template<typename Key>
struct KeyIndexInfo {
    Key key;          // The key stored in the hash table slot
    size_t start_index; // Index into the separate storage array
    size_t count;       // Number of rows associated with this key
    bool is_valid = false; // Whether this slot is occupied
};

} // namespace Contest
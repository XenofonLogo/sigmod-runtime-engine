
#pragma once

#include <cstddef>
#include <cstdint> // For fixed-width types like int32_t

namespace Contest {


// This must be identical to the one used by IHashTable
template <typename Key>
struct HashEntry {
    Key key;
    uint32_t row_id; // The row ID from the build side (fits all contest tables)
};

// This holds the information stored in the hash table array slots.
template<typename Key>
struct KeyIndexInfo {
    Key key;          // The key stored in the hash table slot
    size_t start_index; // Index into the separate storage array
    size_t count;       // Number of rows associated with this key
    bool is_valid = false; // Whether this slot is occupied
};

} // namespace Contest
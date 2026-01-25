#ifndef TEMP_ALLOCATOR_H
#define TEMP_ALLOCATOR_H

#include <cstddef>
#include <cstdint>
#include <vector>

namespace Contest {

// Slab allocator προσωρινής μνήμης ανά build: δεσμεύει σε chunks και ελευθερώνει μαζικά στο τέλος.
// Χρησιμοποιείται από partition_hash_builder για λίστες chunk ανά partition (βημα STRICT).
struct TempAlloc {
    static constexpr size_t SLAB_SIZE = 1 << 20; // 1MB slabs
    
    std::vector<void*> slabs;   // Κρατά τους δείκτες των slab
    std::byte* current = nullptr; // Τρέχουσα θέση γραφής στο slab
    size_t remaining = 0;         // Υπόλοιπο bytes στο τρέχον slab

    TempAlloc() = default;

    // Allocate memory with alignment
    void* alloc(std::size_t bytes, std::size_t align);

    // Destructor frees all slabs at once
    ~TempAlloc();
};

} // namespace Contest

#endif // TEMP_ALLOCATOR_H

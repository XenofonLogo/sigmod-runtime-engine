#ifndef TEMP_ALLOCATOR_H
#define TEMP_ALLOCATOR_H

#include <cstddef>
#include <cstdint>
#include <vector>

namespace Contest {

// Per-build slab allocator for temporary storage
// Allocates in chunks, frees all at once when destroyed
// Per-build slab allocator for temporary storage.
// Lives on top of ThreeLevelSlab's thread arena: used by partition_hash_builder to allocate
// per-partition chunk lists during the required partitioned hash build.
struct TempAlloc {
    static constexpr size_t SLAB_SIZE = 1 << 20; // 1MB slabs
    
    std::vector<void*> slabs;
    std::byte* current = nullptr;
    size_t remaining = 0;

    TempAlloc() = default;

    // Allocate memory with alignment
    void* alloc(std::size_t bytes, std::size_t align);

    // Destructor frees all slabs at once
    ~TempAlloc();
};

} // namespace Contest

#endif // TEMP_ALLOCATOR_H

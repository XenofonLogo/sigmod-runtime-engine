#include "temp_allocator.h"
#include <algorithm>

namespace Contest {

void* TempAlloc::alloc(std::size_t bytes, std::size_t align) {
    const std::size_t aligned = (bytes + (align - 1)) & ~(align - 1);
    
    if (remaining < aligned) {
        // Allocate new slab
        const size_t slab_bytes = std::max(aligned, SLAB_SIZE);
        void* slab = ::operator new(slab_bytes);
        slabs.push_back(slab);
        current = static_cast<std::byte*>(slab);
        remaining = slab_bytes;
    }

    void* ptr = current;
    current += aligned;
    remaining -= aligned;
    return ptr;
}

TempAlloc::~TempAlloc() {
    // Free all slabs at once
    for (void* slab : slabs) {
        ::operator delete(slab);
    }
}

} // namespace Contest

#include "temp_allocator.h"
#include <algorithm>

namespace Contest {

void* TempAlloc::alloc(std::size_t bytes, std::size_t align) {
    // Υπολογίζει το μέγεθος με ευθυγράμμιση (alignment)
    const std::size_t aligned = (bytes + (align - 1)) & ~(align - 1);
    // Αν δεν υπάρχει αρκετός χώρος στο τρέχον slab, δεσμεύει νέο
    if (remaining < aligned) {
        const size_t slab_bytes = std::max(aligned, SLAB_SIZE);
        void* slab = ::operator new(slab_bytes);
        slabs.push_back(slab);
        current = static_cast<std::byte*>(slab);
        remaining = slab_bytes;
    }
    // Επιστρέφει pointer στη νέα μνήμη και ενημερώνει τον δείκτη
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

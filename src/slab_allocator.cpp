#include "slab_allocator.h"
#include <algorithm>

namespace Contest {

// PartitionArena::alloc (Επίπεδο 3: Per-partition δέσμευση)
// Δεσμεύει μνήμη από το ανατεθειμένο block, ή ζητά νέο από parent allocator
void* PartitionArena::alloc(std::size_t bytes, std::size_t align) {
    // Υπολογισμός offset για ευθυγράμμιση του pointer
    const std::size_t offset = ((reinterpret_cast<uintptr_t>(current) + (align - 1)) & ~(align - 1))
                             - reinterpret_cast<uintptr_t>(current);
    const std::size_t aligned_bytes = (bytes + (align - 1)) & ~(align - 1);
    const std::size_t total_needed = offset + aligned_bytes;
    
    // Αν υπάρχει χώρος στο τρέχον block, χρησιμοποίησε το
    if (current && remaining >= total_needed) {
        std::byte* ptr = current + offset;
        current += total_needed;
        remaining -= total_needed;
        return ptr;
    }
    
    // Αλλιώς, ζήτησε νέο block από το parent thread allocator (request max alignment)
    if (parent) {
        const std::size_t block_size = parent->SLAB_SIZE;
        void* new_block = parent->alloc(block_size, 64);  // Request 64-byte aligned block
        owned_blocks.push_back(new_block);
        current = static_cast<std::byte*>(new_block);
        remaining = block_size;
        
        // Block is 64-byte aligned, now allocate from it with proper alignment
        const std::size_t offset2 = ((reinterpret_cast<uintptr_t>(current) + (align - 1)) & ~(align - 1))
                       - reinterpret_cast<uintptr_t>(current);
        std::byte* ptr = current + offset2;
        current += offset2 + aligned_bytes;
        remaining -= (offset2 + aligned_bytes);
        return ptr;
    }
    
    return nullptr;  // Σφάλμα: δεν υπάρχει parent allocator
}

// SlabAllocator::SlabAllocator (Επίπεδο 2: Per-thread allocator)
// Αρχικοποίηση: ορίζει parent allocator για όλες τις partition arenas
SlabAllocator::SlabAllocator() : current(nullptr), remaining(0) {
    // Ορίζουμε το parent pointer για κάθε partition arena
    for (size_t i = 0; i < NUM_PARTITIONS; ++i) {
        partitions[i].parent = this;
        partitions[i].current = nullptr;
        partitions[i].remaining = 0;
    }
}

// SlabAllocator::alloc (Επίπεδο 2: Thread-local bulk allocation)
// Δεсμеύει μνήμη σε slabs (1MB blocks από operator new)
void* SlabAllocator::alloc(std::size_t bytes, std::size_t align) {
    // Ensure align is at least 1
    if (align < 1) align = 1;
    
    // Υπολογίζει μέγεθος με ευθυγράμμιση (power-of-two align μόνο)
    const std::size_t aligned_bytes = (bytes + (align - 1)) & ~(align - 1);
    
    // Υπολογισμός offset για ευθυγράμμιση του pointer
    const std::size_t offset = ((reinterpret_cast<uintptr_t>(current) + (align - 1)) & ~(align - 1))
                             - reinterpret_cast<uintptr_t>(current);
    const std::size_t total_needed = offset + aligned_bytes;
    
    // Αν δεν φτάνει ο χώρος στο τρέχον slab, δεσμεύει νέο
    if (remaining < total_needed) {
        const size_t slab_bytes = SLAB_SIZE;
        // Allocate extra space for alignment
        void* raw_slab = ::operator new(slab_bytes + align);
        slabs.push_back(raw_slab);
        // Align the pointer
        std::byte* aligned_slab = reinterpret_cast<std::byte*>(
            ((reinterpret_cast<uintptr_t>(raw_slab) + (align - 1)) & ~(align - 1))
        );
        current = aligned_slab;
        remaining = slab_bytes;
    }
    
    // Επιστροφή pointer με ευθυγράμμιση και μετακίνηση cursor
    std::byte* ptr = current + offset;
    current += offset + aligned_bytes;
    remaining -= offset + aligned_bytes;
    return ptr;
}

// SlabAllocator::~SlabAllocator (Bulk free: Επίπεδο 1 απελευθέρωση)
// Ελευθερώνει όλα τα slabs μαζικά μετά την ολοκλήρωση της κατασκευής
SlabAllocator::~SlabAllocator() {
    // Καθαρισμός partition arenas (ενημέρωση καταστάσεων)
    for (size_t i = 0; i < NUM_PARTITIONS; ++i) {
        partitions[i].current = nullptr;
        partitions[i].remaining = 0;
        partitions[i].owned_blocks.clear();
    }
    
    // Ελευθερώνει όλα τα slabs μαζικά (bulk free)
    for (void* slab : slabs) {
        ::operator delete(slab);
    }
}

} // namespace Contest

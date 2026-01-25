#ifndef SLAB_ALLOCATOR_H
#define SLAB_ALLOCATOR_H

#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <vector>

namespace Contest {

// Forward declaration: SlabAllocator αναφέρεται σε PartitionArena παρακάτω
class SlabAllocator;

// 3-Επίπεδος Slab Allocator για Partition-Based Build (STRICT mode)
// 
// Αρχιτεκτονική:
// - Επίπεδο 1 (Global): operator new (δεν διαχειρίζεται εδώ, χρησιμοποιείται από operator new/delete)
// - Επίπεδο 2 (Per-Thread): SlabAllocator με thread-local slabs (1MB blocks)
// - Επίπεδο 3 (Per-Partition): PartitionArena δέσμευση από το thread SlabAllocator
//
// Ροή: Thread SlabAllocator → PartitionArena (sub-allocator) → individual chunks

// Per-partition allocator arena (Επίπεδο 3: Partition-specific memory)
// Κάθε partition δεσμεύει από τον parent thread allocator
struct PartitionArena {
    std::byte* current = nullptr;      // Τρέχουσα θέση γραφής στο ανατεθειμένο block
    size_t remaining = 0;              // Υπόλοιπο bytes στο block
    std::vector<void*> owned_blocks;   // Blocks που ανήκουν σε αυτήν την partition
    SlabAllocator* parent = nullptr;   // Αναφορά σε parent thread allocator

    // Δέσμευση μνήμης από τήν partition arena
    void* alloc(std::size_t bytes, std::size_t align);
};

// Slab allocator προσωρινής μνήμης για partition-based build (Επίπεδο 2: Per-Thread)
// Δεσμεύει σε blocks και ελευθερώνει μαζικά στο τέλος.
// Χρησιμοποιείται από partition_hash_builder για λίστες chunk ανά partition (βήμα STRICT).
struct SlabAllocator {
    static constexpr size_t SLAB_SIZE = 1 << 20;  // 1MB slabs (Επίπεδο 2 block size)
    
    // Allow runtime override for experiments via NUM_PARTITIONS_OVERRIDE
    static constexpr size_t DEFAULT_PARTITIONS = 64;  // Optimal per experiments
    static size_t get_num_partitions() {
        const char* env = std::getenv("NUM_PARTITIONS_OVERRIDE");
        if (env && *env) {
            return static_cast<size_t>(std::atoi(env));
        }
        return DEFAULT_PARTITIONS;
    }
    
    // Static array sized to max possible partitions (512)
    // We'll only use up to get_num_partitions() at runtime
    static constexpr size_t NUM_PARTITIONS = 64;  // Optimal (experiments showed 64 > 128, 256)
    
    std::vector<void*> slabs;          // Κρατά τους δείκτες των slab (Επίπεδο 1 allocations)
    std::byte* current = nullptr;      // Τρέχουσα θέση γραφής στο τρέχον slab
    size_t remaining = 0;              // Υπόλοιπο bytes στο τρέχον slab
    
    // Επίπεδο 3: Per-partition arenas για ξεχωριστή δέσμευση ανά partition
    PartitionArena partitions[NUM_PARTITIONS];

    SlabAllocator();

    // Allocate memory with alignment (Επίπεδο 2: Thread-level bulk allocation)
    void* alloc(std::size_t bytes, std::size_t align);

    // Δημιουργία per-partition arena για χρήση ανά partition
    PartitionArena& get_partition_arena(size_t partition_id) {
        if (partition_id >= NUM_PARTITIONS) partition_id = 0;
        partitions[partition_id].parent = this;
        return partitions[partition_id];
    }

    // Destructor frees all slabs at once (bulk free στο τέλος)
    ~SlabAllocator();
};

} // namespace Contest

#endif // SLAB_ALLOCATOR_H

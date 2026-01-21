#ifndef PARTITION_BUILD_H
#define PARTITION_BUILD_H

#include <cstdlib>

namespace Contest {

// Check if partition build is enabled via environment variable
// Default: disabled (requires explicit REQ_PARTITION_BUILD=1)
// 
// Partition build implements a 3-phase parallel hash table construction:
// Phase 1: Parallel partitioning of input entries into local buckets
// Phase 2: Thread synchronization (join)
// Phase 3: Parallel per-partition hash table population
//
// This is implemented in parallel_unchained_hashtable.h:
// - build_from_entries_partitioned_parallel()
// - Uses TempAlloc with per-build slab allocator (1MB slabs)
// - Implements work-stealing within partitions
inline bool required_partition_build_enabled() {
    static const bool enabled = [] {
        const char* v = std::getenv("REQ_PARTITION_BUILD");
        return v && *v && *v != '0';
    }();
    return enabled;
}

} // namespace Contest

#endif // PARTITION_BUILD_H

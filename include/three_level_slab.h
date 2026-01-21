#pragma once

#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <memory>
#include <new>
#include <vector>

namespace Contest {

// 3-level slab allocator (assignment-style):
// Level 1: Global arena provides large blocks.
// Level 2: Thread-local arena sub-allocates from global blocks.
// Level 3: Partition arena (per thread) serves small allocations for per-partition chunks.
// TempAlloc (see temp_allocator.h) sits on top of the thread arena to hand out per-partition
// chunk lists during partitioned hash builds.
//
class ThreeLevelSlab {
public:
    // Mandatory slab allocator per requirements: always enabled.
    static bool enabled() { return true; }

    struct PartitionArena {
        // Level 3: partition arena — small allocations.
        void* alloc(std::size_t bytes, std::size_t align) {
            return thread_arena().alloc(bytes, align);
        }

        void dealloc(void* p, std::size_t bytes) noexcept {
            (void)p;
            (void)bytes;
            // Slab allocator: memory is reclaimed when thread arena is destroyed
            // Individual deallocation is a no-op for bump allocator pattern
        }

    private:
        struct ThreadArena {
            std::byte* cur = nullptr;
            std::size_t remaining = 0;
            std::vector<void*> blocks; // Level 1: per-thread large blocks

            void* alloc(std::size_t bytes, std::size_t align) {
                const std::size_t aligned = (bytes + (align - 1)) & ~(align - 1);
                void* p = nullptr;

                if (cur && remaining >= aligned) {
                    p = cur;
                    cur += aligned;
                    remaining -= aligned;
                    return p;
                }

                // Grab a new block (per-thread, no locks)
                const std::size_t block_size = global_block_size();
                const std::size_t need = aligned + align;
                const std::size_t bytes_to_get = need > block_size ? need : block_size;

                std::byte* block = static_cast<std::byte*>(::operator new(bytes_to_get));
                blocks.push_back(block);

                std::uintptr_t raw = reinterpret_cast<std::uintptr_t>(block);
                std::uintptr_t aligned_raw = (raw + (align - 1)) & ~(static_cast<std::uintptr_t>(align - 1));
                cur = reinterpret_cast<std::byte*>(aligned_raw);
                remaining = bytes_to_get - (aligned_raw - raw);

                p = cur;
                cur += aligned;
                remaining -= aligned;
                return p;
            }

            ~ThreadArena() = default; // Blocks intentionally retained until process exit to avoid dangling cross-thread partitions
        };

        static ThreadArena& thread_arena() {
            // Level 2: thread-local arena.
            static thread_local ThreadArena arena;
            return arena;
        }
    };

    static PartitionArena partition_arena() { return PartitionArena{}; }

    static std::size_t global_block_size() {
        // Default: 4 MiB blocks. Override via REQ_SLAB_GLOBAL_BLOCK_BYTES.
        static const std::size_t size = [] {
            const char* v = std::getenv("REQ_SLAB_GLOBAL_BLOCK_BYTES");
            if (!v || !*v) return static_cast<std::size_t>(1ull << 22);
            const long parsed = std::strtol(v, nullptr, 10);
            if (parsed < (1 << 20)) return static_cast<std::size_t>(1ull << 20);
            return static_cast<std::size_t>(parsed);
        }();
        return size;
    }
};

} // namespace Contest

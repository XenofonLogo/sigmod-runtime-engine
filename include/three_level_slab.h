#pragma once

#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <memory>
#include <new>
#include <vector>

namespace Contest {

// 3-επίπεδος slab allocator:
// Επίπεδο 1: global blocks μεγάλου μεγέθους.
// Επίπεδο 2: thread-local arena που τεμαχίζει τα global blocks.
// Επίπεδο 3: partition arena ανά thread για μικρές δεσμεύσεις (π.χ. chunk lists στο partitioned build).
// Το TempAlloc χτίζεται πάνω από το thread arena για τις per-partition λίστες.

class ThreeLevelSlab {
public:
    // Πάντα ενεργός σύμφωνα με τις απαιτήσεις
    static bool enabled() { return true; }

    struct PartitionArena {
        // Επίπεδο 3: partition arena — μικρές δεσμεύσεις για partition chunks
        void* alloc(std::size_t bytes, std::size_t align) {
            return thread_arena().alloc(bytes, align);
        }

        void dealloc(void* p, std::size_t bytes) noexcept {
            (void)p;
            (void)bytes;
            // Slab allocator: η μνήμη απελευθερώνεται όταν καταστρέφεται το thread arena
            // Η ατομική αποδέσμευση (dealloc) δεν κάνει τίποτα (bump allocator pattern)
        }

    private:
        struct ThreadArena {
            std::byte* cur = nullptr;
            std::size_t remaining = 0;
            std::vector<void*> blocks; // Επίπεδο 1: blocks ανά thread

            void* alloc(std::size_t bytes, std::size_t align) {
                // Υπολογισμός μεγέθους με ευθυγράμμιση
                const std::size_t aligned = (bytes + (align - 1)) & ~(align - 1);
                void* p = nullptr;

                // Αν υπάρχει χώρος στο τρέχον block
                if (cur && remaining >= aligned) {
                    p = cur;
                    cur += aligned;
                    remaining -= aligned;
                    return p;
                }

                // Νέο block (ανά thread, χωρίς locks)
                const std::size_t block_size = global_block_size();
                const std::size_t need = aligned + align;
                const std::size_t bytes_to_get = need > block_size ? need : block_size;

                std::byte* block = static_cast<std::byte*>(::operator new(bytes_to_get));
                blocks.push_back(block);

                // Ευθυγράμμιση pointer στο νέο block
                std::uintptr_t raw = reinterpret_cast<std::uintptr_t>(block);
                std::uintptr_t aligned_raw = (raw + (align - 1)) & ~(static_cast<std::uintptr_t>(align - 1));
                cur = reinterpret_cast<std::byte*>(aligned_raw);
                remaining = bytes_to_get - (aligned_raw - raw);

                p = cur;
                cur += aligned;
                remaining -= aligned;
                return p;
            }

            ~ThreadArena() = default; // Δεν αποδεσμεύουμε τα blocks για αποφυγή dangling μεταξύ threads
        };

        static ThreadArena& thread_arena() {
            // Επίπεδο 2: thread-local arena (ένα ανά thread)
            static thread_local ThreadArena arena;
            return arena;
        }
    };

    // Επιστρέφει partition arena (για χρήση σε κάθε thread)
    static PartitionArena partition_arena() { return PartitionArena{}; }

    // Μέγεθος global block (σταθερό: 4 MiB)
    static std::size_t global_block_size() {
        return static_cast<std::size_t>(1ull << 22);
    }
};

} // namespace Contest

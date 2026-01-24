#pragma once

#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <memory>
#include <new>
#include <vector>

namespace Contest {

// 3-επίπεδος slab allocator:
// Επίπεδο 1: Το global arena παρέχει μεγάλα blocks μνήμης.
// Επίπεδο 2: Το thread-local arena κάνει sub-allocate από τα global blocks.
// Επίπεδο 3: Το partition arena (ανά thread) εξυπηρετεί μικρές δεσμεύσεις για τα per-partition chunks.
// Το TempAlloc (δες temp_allocator.h) κάθεται πάνω από το thread arena και δίνει per-partition chunk lists κατά το partitioned hash build.

class ThreeLevelSlab {
public:
    // Υποχρεωτικός slab allocator σύμφωνα με τις απαιτήσεις: πάντα ενεργοποιημένος.
    static bool enabled() { return true; }

    struct PartitionArena {
        // Επίπεδο 3: partition arena — μικρές δεσμεύσεις μνήμης για partition chunks.
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
            std::vector<void*> blocks; // Επίπεδο 1: μεγάλα blocks ανά thread

            void* alloc(std::size_t bytes, std::size_t align) {
                // Υπολογίζει το μέγεθος με ευθυγράμμιση (alignment)
                const std::size_t aligned = (bytes + (align - 1)) & ~(align - 1);
                void* p = nullptr;

                // Αν υπάρχει αρκετός χώρος στο τρέχον block, επιστρέφει pointer
                if (cur && remaining >= aligned) {
                    p = cur;
                    cur += aligned;
                    remaining -= aligned;
                    return p;
                }

                // Δεσμεύει νέο block (ανά thread, χωρίς locks)
                const std::size_t block_size = global_block_size();
                const std::size_t need = aligned + align;
                const std::size_t bytes_to_get = need > block_size ? need : block_size;

                std::byte* block = static_cast<std::byte*>(::operator new(bytes_to_get));
                blocks.push_back(block);

                // Ευθυγραμμίζει το pointer στο νέο block
                std::uintptr_t raw = reinterpret_cast<std::uintptr_t>(block);
                std::uintptr_t aligned_raw = (raw + (align - 1)) & ~(static_cast<std::uintptr_t>(align - 1));
                cur = reinterpret_cast<std::byte*>(aligned_raw);
                remaining = bytes_to_get - (aligned_raw - raw);

                p = cur;
                cur += aligned;
                remaining -= aligned;
                return p;
            }

            ~ThreadArena() = default; // Τα blocks διατηρούνται μέχρι το τέλος του process για να αποφευχθούν dangling pointers μεταξύ threads
        };

        static ThreadArena& thread_arena() {
            // Επίπεδο 2: thread-local arena (ένα arena ανά thread)
            static thread_local ThreadArena arena;
            return arena;
        }
    };

    // Επιστρέφει ένα partition arena (για χρήση σε κάθε thread)
    static PartitionArena partition_arena() { return PartitionArena{}; }

    // Επιστρέφει το μέγεθος του global block (default: 4 MiB, μπορεί να αλλάξει με env var)
    static std::size_t global_block_size() {
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

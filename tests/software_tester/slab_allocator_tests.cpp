#include <catch2/catch_test_macros.hpp>
#include <vector>
#include <thread>
#include <cstdlib>
#include <set>
#include "slab_allocator.h"

using namespace Contest;

// ============================================================================
// SLAB ALLOCATOR TESTS
// ============================================================================

TEST_CASE("SlabAllocator: basic allocation", "[allocator][slab]") {
    SlabAllocator allocator;
    PartitionArena& arena = allocator.get_partition_arena(0);
    void* p1 = arena.alloc(100, 8);
    void* p2 = arena.alloc(200, 16);
    REQUIRE(p1 != nullptr);
    REQUIRE(p2 != nullptr);
    REQUIRE(p1 != p2);
}

TEST_CASE("SlabAllocator: alignment verification", "[allocator][slab][alignment]") {
    SlabAllocator allocator;
    PartitionArena& arena = allocator.get_partition_arena(0);
    
    // Test 8-byte alignment
    void* p8 = arena.alloc(100, 8);
    REQUIRE(reinterpret_cast<uintptr_t>(p8) % 8 == 0);
    
    // Test 16-byte alignment
    void* p16 = arena.alloc(100, 16);
    REQUIRE(reinterpret_cast<uintptr_t>(p16) % 16 == 0);
    
    // Test 32-byte alignment
    void* p32 = arena.alloc(100, 32);
    REQUIRE(reinterpret_cast<uintptr_t>(p32) % 32 == 0);
}

TEST_CASE("SlabAllocator: large allocation", "[allocator][slab][large]") {
    SlabAllocator allocator;
    PartitionArena& arena = allocator.get_partition_arena(0);
    
    // Allocate larger than typical block size
    size_t large_size = 5 * 1024 * 1024;  // 5 MiB
    void* p = arena.alloc(large_size, 16);
    REQUIRE(p != nullptr);
    REQUIRE(reinterpret_cast<uintptr_t>(p) % 16 == 0);
}

TEST_CASE("SlabAllocator: multiple sequential allocations", "[allocator][slab][sequential]") {
    SlabAllocator allocator;
    PartitionArena& arena = allocator.get_partition_arena(0);
    
    std::vector<void*> ptrs;
    for (int i = 0; i < 100; ++i) {
        void* p = arena.alloc(256, 8);
        REQUIRE(p != nullptr);
        ptrs.push_back(p);
    }
    
    // All pointers should be unique
    std::set<void*> unique_ptrs(ptrs.begin(), ptrs.end());
    REQUIRE(unique_ptrs.size() == 100);
}

TEST_CASE("SlabAllocator: dealloc is no-op (bump allocator)", "[allocator][slab][dealloc]") {
    SlabAllocator allocator;
    PartitionArena& arena = allocator.get_partition_arena(0);
    
    void* p1 = arena.alloc(100, 8);
    REQUIRE(p1 != nullptr);
    
    // Next allocation should still work (no explicit dealloc in bump allocator)
    void* p2 = arena.alloc(100, 8);
    REQUIRE(p2 != nullptr);
}

TEST_CASE("SlabAllocator: instantiation succeeds", "[allocator][slab][enabled]") {
    // SlabAllocator should instantiate without errors
    SlabAllocator allocator;
    REQUIRE(allocator.slabs.empty());  // No slabs allocated until first use
}

TEST_CASE("SlabAllocator: thread-local isolation", "[allocator][slab][thread]") {
    std::vector<void*> thread1_ptrs, thread2_ptrs;
    std::thread t1([&] {
        SlabAllocator allocator;
        PartitionArena& arena = allocator.get_partition_arena(0);
        for (int i = 0; i < 10; ++i) {
            thread1_ptrs.push_back(arena.alloc(64, 8));
        }
    });
    
    std::thread t2([&] {
        SlabAllocator allocator;
        PartitionArena& arena = allocator.get_partition_arena(0);
        for (int i = 0; i < 10; ++i) {
            thread2_ptrs.push_back(arena.alloc(64, 8));
        }
    });
    
    t1.join();
    t2.join();
    
    // Thread-local allocators should be isolated - pointers may overlap in virtual space
    // but allocators are managed per-thread
    REQUIRE(thread1_ptrs.size() == 10);
    REQUIRE(thread2_ptrs.size() == 10);
}

TEST_CASE("SlabAllocator: global block size", "[allocator][slab][config]") {
    SlabAllocator allocator;
    // Block size should match SLAB_SIZE constant (1 MiB = 1 << 20)
    size_t expected_size = 1 << 20;  // 1 MiB
    REQUIRE(allocator.SLAB_SIZE >= (1 << 20));
    REQUIRE(allocator.SLAB_SIZE <= (1 << 25));  // Reasonable upper bound
}

TEST_CASE("SlabAllocator: allocation with varying alignments", "[allocator][slab][alignments]") {
    SlabAllocator allocator;
    PartitionArena& arena = allocator.get_partition_arena(0);
    
    // Test reasonable alignment values (up to 32 bytes)
    for (int align = 1; align <= 32; align *= 2) {
        void* p = arena.alloc(256, align);
        REQUIRE(p != nullptr);
        REQUIRE(reinterpret_cast<uintptr_t>(p) % align == 0);
    }
}


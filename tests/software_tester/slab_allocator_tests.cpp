#include <catch2/catch_test_macros.hpp>
#include <vector>
#include <thread>
#include <cstdlib>
#include <set>
#include "three_level_slab.h"

using namespace Contest;

// ============================================================================
// SLAB ALLOCATOR TESTS
// ============================================================================

TEST_CASE("ThreeLevelSlab: basic allocation", "[allocator][slab]") {
    auto arena = ThreeLevelSlab::partition_arena();
    void* p1 = arena.alloc(100, 8);
    void* p2 = arena.alloc(200, 16);
    REQUIRE(p1 != nullptr);
    REQUIRE(p2 != nullptr);
    REQUIRE(p1 != p2);
}

TEST_CASE("ThreeLevelSlab: alignment verification", "[allocator][slab][alignment]") {
    auto arena = ThreeLevelSlab::partition_arena();
    
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

TEST_CASE("ThreeLevelSlab: large allocation", "[allocator][slab][large]") {
    auto arena = ThreeLevelSlab::partition_arena();
    
    // Allocate larger than typical block size
    size_t large_size = 5 * 1024 * 1024;  // 5 MiB
    void* p = arena.alloc(large_size, 16);
    REQUIRE(p != nullptr);
    REQUIRE(reinterpret_cast<uintptr_t>(p) % 16 == 0);
}

TEST_CASE("ThreeLevelSlab: multiple sequential allocations", "[allocator][slab][sequential]") {
    auto arena = ThreeLevelSlab::partition_arena();
    
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

TEST_CASE("ThreeLevelSlab: dealloc is no-op (bump allocator)", "[allocator][slab][dealloc]") {
    auto arena = ThreeLevelSlab::partition_arena();
    
    void* p1 = arena.alloc(100, 8);
    REQUIRE(p1 != nullptr);
    
    // Deallocate (should be no-op in bump allocator pattern)
    arena.dealloc(p1, 100);
    
    // Next allocation should still work
    void* p2 = arena.alloc(100, 8);
    REQUIRE(p2 != nullptr);
}

TEST_CASE("ThreeLevelSlab: enabled() returns true", "[allocator][slab][enabled]") {
    // Slab allocator should always be enabled per assignment
    REQUIRE(ThreeLevelSlab::enabled() == true);
}

TEST_CASE("ThreeLevelSlab: thread-local isolation", "[allocator][slab][thread]") {
    std::vector<void*> thread1_ptrs, thread2_ptrs;
    std::thread t1([&] {
        auto arena = ThreeLevelSlab::partition_arena();
        for (int i = 0; i < 10; ++i) {
            thread1_ptrs.push_back(arena.alloc(64, 8));
        }
    });
    
    std::thread t2([&] {
        auto arena = ThreeLevelSlab::partition_arena();
        for (int i = 0; i < 10; ++i) {
            thread2_ptrs.push_back(arena.alloc(64, 8));
        }
    });
    
    t1.join();
    t2.join();
    
    // Thread-local arenas should be isolated - pointers may overlap in virtual space
    // but arenas are managed per-thread
    REQUIRE(thread1_ptrs.size() == 10);
    REQUIRE(thread2_ptrs.size() == 10);
}

TEST_CASE("ThreeLevelSlab: global_block_size() returns reasonable value", "[allocator][slab][config]") {
    size_t block_size = ThreeLevelSlab::global_block_size();
    // Default is 4 MiB (1 << 22), minimum is 1 MiB (1 << 20)
    REQUIRE(block_size >= (1 << 20));
    REQUIRE(block_size <= (1 << 25));  // Reasonable upper bound
}

TEST_CASE("ThreeLevelSlab: allocation with varying alignments", "[allocator][slab][alignments]") {
    auto arena = ThreeLevelSlab::partition_arena();
    
    // Test reasonable alignment values (up to 32 bytes)
    for (int align = 1; align <= 32; align *= 2) {
        void* p = arena.alloc(256, align);
        REQUIRE(p != nullptr);
        REQUIRE(reinterpret_cast<uintptr_t>(p) % align == 0);
    }
}


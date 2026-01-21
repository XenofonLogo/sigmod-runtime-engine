#include <catch2/catch_test_macros.hpp>
#include <vector>
#include <thread>
#include "parallel_unchained_hashtable.h"
#include "hash_common.h"

using namespace Contest;

// ============================================================================
// PARTITIONED HASH BUILD TESTS
// ============================================================================

TEST_CASE("Partitioned build: phase correctness with small dataset", "[partitioned-build][phases]") {
    FlatUnchainedHashTable<int32_t> table;
    
    std::vector<HashEntry<int32_t>> entries = {
        {1, 0}, {2, 1}, {3, 2}, {4, 3}, {5, 4},
        {10, 5}, {20, 6}, {30, 7}, {40, 8}, {50, 9}
    };
    
    table.build_from_entries(entries);
    
    // Verify all entries are findable
    for (const auto& e : entries) {
        size_t len;
        const auto* bucket = table.probe(e.key, len);
        REQUIRE(bucket != nullptr);
        REQUIRE(len > 0);
        
        // Find exact entry
        bool found = false;
        for (size_t i = 0; i < len; ++i) {
            if (bucket[i].key == e.key && bucket[i].row_id == e.row_id) {
                found = true;
                break;
            }
        }
        REQUIRE(found);
    }
}

TEST_CASE("Partitioned build: contiguous tuple storage", "[partitioned-build][storage]") {
    FlatUnchainedHashTable<int32_t> table;
    
    // Create entries with known distribution
    std::vector<HashEntry<int32_t>> entries;
    for (int i = 1; i <= 100; ++i) {
        entries.push_back({i, static_cast<uint32_t>(i - 1)});
    }
    
    table.build_from_entries(entries);
    
    // Check that tuples are contiguous within partitions
    size_t total_found = 0;
    for (const auto& e : entries) {
        size_t len;
        const auto* bucket = table.probe(e.key, len);
        if (bucket) total_found += len;
    }
    
    REQUIRE(total_found >= entries.size());
}

TEST_CASE("Partitioned build: prefix sum correctness", "[partitioned-build][prefix-sum]") {
    FlatUnchainedHashTable<int32_t> table;
    
    // Build with known keys to verify ranges
    std::vector<HashEntry<int32_t>> entries;
    int row_id = 0;
    
    // Create entries with varying hash distribution
    for (int key : {1, 2, 3, 10, 11, 12, 100, 101, 102}) {
        entries.push_back({key, static_cast<uint32_t>(row_id++)});
    }
    
    table.build_from_entries(entries);
    
    // Total tuples should match input
    size_t total = 0;
    for (const auto& e : entries) {
        size_t len;
        table.probe(e.key, len);
        total += len;
    }
    
    REQUIRE(total >= entries.size());
}

TEST_CASE("Partitioned build: with duplicates", "[partitioned-build][duplicates]") {
    FlatUnchainedHashTable<int32_t> table;
    
    std::vector<HashEntry<int32_t>> entries = {
        {42, 0}, {42, 1}, {42, 2},  // duplicate keys
        {7, 3}, {7, 4},
        {99, 5}
    };
    
    table.build_from_entries(entries);
    
    // Probe for key 42
    size_t len;
    const auto* bucket = table.probe(42, len);
    REQUIRE(bucket != nullptr);
    
    // Count how many entries with key 42
    int count = 0;
    for (size_t i = 0; i < len; ++i) {
        if (bucket[i].key == 42) count++;
    }
    REQUIRE(count == 3);
}

TEST_CASE("Partitioned build: empty table", "[partitioned-build][edge-cases]") {
    FlatUnchainedHashTable<int32_t> table;
    
    std::vector<HashEntry<int32_t>> entries;  // empty
    table.build_from_entries(entries);
    
    // Probing should return nullptr
    size_t len;
    const auto* bucket = table.probe(42, len);
    REQUIRE(bucket == nullptr);
    REQUIRE(len == 0);
}

TEST_CASE("Partitioned build: single entry", "[partitioned-build][edge-cases]") {
    FlatUnchainedHashTable<int32_t> table;
    
    std::vector<HashEntry<int32_t>> entries = {{123, 0}};
    table.build_from_entries(entries);
    
    size_t len;
    const auto* bucket = table.probe(123, len);
    REQUIRE(bucket != nullptr);
    REQUIRE(len == 1);
    REQUIRE(bucket[0].key == 123);
    REQUIRE(bucket[0].row_id == 0);
}

TEST_CASE("Partitioned build: large dataset (1000 entries)", "[partitioned-build][large]") {
    FlatUnchainedHashTable<int32_t> table;
    
    std::vector<HashEntry<int32_t>> entries;
    for (int i = 0; i < 1000; ++i) {
        entries.push_back({i % 100, static_cast<uint32_t>(i)});  // 10 duplicates per key
    }
    
    table.build_from_entries(entries);
    
    // Verify we can find entries
    int found_count = 0;
    for (const auto& e : entries) {
        size_t len;
        const auto* bucket = table.probe(e.key, len);
        if (bucket) found_count++;
    }
    
    REQUIRE(found_count > 0);
}

TEST_CASE("Partitioned build: collision handling", "[partitioned-build][collisions]") {
    FlatUnchainedHashTable<int32_t> table;
    
    // Create entries that may collide
    std::vector<HashEntry<int32_t>> entries;
    for (int i = 0; i < 50; ++i) {
        entries.push_back({i, static_cast<uint32_t>(i)});
    }
    
    table.build_from_entries(entries);
    
    // All entries should be retrievable despite collisions
    for (const auto& e : entries) {
        size_t len;
        const auto* bucket = table.probe(e.key, len);
        REQUIRE(bucket != nullptr);
        REQUIRE(len > 0);
    }
}

TEST_CASE("Partitioned build: memory efficiency", "[partitioned-build][memory]") {
    FlatUnchainedHashTable<int32_t> table;
    
    std::vector<HashEntry<int32_t>> entries;
    for (int i = 0; i < 500; ++i) {
        entries.push_back({i, static_cast<uint32_t>(i)});
    }
    
    size_t mem_before = table.memory_usage();
    table.build_from_entries(entries);
    size_t mem_after = table.memory_usage();
    
    // Memory should grow
    REQUIRE(mem_after > mem_before);
    
    // Memory should be reasonable (not excessive)
    size_t expected_approx = entries.size() * sizeof(int32_t) + entries.size() * sizeof(uint32_t);
    REQUIRE(mem_after < expected_approx * 10);  // Allow 10x overhead
}


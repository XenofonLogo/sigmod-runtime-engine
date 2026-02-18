#include <catch2/catch_test_macros.hpp>
#include <vector>
#include <cstdint>
#include <memory>
#include "hashtable_interface.h"
#include "hash_common.h"

// Only include the default unchained wrapper to avoid redefinition errors
// Each wrapper redefines create_hashtable(), so we test them individually
#include "unchained_hashtable_wrapper.h"

// ============================================================================
// HASH TABLE IMPLEMENTATION TESTS
// Tests the actual hash table wrappers used in the join execution engine
// ============================================================================

// Tests for Unchained Hash Table (default implementation)
TEST_CASE("UnchainedHashTable: basic build and probe", "[hashtable][unchained]") {
    auto table = std::make_unique<Contest::UnchainedHashTableWrapper<int32_t>>();
    
    std::vector<Contest::HashEntry<int32_t>> entries = {
        {10, 0}, {20, 1}, {30, 2}
    };
    
    table->reserve(entries.size());
    table->build_from_entries(entries);
    
    size_t len = 0;
    auto* result = table->probe(10, len);
    REQUIRE(result != nullptr);
    REQUIRE(len == 1);
    REQUIRE(result[0].key == 10);
    REQUIRE(result[0].row_id == 0);
}

TEST_CASE("UnchainedHashTable: collision handling", "[hashtable][unchained][collision]") {
    auto table = std::make_unique<Contest::UnchainedHashTableWrapper<int32_t>>();
    
    std::vector<Contest::HashEntry<int32_t>> entries;
    for (int i = 0; i < 1000; ++i) {
        entries.push_back({i, static_cast<uint32_t>(i)});
    }
    
    table->reserve(entries.size());
    table->build_from_entries(entries);
    
    // Verify all entries can be retrieved
    for (int i = 0; i < 1000; ++i) {
        size_t len = 0;
        auto* result = table->probe(i, len);
        REQUIRE(result != nullptr);
        REQUIRE(len >= 1);
        
        bool found = false;
        for (size_t j = 0; j < len; ++j) {
            if (result[j].key == i && result[j].row_id == static_cast<uint32_t>(i)) {
                found = true;
                break;
            }
        }
        REQUIRE(found);
    }
}

TEST_CASE("UnchainedHashTable: duplicate keys", "[hashtable][unchained][duplicates]") {
    auto table = std::make_unique<Contest::UnchainedHashTableWrapper<int32_t>>();
    
    // Add same key with different row_ids
    std::vector<Contest::HashEntry<int32_t>> entries = {
        {42, 0}, {42, 1}, {42, 2}, {100, 3}
    };
    
    table->reserve(entries.size());
    table->build_from_entries(entries);
    
    size_t len = 0;
    auto* result = table->probe(42, len);
    REQUIRE(result != nullptr);
    REQUIRE(len == 3);
    
    // All three entries for key=42 should be found
    std::vector<uint32_t> row_ids;
    for (size_t i = 0; i < len; ++i) {
        if (result[i].key == 42) {
            row_ids.push_back(result[i].row_id);
        }
    }
    REQUIRE(row_ids.size() == 3);
}

TEST_CASE("UnchainedHashTable: missing key", "[hashtable][unchained]") {
    auto table = std::make_unique<Contest::UnchainedHashTableWrapper<int32_t>>();
    
    std::vector<Contest::HashEntry<int32_t>> entries = {
        {10, 0}, {20, 1}
    };
    
    table->build_from_entries(entries);
    
    size_t len = 0;
    auto* result = table->probe(999, len);
    REQUIRE((result == nullptr || len == 0));
}

// Tests for Robin Hood, Cuckoo, and Hopscotch are disabled to avoid
// redefinition errors (each wrapper redefines create_hashtable()).
// They can be tested by changing #include in execute_default.cpp

TEST_CASE("HashTable: load factor stress test", "[hashtable][stress]") {
    auto unchained = std::make_unique<Contest::UnchainedHashTableWrapper<int32_t>>();
    
    std::vector<Contest::HashEntry<int32_t>> entries;
    for (int i = 0; i < 5000; ++i) {
        entries.push_back({i, static_cast<uint32_t>(i)});
    }
    
    unchained->reserve(entries.size());
    unchained->build_from_entries(entries);
    
    // Verify high load factor doesn't break correctness
    for (int i = 0; i < 5000; i += 17) {
        size_t len = 0;
        auto* result = unchained->probe(i, len);
        REQUIRE(result != nullptr);
    }
}

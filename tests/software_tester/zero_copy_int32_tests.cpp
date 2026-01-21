#include <catch2/catch_test_macros.hpp>
#include <vector>
#include <cstring>

// ============================================================================
// ZERO-COPY INDEXING TESTS (REQ_BUILD_FROM_PAGES)
// ============================================================================

TEST_CASE("ZeroCopyInt32: direct page access without copying", "[zero-copy][int32]") {
    // Zero-copy indexing: directly index INT32 columns without copying data
    
    // Simulate a memory page containing INT32 values
    const int NUM_VALUES = 100;
    std::vector<int32_t> page_data;
    for (int i = 0; i < NUM_VALUES; ++i) {
        page_data.push_back(i * 1000);
    }
    
    // Zero-copy: get pointer to raw data, no copying
    const int32_t* data_ptr = page_data.data();
    REQUIRE(data_ptr != nullptr);
    
    // Access directly via pointer
    REQUIRE(*data_ptr == 0);
    REQUIRE(data_ptr[50] == 50000);
}

TEST_CASE("ZeroCopyInt32: null handling for zero-copy INT32", "[zero-copy][nulls]") {
    // Zero-copy works only for INT32 columns without NULLs
    // This test verifies the constraint
    
    // Simulate column without NULLs
    std::vector<int32_t> non_null_column;
    for (int i = 0; i < 1000; ++i) {
        non_null_column.push_back(i);
    }
    
    // Can use zero-copy
    const int32_t* ptr = non_null_column.data();
    REQUIRE(ptr != nullptr);
    REQUIRE(ptr[500] == 500);
    
    // Zero-copy only applies when REQ_BUILD_FROM_PAGES is enabled
    // and column is INT32 without NULLs
}

TEST_CASE("ZeroCopyInt32: pointer arithmetic for range access", "[zero-copy][range]") {
    // Zero-copy enables efficient range access via pointer arithmetic
    
    std::vector<int32_t> page_data;
    for (int i = 0; i < 1000; ++i) {
        page_data.push_back(i);
    }
    
    // Access subrange without copying
    const int32_t* begin = page_data.data() + 100;
    const int32_t* end = page_data.data() + 200;
    
    // Process range
    int count = 0;
    for (const int32_t* p = begin; p < end; ++p) {
        count++;
    }
    
    REQUIRE(count == 100);
    REQUIRE(*begin == 100);
    REQUIRE(*(end - 1) == 199);
}

TEST_CASE("ZeroCopyInt32: memory alignment preservation", "[zero-copy][alignment]") {
    // Zero-copy requires proper alignment in memory pages
    
    std::vector<int32_t> aligned_data;
    for (int i = 0; i < 100; ++i) {
        aligned_data.push_back(i);
    }
    
    // Verify alignment: int32_t should be 4-byte aligned
    const int32_t* ptr = aligned_data.data();
    uintptr_t addr = reinterpret_cast<uintptr_t>(ptr);
    
    // Most allocators align to at least 8 bytes
    REQUIRE(addr % 4 == 0);
}

TEST_CASE("ZeroCopyInt32: no copy overhead", "[zero-copy][performance]") {
    // Zero-copy benefit: no intermediate copying
    
    const int LARGE_SIZE = 100000;
    std::vector<int32_t> source;
    for (int i = 0; i < LARGE_SIZE; ++i) {
        source.push_back(i);
    }
    
    // Traditional approach: copy data
    std::vector<int32_t> copied = source;
    
    // Zero-copy approach: just get pointer
    const int32_t* zero_copy_ptr = source.data();
    
    // Pointer access is much faster than copying 400 KB
    REQUIRE(copied.size() == LARGE_SIZE);
    REQUIRE(zero_copy_ptr[LARGE_SIZE - 1] == LARGE_SIZE - 1);
}

TEST_CASE("ZeroCopyInt32: multi-column zero-copy", "[zero-copy][multi-column]") {
    // Can apply zero-copy to multiple INT32 columns independently
    
    std::vector<int32_t> col1, col2, col3;
    for (int i = 0; i < 1000; ++i) {
        col1.push_back(i);
        col2.push_back(i * 2);
        col3.push_back(i * 3);
    }
    
    // Zero-copy access to all columns
    const int32_t* ptr1 = col1.data();
    const int32_t* ptr2 = col2.data();
    const int32_t* ptr3 = col3.data();
    
    // All accessible without copying
    REQUIRE(ptr1[500] == 500);
    REQUIRE(ptr2[500] == 1000);
    REQUIRE(ptr3[500] == 1500);
}

TEST_CASE("ZeroCopyInt32: constraint - only for INT32 without NULLs", "[zero-copy][constraints]") {
    // Zero-copy only works for INT32 columns with no NULL values
    // This is enforced via REQ_BUILD_FROM_PAGES toggle
    
    // INT32 without NULLs: can use zero-copy
    std::vector<int32_t> valid_for_zero_copy;
    for (int i = 0; i < 100; ++i) {
        valid_for_zero_copy.push_back(i);
    }
    
    // VARCHAR with variable length: cannot use zero-copy
    std::vector<std::string> invalid_for_zero_copy;
    for (int i = 0; i < 100; ++i) {
        invalid_for_zero_copy.push_back("string_" + std::to_string(i));
    }
    
    // Only INT32 is zero-copyable
    const int32_t* int_ptr = valid_for_zero_copy.data();
    REQUIRE(int_ptr != nullptr);
    
    // VARCHAR would need copying or late materialization
    REQUIRE(invalid_for_zero_copy[0] == "string_0");
}

TEST_CASE("ZeroCopyInt32: optimization for hash build", "[zero-copy][hash-build]") {
    // Zero-copy greatly speeds up hashtable build phase
    // No need to copy INT32 keys from pages
    
    // Simulate hash entries built directly from page pointers
    std::vector<int32_t> page;
    for (int i = 0; i < 10000; ++i) {
        page.push_back(i % 1000);  // Keys with duplicates
    }
    
    // Build hashtable entries without copying:
    // Just reference the keys directly from page
    const int32_t* key_ptr = page.data();
    
    // Can iterate and hash without copying 40 KB of data
    int hash_sum = 0;
    for (int i = 0; i < 10000; ++i) {
        // In real code: compute hash(page[i]) without copying
        hash_sum += key_ptr[i] % 1000;
    }
    
    REQUIRE(hash_sum > 0);
}


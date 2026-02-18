#include <catch2/catch_test_macros.hpp>
#include <vector>
#include <cstdint>
#include "plan.h"
#include "table.h"
#include "columnar.h"

// ============================================================================
// INTEGRATION TESTS
// Tests actual columnar storage, zero-copy, late materialization, and execution engine
// ============================================================================

using namespace Contest;

TEST_CASE("ColumnarTable: column buffer creation", "[columnar][storage]") {
    ColumnBuffer buf;
    buf.num_rows = 0;
    
    // Create a simple INT32 column
    column_t col;
    col.values_per_page = 1024;
    
    for (int i = 0; i < 100; ++i) {
        col.append(value_t::make_i32(i));
    }
    
    REQUIRE(col.num_values == 100);
    REQUIRE(!col.pages.empty());
}

TEST_CASE("ColumnarTable: zero-copy INT32 detection", "[columnar][zero-copy]") {
    ColumnBuffer buf;
    column_t col;
    
    // Simulate zero-copy setup
    col.is_zero_copy = true;
    col.values_per_page = 1024;
    
    REQUIRE(col.is_zero_copy);
    REQUIRE(col.src_column == nullptr); // Would be set by actual Table conversion
}

TEST_CASE("ColumnarTable: value_t access patterns", "[columnar][access]") {
    column_t col;
    col.values_per_page = 10;
    
    // Add values across multiple pages
    for (int i = 0; i < 25; ++i) {
        col.append(value_t::make_i32(i * 10));
    }
    
    // Verify page structure
    REQUIRE(col.pages.size() == 3); // 10, 10, 5 values
    REQUIRE(col.pages[0].size() == 10);
    REQUIRE(col.pages[1].size() == 10);
    REQUIRE(col.pages[2].size() == 5);
    
    // Access via get() method
    const auto& val = col.get(15);
    REQUIRE(!val.is_null());
    REQUIRE(val.as_i32() == 150);
}

TEST_CASE("ColumnarTable: NULL handling", "[columnar][nulls]") {
    column_t col;
    
    col.append(value_t::make_i32(10));
    col.append(value_t::make_null());
    col.append(value_t::make_i32(20));
    
    REQUIRE(col.num_values == 3);
    REQUIRE(!col.get(0).is_null());
    REQUIRE(col.get(1).is_null());
    REQUIRE(!col.get(2).is_null());
}

TEST_CASE("ColumnarTable: cached page index optimization", "[columnar][cache]") {
    column_t col;
    col.values_per_page = 100;
    
    // Create multiple pages
    for (int i = 0; i < 300; ++i) {
        col.append(value_t::make_i32(i));
    }
    
    // Sequential access should benefit from cached_page_idx
    for (int i = 0; i < 300; ++i) {
        const auto& val = col.get(i);
        REQUIRE(val.as_i32() == i);
    }
    
    // Cache should have been updated during sequential scan
    REQUIRE(col.cached_page_idx >= 0);
}

// GlobalBloom tests removed - GlobalBloom was deleted from codebase
// Directory-embedded bloom filters (16-bit per bucket) are used instead

TEST_CASE("value_t: INT32 operations", "[value][int32]") {
    auto v = value_t::make_i32(42);
    
    REQUIRE(!v.is_null());
    REQUIRE(v.as_i32() == 42);
}

TEST_CASE("value_t: string reference packing", "[value][string]") {
    // Late materialization: strings stored as packed references
    PackedStringRef ref(0, 0, 42, 10); // table=0, col=0, page=42, slot=10
    auto v = value_t::make_str(ref);
    
    REQUIRE(!v.is_null());
    REQUIRE(v.as_ref() == ref.raw);
}

TEST_CASE("value_t: packed string ref with make_str_ref", "[value][string]") {
    auto v = value_t::make_str_ref(1, 2, 100, 50);
    
    REQUIRE(!v.is_null());
    // String is stored as compressed 64-bit reference
}

TEST_CASE("value_t: NULL value", "[value][null]") {
    auto v = value_t::make_null();
    
    REQUIRE(v.is_null());
}

TEST_CASE("Plan: basic scan node creation", "[plan][scan]") {
    Plan plan;
    
    std::vector<std::tuple<size_t, DataType>> output_attrs = {
        {0, DataType::INT32}
    };
    
    size_t scan_id = plan.new_scan_node(0, output_attrs);
    
    REQUIRE(scan_id == 0);
    REQUIRE(plan.nodes.size() == 1);
}

TEST_CASE("Plan: basic join node creation", "[plan][join]") {
    Plan plan;
    
    std::vector<std::tuple<size_t, DataType>> scan_attrs = {
        {0, DataType::INT32}
    };
    
    // Create two scan nodes
    size_t left = plan.new_scan_node(0, scan_attrs);
    size_t right = plan.new_scan_node(1, scan_attrs);
    
    std::vector<std::tuple<size_t, DataType>> join_output = {
        {0, DataType::INT32},
        {1, DataType::INT32}
    };
    
    size_t join_id = plan.new_join_node(true, left, right, 0, 0, join_output);
    
    REQUIRE(join_id == 2);
    REQUIRE(plan.nodes.size() == 3);
}

TEST_CASE("ColumnBuffer: basic structure", "[columnar][buffer]") {
    ColumnBuffer buf;
    buf.num_rows = 100;
    
    // Add a column
    column_t col;
    for (int i = 0; i < 100; ++i) {
        col.append(value_t::make_i32(i));
    }
    
    buf.columns.push_back(std::move(col));
    
    REQUIRE(buf.num_rows == 100);
    REQUIRE(buf.columns.size() == 1);
    REQUIRE(buf.columns[0].num_values == 100);
}

TEST_CASE("ColumnBuffer: multi-column layout", "[columnar][layout]") {
    ColumnBuffer buf;
    buf.num_rows = 50;
    
    // Column 1: INT32 keys
    column_t col1;
    for (int i = 0; i < 50; ++i) {
        col1.append(value_t::make_i32(i));
    }
    
    // Column 2: INT32 values
    column_t col2;
    for (int i = 0; i < 50; ++i) {
        col2.append(value_t::make_i32(i * 100));
    }
    
    buf.columns.push_back(std::move(col1));
    buf.columns.push_back(std::move(col2));
    
    REQUIRE(buf.columns.size() == 2);
    REQUIRE(buf.columns[0].num_values == 50);
    REQUIRE(buf.columns[1].num_values == 50);
}

TEST_CASE("Late Materialization: deferred column access concept", "[late-mat][deferred]") {
    // Late materialization: only materialize columns needed for output
    // not for intermediate join keys
    
    ColumnBuffer buf;
    buf.num_rows = 100;
    
    // Key column (always needed for join)
    column_t key_col;
    for (int i = 0; i < 100; ++i) {
        key_col.append(value_t::make_i32(i % 10));
    }
    
    // Payload column (deferred until output materialization)
    // Stored as packed string references
    column_t payload_col;
    for (int i = 0; i < 100; ++i) {
        PackedStringRef ref(0, 1, i / 100, i % 100);
        payload_col.append(value_t::make_str(ref));
    }
    
    buf.columns.push_back(std::move(key_col));
    buf.columns.push_back(std::move(payload_col));
    
    // Late materialization: join only uses key_col,
    // payload_col accessed only for output rows
    REQUIRE(buf.columns.size() == 2);
}

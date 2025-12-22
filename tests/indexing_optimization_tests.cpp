#include <catch2/catch_test_macros.hpp>

#include <vector>
#include <tuple>

#include "table.h"
#include "plan.h"
#include "columnar.h"

using namespace Contest;

// -----------------------------------------------------------------------------
// INDEXING OPTIMIZATION TESTS
// -----------------------------------------------------------------------------

TEST_CASE(
    "Indexing optimization: INT32 column without NULLs uses zero-copy",
    "[indexing][zero-copy]"
) {
    // Table: INT32 χωρίς NULLs
    std::vector<std::vector<Data>> data = {
        {1}, {2}, {3}, {4}, {5}
    };
    std::vector<DataType> types = { DataType::INT32 };

    Table table(data, types);
    ColumnarTable columnar = table.to_columnar();

    // Plan + Scan
    Plan plan;
    plan.inputs.emplace_back(std::move(columnar));

    ScanNode scan;
    scan.base_table_id = 0;

    std::vector<std::tuple<size_t, DataType>> output_attrs = {
        {0, DataType::INT32}
    };

    ColumnBuffer buf =
        scan_columnar_to_columnbuffer(plan, scan, output_attrs);

    REQUIRE(buf.num_rows == 5);
    REQUIRE(buf.columns.size() == 1);

    const auto& col = buf.columns[0];

    // ---- ΚΡΙΣΙΜΟΙ ΕΛΕΓΧΟΙ ----
    REQUIRE(col.is_zero_copy == true);
    REQUIRE(col.src_column != nullptr);

    // ---- ΣΩΣΤΗ ΑΝΑΓΝΩΣΗ ΤΙΜΩΝ ----
    for (size_t i = 0; i < 5; ++i) {
        REQUIRE(col.get(i).as_i32() == static_cast<int32_t>(i + 1));
    }
}

// -----------------------------------------------------------------------------

TEST_CASE(
    "Indexing optimization: INT32 column with NULLs disables zero-copy",
    "[indexing][materialize]"
) {
    // Table: INT32 με NULL
    std::vector<std::vector<Data>> data = {
        {1},
        {std::monostate{}},
        {3}
    };
    std::vector<DataType> types = { DataType::INT32 };

    Table table(data, types);
    ColumnarTable columnar = table.to_columnar();

    Plan plan;
    plan.inputs.emplace_back(std::move(columnar));

    ScanNode scan;
    scan.base_table_id = 0;

    std::vector<std::tuple<size_t, DataType>> output_attrs = {
        {0, DataType::INT32}
    };

    ColumnBuffer buf =
        scan_columnar_to_columnbuffer(plan, scan, output_attrs);

    const auto& col = buf.columns[0];

    REQUIRE(col.is_zero_copy == false);
    REQUIRE(col.src_column == nullptr);

    REQUIRE(col.get(0).as_i32() == 1);
    REQUIRE(col.get(1).is_null());
    REQUIRE(col.get(2).as_i32() == 3);
}

// -----------------------------------------------------------------------------

TEST_CASE(
    "Indexing optimization: VARCHAR columns never use zero-copy",
    "[indexing][varchar]"
) {
    using namespace std::string_literals;

    std::vector<std::vector<Data>> data = {
        {"a"s}, {"b"s}, {"c"s}
    };
    std::vector<DataType> types = { DataType::VARCHAR };

    Table table(data, types);
    ColumnarTable columnar = table.to_columnar();

    Plan plan;
    plan.inputs.emplace_back(std::move(columnar));

    ScanNode scan;
    scan.base_table_id = 0;

    std::vector<std::tuple<size_t, DataType>> output_attrs = {
        {0, DataType::VARCHAR}
    };

    ColumnBuffer buf =
        scan_columnar_to_columnbuffer(plan, scan, output_attrs);

    REQUIRE(buf.columns.size() == 1);
    REQUIRE(buf.columns[0].is_zero_copy == false);
}

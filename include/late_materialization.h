#pragma once
#include <cstdint>
#include <vector>
#include <string>
#include <unordered_map>

/*
 * PackedStringRef (64-bit)
 * -------------------------
 * A compact reference to a string stored inside the column-store.
 * Instead of materializing VARCHAR values during scanning or joining,
 * we store only this 64-bit reference. At the end (root operator),
 * the actual string is reconstructed from the original column-store pages.
 *
 * Bit layout (total: 64 bits):
 *   - table_id:  8 bits
 *   - column_id: 8 bits
 *   - page_id:   24 bits
 *   - offset:    20 bits
 *   - flags:      4 bits   (bit0 = is_null, bit1 = is_long_string)
 */
struct PackedStringRef {
    uint64_t data;

    static constexpr int TABLE_BITS  = 8;
    static constexpr int COLUMN_BITS = 8;
    static constexpr int PAGE_BITS   = 24;
    static constexpr int OFFSET_BITS = 20;
    static constexpr int FLAGS_BITS  = 4;

    static PackedStringRef make(uint8_t table_id,
                                uint8_t column_id,
                                uint32_t page_id,
                                uint32_t offset,
                                bool is_null = false,
                                bool is_long = false);

    uint8_t  table_id()  const;
    uint8_t  column_id() const;
    uint32_t page_id()   const;
    uint32_t offset()    const;

    bool is_null() const;
    bool is_long() const;
};

/*
 * value_t
 * -------
 * A minimal tagged union storing either:
 *   - NULL
 *   - 32-bit integer
 *   - PackedStringRef (compressed VARCHAR reference)
 *
 * This avoids the overhead of std::variant and avoids copying strings
 * during intermediate query execution (late materialization).
 */
struct value_t {
    enum Kind : uint8_t {
        VT_NULL   = 0,
        VT_INT32  = 1,
        VT_STRREF = 2
    } kind;

    int32_t  ival;   // used if kind == VT_INT32
    uint64_t sref;   // raw PackedStringRef bits if kind == VT_STRREF

    value_t();                          // default = NULL
    static value_t make_null();
    static value_t make_int(int32_t x);
    static value_t make_strref(const PackedStringRef &r);

    bool is_null()   const;
    bool is_int()    const;
    bool is_strref() const;

    PackedStringRef get_sref() const;
    int32_t         get_int()  const;
};

/*
 * Minimal in-memory representations for demonstration:
 * -----------------------------------------------------
 * These structures simulate a column-store:
 *   - VarcharPage: actual strings
 *   - IntPage: actual integers
 *   - Column: either VARCHAR or INT
 *   - Table: a set of columns and pages
 *   - Catalog: a lookup of tables by table_id
 *
 * Replace these with your real storage structures
 * when integrating into the project.
 */

struct VarcharPage {
    std::vector<std::string> values;
};

struct IntPage {
    std::vector<int32_t> values;
};

struct Column {
    bool is_int;
    std::vector<IntPage> int_pages;
    std::vector<VarcharPage> str_pages;
};

struct Table {
    uint8_t table_id;
    std::vector<Column> columns;

    size_t num_rows() const;
};

struct Catalog {
    std::unordered_map<uint8_t, Table> tables;
};

/*
 * Scanning
 * --------
 * Returns a row-store (vector of rows), where each cell is a value_t.
 * INT32 columns are materialized immediately.
 * VARCHAR columns store only PackedStringRef.
 */
std::vector<std::vector<value_t>>
scan_to_rowstore(Catalog &catalog,
                 uint8_t table_id,
                 const std::vector<uint8_t> &col_ids);

/*
 * ColumnarResult
 * --------------
 * A simple structure representing columnar output.
 * Can either:
 *   - materialize string columns (method 1), or
 *   - keep PackedStringRef until consumption (method 2).
 */
struct ColumnarResult {
    std::vector<bool>           is_int_col;
    std::vector<std::vector<int32_t>>           int_cols;
    std::vector<std::vector<std::string>>       str_cols;   // materialized strings
    std::vector<std::vector<PackedStringRef>>   str_refs;   // late materialization
    size_t num_rows = 0;
};

/*
 * materialize_string()
 * --------------------
 * Converts a PackedStringRef back into a full string
 * by accessing the original column-store pages.
 */
std::string materialize_string(const Catalog &catalog,
                               const PackedStringRef &r);

/*
 * convert_rowstore_to_columnar()
 * ------------------------------
 * Method 1 (slow, for debugging):
 * Materializes all strings and produces a final columnar result.
 */
ColumnarResult convert_rowstore_to_columnar(
    const Catalog &catalog,
    const std::vector<std::vector<value_t>> &rows);

/*
 * direct_hash_join_produce_columnar()
 * -----------------------------------
 * Method 2 (real required solution):
 * Produces a ColumnarTable directly during join execution,
 * WITHOUT ever materializing strings in intermediate steps.
 */
ColumnarResult direct_hash_join_produce_columnar(
    Catalog &catalog,
    uint8_t tableA, uint8_t keyA_col,
    const std::vector<uint8_t> &outputA_cols,
    uint8_t tableB, uint8_t keyB_col,
    const std::vector<uint8_t> &outputB_cols);

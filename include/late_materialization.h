#pragma once
#include <cstdint>
#include <vector>
#include <string>
#include <unordered_map>
#include "plan.h"

/* StringRef (64-bit packed representation)
 * Compact reference to a VARCHAR stored in the paged column-store.
 * Keeps same bit layout as the earlier PackedStringRef but exposes
 * helpers named `StringRef` and `pack`/`unpack` which other code/tests
 * expect. Also provide an alias `PackedStringRef` for compatibility.
 */
struct StringRef
{
    uint64_t data;

    static constexpr int TABLE_BITS = 8;
    static constexpr int COLUMN_BITS = 8;
    static constexpr int PAGE_BITS = 24;
    static constexpr int OFFSET_BITS = 20;
    static constexpr int FLAGS_BITS = 4;

    static StringRef pack_fields(uint8_t table_id,
                                 uint8_t column_id,
                                 uint32_t page_id,
                                 uint32_t offset,
                                 bool is_null = false,
                                 bool is_long = false)
    {
        uint64_t d = 0;
        uint64_t pos = 0;
        d |= (uint64_t)(offset & ((1u << OFFSET_BITS) - 1)) << pos;
        pos += OFFSET_BITS;
        d |= (uint64_t)(page_id & ((1u << PAGE_BITS) - 1)) << pos;
        pos += PAGE_BITS;
        d |= (uint64_t)(column_id & ((1u << COLUMN_BITS) - 1)) << pos;
        pos += COLUMN_BITS;
        d |= (uint64_t)(table_id & ((1u << TABLE_BITS) - 1)) << pos;
        pos += TABLE_BITS;
        uint8_t flags = (is_null ? 1 : 0) | (is_long ? 2 : 0);
        d |= (uint64_t)(flags & ((1u << FLAGS_BITS) - 1)) << pos;
        return StringRef(d);
    }

    // backward-compatible name
    static StringRef make(uint8_t table_id,
                          uint8_t column_id,
                          uint32_t page_id,
                          uint32_t offset,
                          bool is_null = false,
                          bool is_long = false)
    {
        return pack_fields(table_id, column_id, page_id, offset, is_null, is_long);
    }

    uint8_t table_id() const { return (data >> (OFFSET_BITS + PAGE_BITS + COLUMN_BITS)) & ((1u << TABLE_BITS) - 1); }
    uint8_t column_id() const { return (data >> (OFFSET_BITS + PAGE_BITS)) & ((1u << COLUMN_BITS) - 1); }
    uint32_t page_id() const { return (uint32_t)((data >> OFFSET_BITS) & ((1ull << PAGE_BITS) - 1)); }
    uint32_t offset() const { return (uint32_t)(data & ((1ull << OFFSET_BITS) - 1)); }

    bool is_null() const
    {
        uint64_t pos = OFFSET_BITS + PAGE_BITS + COLUMN_BITS + TABLE_BITS;
        return ((data >> pos) & 1ull) != 0;
    }
    bool is_long() const
    {
        uint64_t pos = OFFSET_BITS + PAGE_BITS + COLUMN_BITS + TABLE_BITS;
        return ((data >> (pos + 1)) & 1ull) != 0;
    }

    uint64_t pack() const { return data; }
    static StringRef unpack(uint64_t v) { return StringRef(v); }
    // construct from raw packed bits
    explicit StringRef(uint64_t raw) : data(raw) {}
    // convenience ctor matching earlier code paths
    StringRef(uint16_t table_id, uint8_t column_id, uint32_t page_id, uint32_t offset)
        : data(pack_fields(static_cast<uint8_t>(table_id), column_id, page_id, offset).data) {}
};

using PackedStringRef = StringRef;

// Lightweight helpers used by the columnar join/finalizer
struct StringRefHash
{
    const Plan *plan;
    explicit StringRefHash(const Plan *p = nullptr) : plan(p) {}
    size_t operator()(const StringRef &r) const noexcept
    {
        return std::hash<uint64_t>()(r.pack());
    }
};

struct StringRefEq
{
    const Plan *plan;
    explicit StringRefEq(const Plan *p = nullptr) : plan(p) {}
    bool operator()(const StringRef &a, const StringRef &b) const noexcept
    {
        return a.pack() == b.pack();
    }
};

// Resolver: convert a packed StringRef to (ptr,len)
struct StringRefResolver
{
    const Plan *plan;
    explicit StringRefResolver(const Plan *p = nullptr) : plan(p) {}
    std::pair<const char *, size_t> resolve(uint64_t packed_ref, std::string &tmp) const
    {
        if (plan == nullptr)
            return {nullptr, 0};
        StringRef r = StringRef::unpack(packed_ref);
        if (r.is_null())
            return {nullptr, 0};

        uint8_t table_id = r.table_id();
        uint8_t col_id = r.column_id();
        uint32_t page_id = r.page_id();
        uint32_t offset = r.offset();

        if (table_id >= plan->inputs.size())
            return {nullptr, 0};
        const auto &tbl = plan->inputs[table_id];
        if (col_id >= tbl.columns.size())
            return {nullptr, 0};
        const auto &col = tbl.columns[col_id];
        if (page_id >= col.pages.size())
            return {nullptr, 0};

        auto *page = col.pages[page_id]->data;
        uint16_t num_rows = *reinterpret_cast<uint16_t *>(page);

        // Long-string (multi-page) handling: first page marked 0xffff, following pages 0xfffe
        if (num_rows == 0xffff)
        {
            tmp.clear();
            // first page contains fragment
            auto *ptr = reinterpret_cast<char *>(page + 4);
            uint16_t frag_len = *reinterpret_cast<uint16_t *>(page + 2);
            tmp.append(ptr, ptr + frag_len);
            // subsequent pages
            uint32_t pid = page_id + 1;
            while (pid < col.pages.size())
            {
                auto *p = col.pages[pid]->data;
                uint16_t nr = *reinterpret_cast<uint16_t *>(p);
                if (nr != 0xfffe)
                    break;
                uint16_t frag = *reinterpret_cast<uint16_t *>(p + 2);
                auto *dp = reinterpret_cast<char *>(p + 4);
                tmp.append(dp, dp + frag);
                ++pid;
            }
            return std::pair<const char *, size_t>(tmp.data(), tmp.size());
        }

        if (num_rows == 0xfffe)
        {
            // Invalid: continuation page without starter
            return std::pair<const char *, size_t>(nullptr, 0);
        }

        // Regular page: offsets table followed by concatenated strings
        uint16_t num_non_null = *reinterpret_cast<uint16_t *>(page + 2);
        if (offset >= num_non_null)
            return {nullptr, 0};
        auto *offset_begin = reinterpret_cast<uint16_t *>(page + 4);
        auto *data_begin = reinterpret_cast<char *>(page + 4 + num_non_null * 2);
        size_t start = (offset == 0) ? 0 : static_cast<size_t>(offset_begin[offset - 1]);
        size_t end = static_cast<size_t>(offset_begin[offset]);
        return std::pair<const char *, size_t>(data_begin + start, end - start);
    }
};

/* Unified representation that supports multiple compatibility APIs used
 * across the codebase/tests. It provides:
 *  - enum `Type` with I32/I64/FP64/STR/NUL
 *  - union `u` with fields i32/i64/f64/ref (packed StringRef)
 *  - factory functions: make_i32/make_i64/make_f64/make_str/make_null
 *  - compatibility aliases: make_int, from_int, from_stringref, make_strref
 */
struct value_t
{
    enum Type : uint8_t
    {
        NUL = 0,
        I32 = 1,
        I64 = 2,
        FP64 = 3,
        STR = 4
    } type;

    union U
    {
        int32_t i32;
        int64_t i64;
        double f64;
        uint64_t ref; // packed StringRef
        U() : i32(0) {}
    } u;

    value_t() : type(NUL) { u.ref = 0; }

    static value_t make_null()
    {
        value_t v;
        v.type = NUL;
        v.u.ref = 0;
        return v;
    }
    static value_t make_i32(int32_t x)
    {
        value_t v;
        v.type = I32;
        v.u.i32 = x;
        return v;
    }
    static value_t make_i64(int64_t x)
    {
        value_t v;
        v.type = I64;
        v.u.i64 = x;
        return v;
    }
    static value_t make_f64(double x)
    {
        value_t v;
        v.type = FP64;
        v.u.f64 = x;
        return v;
    }
    static value_t make_str(const StringRef &r)
    {
        value_t v;
        v.type = STR;
        v.u.ref = r.pack();
        return v;
    }

    // Compatibility helpers (old/new names)
    static value_t make_int(int32_t x) { return make_i32(x); }
    static value_t from_int(int32_t x) { return make_i32(x); }
    static value_t from_stringref(const StringRef &r) { return make_str(r); }
    static value_t make_strref(const PackedStringRef &r) { return make_str(r); }

    bool is_null() const { return type == NUL; }
    bool is_int() const { return type == I32; }
    bool is_i64() const { return type == I64; }
    bool is_f64() const { return type == FP64; }
    bool is_strref() const { return type == STR; }

    int32_t get_int() const { return u.i32; }
    int64_t get_i64() const { return u.i64; }
    double get_f64() const { return u.f64; }
    PackedStringRef get_sref() const { return PackedStringRef::unpack(u.ref); }
};

struct LM_VarcharPage
{
    std::vector<std::string> values;
};
struct LM_IntPage
{
    std::vector<int32_t> values;
};

struct LM_Column
{
    bool is_int;
    std::vector<LM_IntPage> int_pages;
    std::vector<LM_VarcharPage> str_pages;
};

struct LM_Table
{
    uint8_t table_id;
    std::vector<LM_Column> columns;
    size_t num_rows() const;
};

struct Catalog
{
    std::unordered_map<uint8_t, LM_Table> tables;
};

/*
 * Returns a row-store (vector of rows), where each cell is a value_t.
 * INT32 columns are materialized immediately.
 * VARCHAR columns store only PackedStringRef.
 */
std::vector<std::vector<value_t>>
scan_to_rowstore(Catalog &catalog,
                 uint8_t table_id,
                 const std::vector<uint8_t> &col_ids);

/*
 * A simple structure representing columnar output.
 * Can either:
 *   - materialize string columns (method 1), or
 *   - keep PackedStringRef until consumption (method 2).
 */
struct ColumnarResult
{
    std::vector<bool> is_int_col;
    std::vector<std::vector<int32_t>> int_cols;
    std::vector<std::vector<std::string>> str_cols;     // materialized strings
    std::vector<std::vector<PackedStringRef>> str_refs; // late materialization
    size_t num_rows = 0;
};

/* Converts a PackedStringRef back into a full string
 by accessing the original column-store pages.
 */
std::string materialize_string(const Catalog &catalog,
                               const PackedStringRef &r);

/* Materializes all strings and produces a final columnar result.*/
ColumnarResult convert_rowstore_to_columnar(
    const Catalog &catalog,
    const std::vector<std::vector<value_t>> &rows);

/*Produces a ColumnarTable directly during join execution*/
ColumnarResult direct_hash_join_produce_columnar(
    Catalog &catalog,
    uint8_t tableA, uint8_t keyA_col,
    const std::vector<uint8_t> &outputA_cols,
    uint8_t tableB, uint8_t keyB_col,
    const std::vector<uint8_t> &outputB_cols);

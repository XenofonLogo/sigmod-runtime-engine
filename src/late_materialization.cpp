#include "late_materialization.h"
#include <iostream>
#include <utility>

/**********************************************************************
 * PackedStringRef Implementation
 *********************************************************************/

PackedStringRef PackedStringRef::make(uint8_t table_id,
                                      uint8_t column_id,
                                      uint32_t page_id,
                                      uint32_t offset,
                                      bool is_null,
                                      bool is_long)
{
    uint64_t d = 0;
    uint64_t pos = 0;

    // offset (20 bits)
    d |= (uint64_t)(offset & ((1u << OFFSET_BITS) - 1)) << pos;
    pos += OFFSET_BITS;

    // page_id (24 bits)
    d |= (uint64_t)(page_id & ((1u << PAGE_BITS) - 1)) << pos;
    pos += PAGE_BITS;

    // column_id (8 bits)
    d |= (uint64_t)(column_id & ((1u << COLUMN_BITS) - 1)) << pos;
    pos += COLUMN_BITS;

    // table_id (8 bits)
    d |= (uint64_t)(table_id & ((1u << TABLE_BITS) - 1)) << pos;
    pos += TABLE_BITS;

    // flags (4 bits)
    uint8_t flags = (is_null ? 1 : 0) | (is_long ? 2 : 0);
    d |= (uint64_t)(flags & ((1u << FLAGS_BITS) - 1)) << pos;

    return PackedStringRef{d};
}

uint8_t PackedStringRef::table_id() const {
    return (data >> (OFFSET_BITS + PAGE_BITS + COLUMN_BITS)) & ((1u << TABLE_BITS) - 1);
}

uint8_t PackedStringRef::column_id() const {
    return (data >> (OFFSET_BITS + PAGE_BITS)) & ((1u << COLUMN_BITS) - 1);
}

uint32_t PackedStringRef::page_id() const {
    return (uint32_t)((data >> OFFSET_BITS) & ((1ull << PAGE_BITS) - 1));
}

uint32_t PackedStringRef::offset() const {
    return (uint32_t)(data & ((1ull << OFFSET_BITS) - 1));
}

bool PackedStringRef::is_null() const {
    uint64_t pos = OFFSET_BITS + PAGE_BITS + COLUMN_BITS + TABLE_BITS;
    return ((data >> pos) & 1ull) != 0;
}

bool PackedStringRef::is_long() const {
    uint64_t pos = OFFSET_BITS + PAGE_BITS + COLUMN_BITS + TABLE_BITS;
    return ((data >> (pos + 1)) & 1ull) != 0;
}

/**********************************************************************
 * value_t Implementation
 *********************************************************************/

value_t::value_t() : kind(VT_NULL), ival(0), sref(0) {}

value_t value_t::make_null() {
    value_t v;
    v.kind = VT_NULL;
    return v;
}

value_t value_t::make_int(int32_t x) {
    value_t v;
    v.kind = VT_INT32;
    v.ival = x;
    return v;
}

value_t value_t::make_strref(const PackedStringRef &r) {
    value_t v;
    v.kind = VT_STRREF;
    v.sref = r.data;
    return v;
}

bool value_t::is_null() const { return kind == VT_NULL; }
bool value_t::is_int() const  { return kind == VT_INT32; }
bool value_t::is_strref() const { return kind == VT_STRREF; }

PackedStringRef value_t::get_sref() const {
    return PackedStringRef{sref};
}

int32_t value_t::get_int() const {
    return ival;
}

/**********************************************************************
 * Table / Catalog Implementation
 *********************************************************************/

size_t Table::num_rows() const {
    if (columns.empty()) return 0;

    const Column &c = columns[0];

    if (c.is_int) {
        size_t total = 0;
        for (const auto &pg : c.int_pages)
            total += pg.values.size();
        return total;
    } else {
        size_t total = 0;
        for (const auto &pg : c.str_pages)
            total += pg.values.size();
        return total;
    }
}

/**********************************************************************
 * Scanning (producing row-store of value_t)
 *********************************************************************/

std::vector<std::vector<value_t>>
scan_to_rowstore(Catalog &catalog,
                 uint8_t table_id,
                 const std::vector<uint8_t> &col_ids)
{
    auto it = catalog.tables.find(table_id);
    if (it == catalog.tables.end())
        return {};

    Table &tbl = it->second;
    size_t row_count = tbl.num_rows();

    std::vector<std::vector<value_t>> rows(
        row_count,
        std::vector<value_t>(col_ids.size(), value_t::make_null())
    );

    // For every requested column, fill each row.
    for (size_t col_idx = 0; col_idx < col_ids.size(); ++col_idx) {
        uint8_t col_id = col_ids[col_idx];
        Column &col = tbl.columns[col_id];

        size_t row_id = 0;

        if (col.is_int) {
            // Materialize all integers immediately.
            for (auto &page : col.int_pages) {
                for (int32_t val : page.values) {
                    rows[row_id][col_idx] = value_t::make_int(val);
                    row_id++;
                }
            }
        } else {
            // VARCHAR: do NOT materialize string; store PackedStringRef.
            for (uint32_t p = 0; p < col.str_pages.size(); ++p) {
                auto &page = col.str_pages[p];
                for (uint32_t off = 0; off < page.values.size(); ++off) {
                    PackedStringRef ref = PackedStringRef::make(
                        table_id, col_id, p, off, false, false
                    );
                    rows[row_id][col_idx] = value_t::make_strref(ref);
                    row_id++;
                }
            }
        }
    }

    return rows;
}

/**********************************************************************
 * Helper: Materialize a PackedStringRef back to actual VARCHAR
 *********************************************************************/

std::string materialize_string(const Catalog &catalog,
                               const PackedStringRef &r)
{
    if (r.is_null())
        return "";

    auto it = catalog.tables.find(r.table_id());
    if (it == catalog.tables.end())
        return "";

    const Table &tbl = it->second;
    uint8_t col_id = r.column_id();
    if (col_id >= tbl.columns.size())
        return "";

    const Column &col = tbl.columns[col_id];
    if (col.is_int)
        return "";   // invalid but safe

    uint32_t pg = r.page_id();
    uint32_t off = r.offset();

    if (pg >= col.str_pages.size()) return "";
    const auto &page = col.str_pages[pg];

    if (off >= page.values.size()) return "";
    return page.values[off];
}

/**********************************************************************
 * Method 1:
 * Convert row-store (value_t) to columnar result (materializing strings)
 *********************************************************************/

ColumnarResult convert_rowstore_to_columnar(
    const Catalog &catalog,
    const std::vector<std::vector<value_t>> &rows)
{
    ColumnarResult res;
    if (rows.empty()) return res;

    size_t cols = rows[0].size();
    size_t nrows = rows.size();

    res.num_rows = nrows;
    res.is_int_col.resize(cols);
    res.int_cols.resize(cols);
    res.str_cols.resize(cols);

    // Infer types from first row.
    for (size_t c = 0; c < cols; ++c)
        res.is_int_col[c] = rows[0][c].is_int();

    // Reserve capacity.
    for (size_t c = 0; c < cols; ++c) {
        if (res.is_int_col[c])
            res.int_cols[c].reserve(nrows);
        else
            res.str_cols[c].reserve(nrows);
    }

    // Fill columns.
    for (size_t r = 0; r < nrows; ++r) {
        for (size_t c = 0; c < cols; ++c) {
            const value_t &v = rows[r][c];
            if (v.is_int()) {
                res.int_cols[c].push_back(v.get_int());
            } else if (v.is_strref()) {
                res.str_cols[c].push_back(
                    materialize_string(catalog, v.get_sref())
                );
            } else {
                // NULL
                if (res.is_int_col[c])
                    res.int_cols[c].push_back(0);
                else
                    res.str_cols[c].push_back("");
            }
        }
    }

    return res;
}

/**********************************************************************
 * Method 2:
 * Direct hash join producing a columnar result without materializing strings
 *********************************************************************/

ColumnarResult direct_hash_join_produce_columnar(
    Catalog &catalog,
    uint8_t tableA, uint8_t keyA_col,
    const std::vector<uint8_t> &outputA_cols,
    uint8_t tableB, uint8_t keyB_col,
    const std::vector<uint8_t> &outputB_cols)
{
    // Build hash table from A(key) → list of rowIDs.
    auto Akey_rows = scan_to_rowstore(catalog, tableA, {keyA_col});

    std::unordered_map<int32_t, std::vector<size_t>> build_map;
    build_map.reserve(Akey_rows.size());

    for (size_t r = 0; r < Akey_rows.size(); ++r) {
        const value_t &v = Akey_rows[r][0];
        if (v.is_int())
            build_map[v.get_int()].push_back(r);
    }

    // Scan complete output columns for A and B.
    std::vector<uint8_t> Acols = outputA_cols;
    Acols.insert(Acols.begin(), keyA_col);
    auto A_out = scan_to_rowstore(catalog, tableA, Acols);

    std::vector<uint8_t> Bcols = outputB_cols;
    Bcols.insert(Bcols.begin(), keyB_col);
    auto B_out = scan_to_rowstore(catalog, tableB, Bcols);

    size_t nA = Acols.size();
    size_t nB = Bcols.size();
    size_t total_cols = nA + nB;

    ColumnarResult res;
    res.is_int_col.resize(total_cols);

    // Infer types
    for (size_t c = 0; c < nA; ++c)
        res.is_int_col[c] = A_out[0][c].is_int();

    for (size_t c = 0; c < nB; ++c)
        res.is_int_col[nA + c] = B_out[0][c].is_int();

    res.int_cols.resize(total_cols);
    res.str_refs.resize(total_cols);

    // Probe phase.
    for (size_t rb = 0; rb < B_out.size(); ++rb) {
        const value_t &vkey = B_out[rb][0];
        if (!vkey.is_int()) continue;

        int32_t key = vkey.get_int();
        auto hit = build_map.find(key);
        if (hit == build_map.end()) continue;

        for (size_t ra : hit->second) {

            // Append A columns.
            for (size_t c = 0; c < nA; ++c) {
                const value_t &v = A_out[ra][c];
                if (res.is_int_col[c])
                    res.int_cols[c].push_back(v.get_int());
                else
                    res.str_refs[c].push_back(v.get_sref());
            }

            // Append B columns.
            for (size_t c = 0; c < nB; ++c) {
                size_t outc = nA + c;
                const value_t &v = B_out[rb][c];
                if (res.is_int_col[outc])
                    res.int_cols[outc].push_back(v.get_int());
                else
                    res.str_refs[outc].push_back(v.get_sref());
            }

            res.num_rows++;
        }
    }

    return res;
}

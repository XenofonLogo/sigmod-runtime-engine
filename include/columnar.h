#pragma once

#include <vector>
#include <cstddef>
#include "plan.h"
#include "table.h"
#include "late_materialization.h"

namespace Contest {

// Helper for reading a bitmap (1=valid, 0=null) inlined for speed
static inline bool get_bitmap_local_col(const uint8_t* bitmap, uint16_t idx) {
    auto byte_idx = idx / 8;
    auto bit = idx % 8;
    return bitmap[byte_idx] & (1u << bit);
}

// Column storage with optional zero-copy for INT32 without NULLs to avoid materialization
struct column_t {
    std::vector<std::vector<value_t>> pages;    // Pages of stored value_t

    const Column* src_column = nullptr;         // Source for zero-copy (INT32 without NULL)
    std::vector<size_t> page_offsets;           // Cumulative offsets per page
    bool is_zero_copy = false;                  // Zero-copy enabled flag

    size_t values_per_page = 1024;              // Page size in number of value_t
    size_t num_values = 0;                      // Total stored values

    mutable size_t cached_page_idx = 0;         // Page cache for sequential access

    column_t() = default;
    explicit column_t(size_t page_size) : values_per_page(page_size), num_values(0) {}

    // Append a value, auto-creating/filling pages as needed
    void append(const value_t& v) {
        if (pages.empty() || pages.back().size() >= values_per_page) {
            pages.emplace_back();
            pages.back().reserve(values_per_page);
        }
        pages.back().push_back(v);
        ++num_values;
    }

    const value_t& get(size_t row_idx) const {
        // ZERO-COPY path: avoids materialization and bitmap checks
        if (is_zero_copy && src_column != nullptr) {
            static thread_local value_t tmp;
            
            // Start from cached page (typical sequential access)
            size_t page_idx = cached_page_idx;
            
            // Check if the row is in the cached page
            if (page_idx < page_offsets.size() - 1 &&
                row_idx >= page_offsets[page_idx] &&
                row_idx < page_offsets[page_idx + 1]) {
                // Cache hit
            }
            // Check next page (common for sequential patterns)
            else if (page_idx + 1 < page_offsets.size() - 1 &&
                     row_idx >= page_offsets[page_idx + 1] &&
                     row_idx < page_offsets[page_idx + 2]) {
                cached_page_idx = ++page_idx;
            }
            // Binary search for the page
            else {
                size_t left = 0, right = page_offsets.size() - 1;
                while (left < right - 1) {
                    size_t mid = (left + right) / 2;
                    if (row_idx < page_offsets[mid]) {
                        right = mid;
                    } else {
                        left = mid;
                    }
                }
                page_idx = left;
                cached_page_idx = page_idx;
            }
            
            // Compute position within the page
            size_t slot = row_idx - page_offsets[page_idx];
            
            // Direct read from raw page (INT32 data starts at +4)
            auto* page = src_column->pages[page_idx]->data;
            auto* data = reinterpret_cast<const int32_t*>(page + 4);
            
            tmp = value_t::make_i32(data[slot]);
            return tmp;
        }

        // STANDARD path: access materialized value_t
        size_t page_idx = row_idx / values_per_page;
        size_t offset_in_page = row_idx % values_per_page;
        return pages[page_idx][offset_in_page];
    }

    // Thread-safe accessor without shared mutable state (returns by value)
    value_t get_cached(size_t row_idx, size_t& page_cache) const {
        if (is_zero_copy && src_column != nullptr) {
            size_t page_idx = page_cache;
            if (page_idx >= page_offsets.size() - 1) page_idx = 0;

            // Fast path: cache hit
            if (row_idx >= page_offsets[page_idx] && row_idx < page_offsets[page_idx + 1]) {
                // ok
            }
            // Check next page (useful for semi-clustered access)
            else if (page_idx + 1 < page_offsets.size() - 1 && row_idx >= page_offsets[page_idx + 1] &&
                     row_idx < page_offsets[page_idx + 2]) {
                ++page_idx;
            } else {
                size_t left = 0, right = page_offsets.size() - 1;
                while (left < right - 1) {
                    size_t mid = (left + right) / 2;
                    if (row_idx < page_offsets[mid]) right = mid;
                    else left = mid;
                }
                page_idx = left;
            }

            page_cache = page_idx;
            const size_t slot = row_idx - page_offsets[page_idx];      // Position within the page
            auto* page = src_column->pages[page_idx]->data;
            auto* data = reinterpret_cast<const int32_t*>(page + 4);
            return value_t::make_i32(data[slot]);
        }

        const size_t page_idx = row_idx / values_per_page;
        const size_t offset_in_page = row_idx % values_per_page;
        return pages[page_idx][offset_in_page];
    }

    class Iterator {
    public:
        Iterator(const column_t* col, size_t idx) : column(col), row_idx(idx) {} // Holds pointer + position
        const value_t& operator*() const { return column->get(row_idx); }        // Access current value
        Iterator& operator++() { ++row_idx; return *this; }                       // Move to next
        bool operator!=(const Iterator& other) const { return row_idx != other.row_idx; } // End comparison
    private:
        const column_t* column; // Data source
        size_t row_idx;         // Current index
    };

    Iterator begin() const { return Iterator(this, 0); }             // Start iteration
    Iterator end() const { return Iterator(this, num_values); }      // End iteration
    size_t size() const { return num_values; }                       // Number of elements
};

struct ColumnBuffer {
    std::vector<column_t> columns;   // Data columns
    size_t num_rows = 0;             // Number of rows
    std::vector<DataType> types;     // Column types

    ColumnBuffer() = default;
    ColumnBuffer(size_t cols, size_t rows) : columns(cols), num_rows(rows) {
        for (auto &c : columns) c = column_t(1024); // Initialize fixed page size
    }
    size_t num_cols() const { return columns.size(); } // Return number of columns
};

// Columnar operations
ColumnBuffer scan_columnar_to_columnbuffer(const Plan& plan,
    const ScanNode& scan,
    const std::vector<std::tuple<size_t, DataType>>& output_attrs);

ColumnBuffer join_columnbuffer_hash(const Plan& plan,
    const JoinNode& join,
    const std::vector<std::tuple<size_t, DataType>>& output_attrs,
    const ColumnBuffer& left,
    const ColumnBuffer& right);

ColumnarTable finalize_columnbuffer_to_columnar(const Plan& plan,
    const ColumnBuffer& buf,
    const std::vector<std::tuple<size_t, DataType>>& output_attrs);

} // namespace Contest
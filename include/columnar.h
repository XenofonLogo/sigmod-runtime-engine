#pragma once

#include <vector>
#include <cstddef>
#include "plan.h"
#include "table.h"
#include "late_materialization.h"

namespace Contest {

// Helper function to read bitmap
static inline bool get_bitmap_local_col(const uint8_t* bitmap, uint16_t idx) {
    auto byte_idx = idx / 8;
    auto bit = idx % 8;
    return bitmap[byte_idx] & (1u << bit);
}

// Column store with smart zero-copy
struct column_t {
    std::vector<std::vector<value_t>> pages;

    // Zero-copy mode for INT32 without nulls
    const Column* src_column = nullptr;
    std::vector<size_t> page_offsets;  // Cumulative row counts per page
    bool is_zero_copy = false;

    size_t values_per_page = 1024;
    size_t num_values = 0;

    // CACHE for sequential access optimization
    mutable size_t cached_page_idx = 0;

    column_t() = default;
    explicit column_t(size_t page_size) : values_per_page(page_size), num_values(0) {}

    void append(const value_t& v) {
        if (pages.empty() || pages.back().size() >= values_per_page) {
            pages.emplace_back();
            pages.back().reserve(values_per_page);
        }
        pages.back().push_back(v);
        ++num_values;
    }

    const value_t& get(size_t row_idx) const {
        // ZERO-COPY PATH with cached page lookup
        if (is_zero_copy && src_column != nullptr) {
            static thread_local value_t tmp;
            
            // Start search from cached page (exploits sequential access)
            size_t page_idx = cached_page_idx;
            
            // Check if row is in cached page
            if (page_idx < page_offsets.size() - 1 &&
                row_idx >= page_offsets[page_idx] &&
                row_idx < page_offsets[page_idx + 1]) {
                // Cache hit! Use cached page
            }
            // Check next page (very common in sequential scans)
            else if (page_idx + 1 < page_offsets.size() - 1 &&
                     row_idx >= page_offsets[page_idx + 1] &&
                     row_idx < page_offsets[page_idx + 2]) {
                cached_page_idx = ++page_idx;
            }
            // Binary search for the page
            else {
                // Binary search in page_offsets
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
            
            // Calculate slot within the page
            size_t slot = row_idx - page_offsets[page_idx];
            
            // Read directly from the source page
            auto* page = src_column->pages[page_idx]->data;
            auto* data = reinterpret_cast<const int32_t*>(page + 4);
            
            tmp = value_t::make_i32(data[slot]);
            return tmp;
        }

        // STANDARD PATH: Access materialized data
        size_t page_idx = row_idx / values_per_page;
        size_t offset_in_page = row_idx % values_per_page;
        return pages[page_idx][offset_in_page];
    }

    class Iterator {
    public:
        Iterator(const column_t* col, size_t idx) : column(col), row_idx(idx) {}
        const value_t& operator*() const { return column->get(row_idx); }
        Iterator& operator++() { ++row_idx; return *this; }
        bool operator!=(const Iterator& other) const { return row_idx != other.row_idx; }
    private:
        const column_t* column;
        size_t row_idx;
    };

    Iterator begin() const { return Iterator(this, 0); }
    Iterator end() const { return Iterator(this, num_values); }
    size_t size() const { return num_values; }
};

struct ColumnBuffer {
    std::vector<column_t> columns;
    size_t num_rows = 0;
    std::vector<DataType> types;

    ColumnBuffer() = default;
    ColumnBuffer(size_t cols, size_t rows) : columns(cols), num_rows(rows) {
        for (auto &c : columns) c = column_t(1024);
    }
    size_t num_cols() const { return columns.size(); }
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
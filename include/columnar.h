#pragma once

#include <vector>
#include <cstddef>

#include "plan.h"
#include "table.h"
#include "late_materialization.h"

namespace Contest {

// ============================================================================
//  Helper: Ανάγνωση bitmap (true = non-null)
// ============================================================================
static inline bool get_bitmap_local_col(const uint8_t* bitmap, uint16_t idx) {
    size_t byte_idx = idx / 8;
    size_t bit_idx  = idx % 8;
    return bitmap[byte_idx] & (1u << bit_idx);
}

// ============================================================================
//  column_t
// ============================================================================
//
//  Λογική αναπαράσταση στήλης κατά την εκτέλεση:
//   - είτε materialized (pages<vector<value_t>>)
//   - είτε zero-copy (INT32 χωρίς NULLs)
//
struct column_t {

    // ----------------------------------------------------------------
    // Materialized δεδομένα
    // ----------------------------------------------------------------
    std::vector<std::vector<value_t>> pages;

    // ----------------------------------------------------------------
    // Zero-copy δεδομένα
    // ----------------------------------------------------------------
    const Column* src_column = nullptr;      // pointer στο input Column
    std::vector<size_t> page_offsets;        // cumulative rows ανά page
    bool is_zero_copy = false;

    // ----------------------------------------------------------------
    // Metadata
    // ----------------------------------------------------------------
    size_t values_per_page = 1024;
    size_t num_values = 0;

    // Cache για γρήγορη σειριακή πρόσβαση
    mutable size_t cached_page_idx = 0;

    column_t() = default;
    explicit column_t(size_t page_size)
        : values_per_page(page_size), num_values(0) {}

    // ----------------------------------------------------------------
    // Materialized append
    // ----------------------------------------------------------------
    void append(const value_t& v) {
        if (pages.empty() || pages.back().size() >= values_per_page) {
            pages.emplace_back();
            pages.back().reserve(values_per_page);
        }
        pages.back().push_back(v);
        ++num_values;
    }

    // ----------------------------------------------------------------
    // Ανάγνωση τιμής
    // ----------------------------------------------------------------
    const value_t& get(size_t row_idx) const {

        // =============================================================
        // ZERO-COPY PATH
        // =============================================================
        if (is_zero_copy && src_column != nullptr) {

            // thread_local για ασφαλή επιστροφή reference
            static thread_local value_t tmp;

            size_t page_idx = cached_page_idx;

            // Cache hit
            if (page_idx + 1 < page_offsets.size() &&
                row_idx >= page_offsets[page_idx] &&
                row_idx <  page_offsets[page_idx + 1]) {
                // ok
            }
            // Cache +1
            else if (page_idx + 2 < page_offsets.size() &&
                     row_idx >= page_offsets[page_idx + 1] &&
                     row_idx <  page_offsets[page_idx + 2]) {
                cached_page_idx = ++page_idx;
            }
            // Binary search
            else {
                size_t l = 0, r = page_offsets.size() - 1;
                while (l < r - 1) {
                    size_t m = (l + r) / 2;
                    if (row_idx < page_offsets[m]) r = m;
                    else l = m;
                }
                page_idx = l;
                cached_page_idx = l;
            }

            size_t slot = row_idx - page_offsets[page_idx];

            const uint8_t* page =
                reinterpret_cast<const uint8_t*>(
                    src_column->pages[page_idx]->data);

            const int32_t* data =
                reinterpret_cast<const int32_t*>(page + 4);

            tmp = value_t::make_i32(data[slot]);
            return tmp;
        }

        // =============================================================
        // MATERIALIZED PATH
        // =============================================================
        size_t page_idx = row_idx / values_per_page;
        size_t offset   = row_idx % values_per_page;
        return pages[page_idx][offset];
    }

    // ----------------------------------------------------------------
    // Iterators
    // ----------------------------------------------------------------
    class Iterator {
    public:
        Iterator(const column_t* col, size_t idx)
            : column(col), row_idx(idx) {}

        const value_t& operator*() const {
            return column->get(row_idx);
        }

        Iterator& operator++() {
            ++row_idx;
            return *this;
        }

        bool operator!=(const Iterator& other) const {
            return row_idx != other.row_idx;
        }

    private:
        const column_t* column;
        size_t row_idx;
    };

    Iterator begin() const { return Iterator(this, 0); }
    Iterator end()   const { return Iterator(this, num_values); }

    size_t size() const { return num_values; }
};

// ============================================================================
// ColumnBuffer
// ============================================================================
struct ColumnBuffer {

    std::vector<column_t> columns;
    size_t num_rows = 0;
    std::vector<DataType> types;

    ColumnBuffer() = default;

    ColumnBuffer(size_t cols, size_t rows)
        : columns(cols), num_rows(rows) {
        for (auto& c : columns) {
            c = column_t(1024);
        }
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
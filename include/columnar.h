#pragma once

#include <vector>
#include <cstddef>
#include "plan.h"
#include "table.h"
#include "late_materialization.h"

namespace Contest {

// Column store
struct column_t {
    std::vector<std::vector<value_t>> pages;
    size_t values_per_page = 1024;
    size_t num_values = 0;

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

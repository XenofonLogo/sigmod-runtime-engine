#pragma once

#include <vector>
#include <cstddef>
#include "plan.h"
#include "table.h"
#include "late_materialization.h"

namespace Contest {

// Βοηθός ανάγνωσης bitmap (1=valid, 0=null) σε inline μορφή για ταχύτητα
static inline bool get_bitmap_local_col(const uint8_t* bitmap, uint16_t idx) {
    auto byte_idx = idx / 8;
    auto bit = idx % 8;
    return bitmap[byte_idx] & (1u << bit);
}

// Αποθήκευση στηλών με επιλογή zero-copy για INT32 χωρίς NULLs ώστε να αποφεύγεται materialization
struct column_t {
    std::vector<std::vector<value_t>> pages;    // Σελίδες αποθηκευμένων value_t

    const Column* src_column = nullptr;         // Πηγή zero-copy (INT32 χωρίς NULL)
    std::vector<size_t> page_offsets;           // Αθροιστικά offsets ανά σελίδα
    bool is_zero_copy = false;                  // Flag ενεργοποίησης zero-copy

    size_t values_per_page = 1024;              // Μέγεθος σελίδας σε πλήθος value_t
    size_t num_values = 0;                      // Σύνολο αποθηκευμένων τιμών

    mutable size_t cached_page_idx = 0;         // Cache σελίδας για σειριακή πρόσβαση

    column_t() = default;
    explicit column_t(size_t page_size) : values_per_page(page_size), num_values(0) {}

    // Προσθήκη τιμής με αυτόματη δημιουργία/γεμίσματος σελίδων
    void append(const value_t& v) {
        if (pages.empty() || pages.back().size() >= values_per_page) {
            pages.emplace_back();
            pages.back().reserve(values_per_page);
        }
        pages.back().push_back(v);
        ++num_values;
    }

    const value_t& get(size_t row_idx) const {
        // ZERO-COPY διαδρομή: αποφεύγει materialization και bitmap check
        if (is_zero_copy && src_column != nullptr) {
            static thread_local value_t tmp;
            
            // Ξεκίνα από cached σελίδα (τυπικά διαδοχική πρόσβαση)
            size_t page_idx = cached_page_idx;
            
            // Έλεγχος αν το row είναι στην cached σελίδα
            if (page_idx < page_offsets.size() - 1 &&
                row_idx >= page_offsets[page_idx] &&
                row_idx < page_offsets[page_idx + 1]) {
                // Cache hit
            }
            // Έλεγχος επόμενης σελίδας (συχνό σε σειριακά patterns)
            else if (page_idx + 1 < page_offsets.size() - 1 &&
                     row_idx >= page_offsets[page_idx + 1] &&
                     row_idx < page_offsets[page_idx + 2]) {
                cached_page_idx = ++page_idx;
            }
            // Δυαδική αναζήτηση σελίδας
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
            
            // Υπολογισμός θέσης μέσα στη σελίδα
            size_t slot = row_idx - page_offsets[page_idx];
            
            // Άμεση ανάγνωση από raw page (INT32 data ξεκινά στο +4)
            auto* page = src_column->pages[page_idx]->data;
            auto* data = reinterpret_cast<const int32_t*>(page + 4);
            
            tmp = value_t::make_i32(data[slot]);
            return tmp;
        }

        // STANDARD διαδρομή: πρόσβαση σε materialized value_t
        size_t page_idx = row_idx / values_per_page;
        size_t offset_in_page = row_idx % values_per_page;
        return pages[page_idx][offset_in_page];
    }

    // Thread-safe accessor χωρίς χρήση shared mutable state (επιστρέφει by value)
    value_t get_cached(size_t row_idx, size_t& page_cache) const {
        if (is_zero_copy && src_column != nullptr) {
            size_t page_idx = page_cache;
            if (page_idx >= page_offsets.size() - 1) page_idx = 0;

            // Γρήγορη διαδρομή: cache hit
            if (row_idx >= page_offsets[page_idx] && row_idx < page_offsets[page_idx + 1]) {
                // ok
            }
            // Έλεγχος επόμενης σελίδας (χρήσιμο σε ημι-συσπειρωμένη πρόσβαση)
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
            const size_t slot = row_idx - page_offsets[page_idx];      // Θέση μέσα στη σελίδα
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
        Iterator(const column_t* col, size_t idx) : column(col), row_idx(idx) {} // Δεσμεύει δείκτη + θέση
        const value_t& operator*() const { return column->get(row_idx); }        // Πρόσβαση τρέχουσας τιμής
        Iterator& operator++() { ++row_idx; return *this; }                       // Επόμενη τιμή
        bool operator!=(const Iterator& other) const { return row_idx != other.row_idx; } // Σύγκριση τέλους
    private:
        const column_t* column; // Πηγή δεδομένων
        size_t row_idx;         // Τρέχον index
    };

    Iterator begin() const { return Iterator(this, 0); }             // Έναρξη iteration
    Iterator end() const { return Iterator(this, num_values); }      // Τέλος iteration
    size_t size() const { return num_values; }                       // Πλήθος στοιχείων
};

struct ColumnBuffer {
    std::vector<column_t> columns;   // Στήλες δεδομένων
    size_t num_rows = 0;             // Πλήθος γραμμών
    std::vector<DataType> types;     // Τύποι στηλών

    ColumnBuffer() = default;
    ColumnBuffer(size_t cols, size_t rows) : columns(cols), num_rows(rows) {
        for (auto &c : columns) c = column_t(1024); // Αρχικοποίηση σταθερού page size
    }
    size_t num_cols() const { return columns.size(); } // Επιστρέφει πλήθος στηλών
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
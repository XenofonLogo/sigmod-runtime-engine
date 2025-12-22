#include "column_zero_copy.h"

namespace Contest {

bool can_zero_copy_int32(const Column& column) {
    if (column.type != DataType::INT32) return false;
    if (column.pages.empty()) return true;

    // Έλεγχος bitmap μόνο
    for (const auto& page_ptr : column.pages) {
        auto* page = page_ptr->data;
        uint16_t num_rows = *reinterpret_cast<const uint16_t*>(page);
        auto* bitmap = reinterpret_cast<const uint8_t*>(
            page + PAGE_SIZE - (num_rows + 7) / 8
        );

        size_t bytes = (num_rows + 7) / 8;
        for (size_t i = 0; i < bytes; ++i) {
            if (bitmap[i] != 0xFF) return false;
        }
    }
    return true;
}

void init_zero_copy_column(
    column_t& out,
    const Column& src,
    size_t total_rows)
{
    out.is_zero_copy = true;
    out.src_column = &src;
    out.num_values = total_rows;
    out.page_offsets.clear();
    out.page_offsets.push_back(0);

    size_t cumulative = 0;
    for (const auto& page_ptr : src.pages) {
        auto* page = page_ptr->data;
        uint16_t rows = *reinterpret_cast<const uint16_t*>(page);
        cumulative += rows;
        out.page_offsets.push_back(cumulative);
    }
}

}

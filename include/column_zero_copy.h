#pragma once
#include "columnar.h"

namespace Contest {

// Ελέγχει αν μια INT32 στήλη ΔΕΝ έχει nulls
bool can_zero_copy_int32(const Column& column);

// Αρχικοποιεί column_t σε zero-copy mode
void init_zero_copy_column(
    column_t& out,
    const Column& src,
    size_t total_rows);

}

#include "late_materialization.h"

// Intentionally empty.
// All LM logic (string storage, decoding, joins, buffer ops)
// happens inside columnar.cpp.
//
// This file exists only to satisfy the build system.
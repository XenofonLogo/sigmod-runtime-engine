#ifndef JOIN_TELEMETRY_H
#define JOIN_TELEMETRY_H

#include <cstdint>
#include <atomic>

namespace Contest {

struct QueryTelemetry {
    uint64_t joins = 0;
    uint64_t build_rows = 0;
    uint64_t probe_rows = 0;
    uint64_t out_rows = 0;
    uint64_t out_cells = 0;
    uint64_t bytes_strict_min = 0; // keys + output writes
    uint64_t bytes_likely = 0;     // + output reads (value_t)
};

// Check if telemetry is enabled via environment variable (JOIN_TELEMETRY, default on)
bool join_telemetry_enabled();

// Initialize telemetry for a new query
void qt_begin_query();

// Record statistics for a single join operation
void qt_add_join(uint64_t build_rows,
                 uint64_t probe_rows,
                 uint64_t out_rows,
                 uint64_t out_cols);

// Print telemetry summary for the query
void qt_end_query();

} // namespace Contest

#endif // JOIN_TELEMETRY_H

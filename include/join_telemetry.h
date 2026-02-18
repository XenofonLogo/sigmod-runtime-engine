#ifndef JOIN_TELEMETRY_H
#define JOIN_TELEMETRY_H

#include <cstdint>
#include <atomic>

namespace Contest {

// Aggregate metrics for a query
struct QueryTelemetry {
    uint64_t joins = 0;            // Number of joins
    uint64_t build_rows = 0;       // Total rows on the build side
    uint64_t probe_rows = 0;       // Total rows on the probe side
    uint64_t out_rows = 0;         // Output rows
    uint64_t out_cells = 0;        // Output cells (rows * cols)
    uint64_t bytes_baseline_min = 0; // Minimum bytes (keys + writes)
    uint64_t bytes_likely = 0;     // Estimated bytes (including reads)
};

// Check if telemetry is enabled (env JOIN_TELEMETRY, default disabled)
bool join_telemetry_enabled();

// Initialize telemetry for a new query
void qt_begin_query();

// Record metrics for a join
void qt_add_join(uint64_t build_rows,
                 uint64_t probe_rows,
                 uint64_t out_rows,
                 uint64_t out_cols);

// Print telemetry summary for the query
void qt_end_query();

} // namespace Contest

#endif // JOIN_TELEMETRY_H

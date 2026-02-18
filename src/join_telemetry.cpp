#include "join_telemetry.h"
#include <cstdio>
#include <cstdlib>
#include <chrono>

// join_telemetry.cpp - optional statistics for joins
#include "join_telemetry.h"
#include <cstdio>
#include <cstdlib>
#include <chrono>
#include "join_telemetry.h"
#include <cstdio>
#include <cstdlib>
#include <chrono>

namespace Contest {

// Increasing identifier per query (shared across threads)
static std::atomic<uint64_t> g_query_seq{0};
// Telemetry measurements stored per-thread for the current query
static thread_local QueryTelemetry g_qt;
// Current query ID (per thread), coordinated with g_query_seq
static thread_local uint64_t g_query_id = 0;
// Query start time (per thread)
static thread_local std::chrono::steady_clock::time_point g_query_start;

bool join_telemetry_enabled() {
    // Optional stats to check if we are memory-bandwidth bound.
    // Default: disabled. Set JOIN_TELEMETRY=1 to enable (any non-'0' value).
    static const bool enabled = [] {
        const char* v = std::getenv("JOIN_TELEMETRY");
        if (!v) return false;  // Default to disabled
        return *v && *v != '0';
    }();
    return enabled;
}

void qt_begin_query() {
    g_query_id = ++g_query_seq;   // Get new ID (atomic increment)
    g_qt = QueryTelemetry{};      // Reset/restart measurements
    g_query_start = std::chrono::steady_clock::now();
}

void qt_add_join(uint64_t build_rows,
                 uint64_t probe_rows,
                 uint64_t out_rows,
                 uint64_t out_cols) {
    g_qt.joins += 1;
    g_qt.build_rows += build_rows;
    g_qt.probe_rows += probe_rows;
    g_qt.out_rows += out_rows;

    const uint64_t out_cells = out_rows * out_cols;
    g_qt.out_cells += out_cells;

    // Estimate moved bytes to provide a simple view of bandwidth
    const uint64_t bytes_keys = (build_rows + probe_rows) * 4ull;   // INT32 join keys (4 bytes)
    const uint64_t bytes_out_write = out_cells * 8ull;              // Writes of value_t (8 bytes)
    const uint64_t bytes_out_read = out_cells * 8ull;               // Reads of value_t (8 bytes)

    // Baseline min: keys + output writes (lower bound)
    g_qt.bytes_baseline_min += bytes_keys + bytes_out_write;
    // Likely: keys + reads + writes (more realistic for hash joins)
    g_qt.bytes_likely += bytes_keys + bytes_out_read + bytes_out_write;
}

void qt_end_query() {
    const auto now = std::chrono::steady_clock::now();
    const double elapsed_ms = std::chrono::duration<double, std::milli>(now - g_query_start).count();
    const double elapsed_s = elapsed_ms / 1000.0;

    const double selectivity = (g_qt.probe_rows == 0) ? 0.0 : static_cast<double>(g_qt.out_rows) / static_cast<double>(g_qt.probe_rows);
    const double avg_out_cols = (g_qt.out_rows == 0) ? 0.0 : static_cast<double>(g_qt.out_cells) / static_cast<double>(g_qt.out_rows);

    const auto safe_div = [](uint64_t bytes, double seconds) -> double {
        if (seconds <= 0.0) return 0.0;
        return static_cast<double>(bytes) / (seconds * 1e9);
    };

    const auto bytes_to_gib = [](uint64_t bytes) -> double {
        return static_cast<double>(bytes) / (1024.0 * 1024.0 * 1024.0);
    };
    const auto ms_at_gbps = [](uint64_t bytes, double gb_per_s) -> double {
        const double seconds = static_cast<double>(bytes) / (gb_per_s * 1e9);
        return seconds * 1000.0;
    };

    // Hypothetical bounds based on bandwidth only (optimistic for hash joins)
    const double bw10 = 10.0;
    const double bw20 = 20.0;
    const double bw40 = 40.0;

    // Summary line for counts and row metrics
    std::fprintf(stderr,
                 "[telemetry q%llu] joins=%llu build=%llu probe=%llu out=%llu out_cells=%llu sel=%.4f avg_out_cols=%.2f\n",
                 (unsigned long long)g_query_id,
                 (unsigned long long)g_qt.joins,
                 (unsigned long long)g_qt.build_rows,
                 (unsigned long long)g_qt.probe_rows,
                 (unsigned long long)g_qt.out_rows,
                 (unsigned long long)g_qt.out_cells,
                 selectivity,
                 avg_out_cols);

    // Estimate data volume in GiB (baseline and likely scenarios)
    std::fprintf(stderr,
                 "[telemetry q%llu] bytes_baseline_min=%.3f GiB  bytes_likely=%.3f GiB\n",
                 (unsigned long long)g_query_id,
                 bytes_to_gib(g_qt.bytes_baseline_min),
                 bytes_to_gib(g_qt.bytes_likely));

    // Actual elapsed time and measured bandwidth
    std::fprintf(stderr,
                 "[telemetry q%llu] elapsed=%.3f ms  bw_baseline=%.2f GB/s  bw_likely=%.2f GB/s\n",
                 (unsigned long long)g_query_id,
                 elapsed_ms,
                 safe_div(g_qt.bytes_baseline_min, elapsed_s),
                 safe_div(g_qt.bytes_likely, elapsed_s));

    // Theoretical lower bounds (bandwidth-only) for baseline bytes
    std::fprintf(stderr,
                 "[telemetry q%llu] BW LB baseline: %.2f/%.2f/%.2f ms @ %.0f/%.0f/%.0f GB/s\n",
                 (unsigned long long)g_query_id,
                 ms_at_gbps(g_qt.bytes_baseline_min, bw10),
                 ms_at_gbps(g_qt.bytes_baseline_min, bw20),
                 ms_at_gbps(g_qt.bytes_baseline_min, bw40),
                 bw10, bw20, bw40);

    // Theoretical lower bound times (bandwidth-only) for likely bytes
    std::fprintf(stderr,
                 "[telemetry q%llu] BW LB likely: %.2f/%.2f/%.2f ms @ %.0f/%.0f/%.0f GB/s\n",
                 (unsigned long long)g_query_id,
                 ms_at_gbps(g_qt.bytes_likely, bw10),
                 ms_at_gbps(g_qt.bytes_likely, bw20),
                 ms_at_gbps(g_qt.bytes_likely, bw40),
                 bw10, bw20, bw40);
}

} // namespace Contest

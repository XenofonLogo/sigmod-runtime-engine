// join_telemetry.cpp — προαιρετικά στατιστικά για joins
#include "join_telemetry.h"
#include <cstdio>
#include <cstdlib>

namespace Contest {

// Αυξανόμενο αναγνωριστικό ανά query (shared μεταξύ νημάτων)
static std::atomic<uint64_t> g_query_seq{0};
// Μετρήσεις τηλεμετρίας αποθηκευμένες ανά thread για το τρέχον query
static thread_local QueryTelemetry g_qt;
// ID τρέχοντος query (ανά thread), συντονίζεται με g_query_seq
static thread_local uint64_t g_query_id = 0;

bool join_telemetry_enabled() {
    // Προαιρετικά stats για να δούμε αν είμαστε memory-bandwidth bound.
    // Default: απενεργοποιημένο. JOIN_TELEMETRY=1 για ενεργοποίηση (οτιδήποτε μη '0').
    static const bool enabled = [] {
        const char* v = std::getenv("JOIN_TELEMETRY");
        if (!v) return false;  // Default to disabled
        return *v && *v != '0';
    }();
    return enabled;
}

void qt_begin_query() {
    g_query_id = ++g_query_seq;   // Πάρε νέο ID (atomic increment)
    g_qt = QueryTelemetry{};      // Μηδενισμός/επανεκκίνηση μετρήσεων
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

    // Εκτίμηση μετακινούμενων bytes για απλή εικόνα bandwidth
    const uint64_t bytes_keys = (build_rows + probe_rows) * 4ull;   // INT32 join keys (4 bytes)
    const uint64_t bytes_out_write = out_cells * 8ull;              // Γραφές value_t (8 bytes)
    const uint64_t bytes_out_read = out_cells * 8ull;               // Αναγνώσεις value_t (8 bytes)

    // Strict min: κλειδιά + γραφές εξόδου (κατώτερο όριο)
    g_qt.bytes_strict_min += bytes_keys + bytes_out_write;
    // Likely: κλειδιά + αναγνώσεις + γραφές (πιο ρεαλιστικό για hash join)
    g_qt.bytes_likely += bytes_keys + bytes_out_read + bytes_out_write;
}

void qt_end_query() {
    const auto bytes_to_gib = [](uint64_t bytes) -> double {
        return static_cast<double>(bytes) / (1024.0 * 1024.0 * 1024.0);
    };
    const auto ms_at_gbps = [](uint64_t bytes, double gb_per_s) -> double {
        const double seconds = static_cast<double>(bytes) / (gb_per_s * 1e9);
        return seconds * 1000.0;
    };

    // Υποθετικά όρια μόνο με βάση bandwidth (αισιόδοξα για hash joins)
    const double bw10 = 10.0;
    const double bw20 = 20.0;
    const double bw40 = 40.0;

    // Γραμμή σύνοψης πλήθους πράξεων και γραμμών
    std::fprintf(stderr,
                 "[telemetry q%llu] joins=%llu build=%llu probe=%llu out=%llu out_cells=%llu\n",
                 (unsigned long long)g_query_id,
                 (unsigned long long)g_qt.joins,
                 (unsigned long long)g_qt.build_rows,
                 (unsigned long long)g_qt.probe_rows,
                 (unsigned long long)g_qt.out_rows,
                 (unsigned long long)g_qt.out_cells);

    // Εκτίμηση όγκου δεδομένων σε GiB (κατώτερο και πιθανό σενάριο)
    std::fprintf(stderr,
                 "[telemetry q%llu] bytes_strict_min=%.3f GiB  bytes_likely=%.3f GiB\n",
                 (unsigned long long)g_query_id,
                 bytes_to_gib(g_qt.bytes_strict_min),
                 bytes_to_gib(g_qt.bytes_likely));

    // Θεωρητικά κάτω όρια χρόνου (μόνο bandwidth) για strict bytes
    std::fprintf(stderr,
                 "[telemetry q%llu] BW LB strict: %.2f/%.2f/%.2f ms @ %.0f/%.0f/%.0f GB/s\n",
                 (unsigned long long)g_query_id,
                 ms_at_gbps(g_qt.bytes_strict_min, bw10),
                 ms_at_gbps(g_qt.bytes_strict_min, bw20),
                 ms_at_gbps(g_qt.bytes_strict_min, bw40),
                 bw10, bw20, bw40);

    // Θεωρητικά κάτω όρια χρόνου (μόνο bandwidth) για likely bytes
    std::fprintf(stderr,
                 "[telemetry q%llu] BW LB likely: %.2f/%.2f/%.2f ms @ %.0f/%.0f/%.0f GB/s\n",
                 (unsigned long long)g_query_id,
                 ms_at_gbps(g_qt.bytes_likely, bw10),
                 ms_at_gbps(g_qt.bytes_likely, bw20),
                 ms_at_gbps(g_qt.bytes_likely, bw40),
                 bw10, bw20, bw40);
}

} // namespace Contest

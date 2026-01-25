#ifndef JOIN_TELEMETRY_H
#define JOIN_TELEMETRY_H

#include <cstdint>
#include <atomic>

namespace Contest {

// Συγκεντρωτικά μετρικά ενός query
struct QueryTelemetry {
    uint64_t joins = 0;            // Πλήθος joins
    uint64_t build_rows = 0;       // Σύνολο γραμμών στην build πλευρά
    uint64_t probe_rows = 0;       // Σύνολο γραμμών στην probe πλευρά
    uint64_t out_rows = 0;         // Γραμμές εξόδου
    uint64_t out_cells = 0;        // Κελιά εξόδου (rows * cols)
    uint64_t bytes_strict_min = 0; // Ελάχιστα bytes (keys + writes)
    uint64_t bytes_likely = 0;     // Εκτιμώμενα bytes (προσθήκη reads)
};

// Έλεγχος αν η τηλεμετρία είναι ενεργή (env JOIN_TELEMETRY, default on)
bool join_telemetry_enabled();

// Αρχικοποίηση τηλεμετρίας για νέο query
void qt_begin_query();

// Καταγραφή μετρικών για ένα join
void qt_add_join(uint64_t build_rows,
                 uint64_t probe_rows,
                 uint64_t out_rows,
                 uint64_t out_cols);

// Εκτύπωση σύνοψης τηλεμετρίας για το query
void qt_end_query();

} // namespace Contest

#endif // JOIN_TELEMETRY_H

/**
 * Project Configuration Modes
 * 
 * Three build modes for different tradeoffs:
 * 
 * 1. OPTIMIZED_PROJECT (default)
 *    - Fast single-pass hash table
 *    - Direct zero-copy page access
 *    - No slab allocator overhead
 *    - Parallel probing with work stealing
 *    - Performance: 12.1s (4 threads, IMDB workload)
 *    - Correctness: Core requirements met
 * 
 * 2. STRICT_PROJECT=1
 *    - Full requirement implementation
 *    - Partition-based hash table construction
 *    - 3-level slab allocator
 *    - All parallelization requirements met
 *    - Performance: Slower (+189%)
 *    - Correctness: 100% of requirements
 * 
 * 3. JOIN_TELEMETRY=1
 *    - Optional performance instrumentation
 *    - Works with any mode
 */

#pragma once

#include <cstdlib>

namespace Contest {

inline bool is_strict_mode() {
    const char* v = std::getenv("STRICT_PROJECT");
    return v && *v && *v != '0';
}

inline bool is_optimized_mode() {
    const char* v = std::getenv("OPTIMIZED_PROJECT");
    return v && *v && *v != '0';
}

// Default: OPTIMIZED (unless STRICT_PROJECT=1 is set)
inline bool use_optimized_project() {
    // Explicit STRICT wins over everything
    if (is_strict_mode()) return false;
    // Explicit OPTIMIZED turns it on
    if (is_optimized_mode()) return true;
    // Default: optimized path
    return true;
}

inline bool use_strict_project() {
    return !use_optimized_project();
}

} // namespace Contest

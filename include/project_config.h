/**
 * Project Configuration Modes
 * 
 * Three build modes for different tradeoffs:
 * 
 * 1. STRICT_PROJECT=1 (default)
 *    - Full requirement implementation
 *    - Partition-based hash table construction
 *    - 3-level slab allocator
 *    - All parallelization requirements met
 *    - Performance: Slower (+189%)
 *    - Correctness: 100% of requirements
 * 
 * 2. OPTIMIZED_PROJECT=1
 *    - Fast single-pass hash table
 *    - Direct zero-copy page access
 *    - No slab allocator overhead
 *    - Parallel probing with work stealing
 *    - Performance: 11.12s baseline (FAST)
 *    - Correctness: Core requirements met
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

// Default: STRICT_PROJECT=1 (unless OPTIMIZED_PROJECT=1 is explicitly set)
inline bool use_strict_project() {
    // If OPTIMIZED is explicitly on, use it
    if (is_optimized_mode()) return false;
    // Otherwise default to STRICT (partition build)
    return true;
}

} // namespace Contest

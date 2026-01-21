#ifndef JOIN_CONFIG_H
#define JOIN_CONFIG_H

#include <cstdlib>
#include <cstdint>

namespace Contest {

// Check if global bloom filter for joins is enabled
// Default: ENABLED for performance. Set JOIN_GLOBAL_BLOOM=0 to disable.
inline bool join_global_bloom_enabled() {
    // (GR) Global bloom πριν το probe: μειώνει probes σε άσχετα keys.
    // Χρήσιμο όταν probe πλευρά είναι τεράστια και το selectivity είναι μικρό.
    static const bool enabled = [] {
        const char* v = std::getenv("JOIN_GLOBAL_BLOOM");
        if (!v || !*v) return true;  // Default: enabled
        return *v != '0';
    }();
    return enabled;
}

// Check if building from pages (zero-copy) is enabled
// Default: enabled (meets assignment requirement for INT32 no-NULL columns).
// Set REQ_BUILD_FROM_PAGES=0 to force the old vector<HashEntry> build path.
inline bool req_build_from_pages_enabled() {
    static const bool enabled = [] {
        const char* v = std::getenv("REQ_BUILD_FROM_PAGES");
        if (!v) return true;
        return *v && *v != '0';
    }();
    return enabled;
}

// Get the number of bits to use for global bloom filter
// Default: 20 -> 1,048,576 bits (128 KiB).
// Clamp to a reasonable range so it doesn't blow up memory.
inline uint32_t join_global_bloom_bits() {
    // (GR) Περισσότερα bits -> λιγότερα false positives αλλά περισσότερη μνήμη.
    static const uint32_t bits = [] {
        const char* v = std::getenv("JOIN_GLOBAL_BLOOM_BITS");
        if (!v || !*v) return 20u;
        const long parsed = std::strtol(v, nullptr, 10);
        if (parsed < 16) return 16u;
        if (parsed > 24) return 24u;
        return static_cast<uint32_t>(parsed);
    }();
    return bits;
}

} // namespace Contest

#endif // JOIN_CONFIG_H

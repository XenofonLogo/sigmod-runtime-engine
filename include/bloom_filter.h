#pragma once
#include <cstdint>
#include <array>

namespace Bloom {

//
// 1) Precomputed popcount table for all 16-bit values
//
// We compute at compile-time (constexpr) a table of 65,536 entries,
// where table[x] = number of set bits in x (popcount).
//
// This allows computing popcount for a 16-bit bloom filter in O(1)
// without performing any bitwise loops at runtime.
//
// The table is generated once at compile-time, so there is no runtime cost.
//
static constexpr std::array<uint8_t, 65536> make_popcount_table() {
    std::array<uint8_t, 65536> table{};
    for (uint32_t i = 0; i < 65536; i++) {
        uint32_t x = i;
        uint32_t c = 0;

        // Brian Kernighan bit trick: repeatedly clears the lowest set bit
        while (x) {
            x &= (x - 1);
            c++;
        }

        table[i] = static_cast<uint8_t>(c);
    }
    return table;
}

// Precomputed popcount table for 16-bit bloom filters
static constexpr auto POPCOUNT16 = make_popcount_table();


//
// 2) Create Bloom tag for each tuple (4 bits per tuple)
// -----------------------------------------------------
//
// Each tuple maps to 4 bits of the bloom filter. The positions are
// chosen from different areas of the hash to provide good dispersion,
// low collisions and a small false-positive rate.
//
// The selection of positions is fixed and repeatable (not random),
// so hash joins are deterministic.
//
inline uint16_t make_tag_from_hash(uint64_t h) {
    // Extract 4 independent 4-bit blocks from the hash.
    // These determine which positions in the bloom filter to set.
    uint16_t b1 = (h >>  4) & 0xF;
    uint16_t b2 = (h >> 12) & 0xF;
    uint16_t b3 = (h >> 20) & 0xF;
    uint16_t b4 = (h >> 28) & 0xF;

    // Set the corresponding 4 bits in the bloom filter.
    return (uint16_t)(
        (1u << b1) |
        (1u << b2) |
        (1u << b3) |
        (1u << b4)
    );
}


//
// 3) Bloom filter membership check (probabilistic rejection)
// ----------------------------------------------------------
//
// If ANY of the tag bits is missing then:
//
//   -> The element is 100% NOT present in the bucket.
//
// If ALL bits are present:
//
//   -> It may be present (potential match)
//   -> It may be a false positive (rare)
//
// This check is extremely cheap: a single bitwise AND.
//
inline bool maybe_contains(uint16_t bloom, uint16_t tag) {
    return (bloom & tag) == tag;
}


//
// 4) Popcount: how many bits are set
// ----------------------------------
//
// Not directly used by the hashtable, but useful for debugging,
// tuning and unit testing.
//
inline uint8_t popcount(uint16_t bloom) {
    return POPCOUNT16[bloom];
}

} // namespace Bloom

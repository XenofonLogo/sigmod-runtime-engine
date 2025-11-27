#pragma once
#include <cstdint>
#include <array>

namespace Bloom {

//
// 1) Precomputed popcount table for all 16-bit values
//
static constexpr std::array<uint8_t, 65536> make_popcount_table() {
    std::array<uint8_t, 65536> table{};
    for (uint32_t i = 0; i < 65536; i++) {
        uint32_t x = i;
        uint32_t c = 0;
        while (x) {
            x &= (x - 1);
            c++;
        }
        table[i] = static_cast<uint8_t>(c);
    }
    return table;
}

static constexpr auto POPCOUNT16 = make_popcount_table();

//
// 2) Bloom tag policy (4 bits per tuple)
//    Use 4 independent bits derived from hash.
//
inline uint16_t make_tag_from_hash(uint64_t h) {
    // Extract four fairly spread-out bit positions: 
    // (arbitrary but stable choice)
    uint16_t b1 = (h >>  4) & 0xF;
    uint16_t b2 = (h >> 12) & 0xF;
    uint16_t b3 = (h >> 20) & 0xF;
    uint16_t b4 = (h >> 28) & 0xF;

    // Set four bits in the bloom filter:
    return (uint16_t)((1u << b1) |
                      (1u << b2) |
                      (1u << b3) |
                      (1u << b4));
}

//
// 3) Fast check (if ANY required bit is missing → reject)
//
inline bool maybe_contains(uint16_t bloom, uint16_t tag) {
    // If ALL bits in 'tag' are present in bloom
    return (bloom & tag) == tag;
}

//
// 4) Optional: number of bits set in bloom — extremely cheap
//
inline uint8_t popcount(uint16_t bloom) {
    return POPCOUNT16[bloom];
}

} 
// bloom_filter.h
#pragma once
#include <cstdint>

namespace Bloom {

// Simple scheme: από 64-bit hash παίρνουμε 4 θέσεις 0..15 και θέτουμε τα αντίστοιχα bits.
// Επέστρεψε ένα 16-bit mask (μπορεί να έχει >1 bit) - αυτό είναι το tag που προστίθεται στον bucket bloom.
inline uint16_t make_tag_from_hash(uint64_t h) {
    // Παράδειγμα: πάρτε 4 διαφορετικά 4-bit values από το hash και βάλτε τα σαν bits.
    uint16_t tag = 0;
    // θέσεις: 0..15
    uint8_t p0 = static_cast<uint8_t>((h >> 4) & 0xFu);
    uint8_t p1 = static_cast<uint8_t>((h >> 10) & 0xFu);
    uint8_t p2 = static_cast<uint8_t>((h >> 20) & 0xFu);
    uint8_t p3 = static_cast<uint8_t>((h >> 28) & 0xFu);

    tag |= static_cast<uint16_t>(1u << p0);
    tag |= static_cast<uint16_t>(1u << p1);
    tag |= static_cast<uint16_t>(1u << p2);
    tag |= static_cast<uint16_t>(1u << p3);
    return tag;
}

inline void add_to_bloom(uint16_t &bloom, uint16_t tag) {
    bloom |= tag;
}

inline bool maybe_contains(uint16_t bloom, uint16_t tag) {
    // Αν οποιοδήποτε από τα bits του tag λείπει στο bloom -> reject.
    // Έτσι: true μόνο αν (bloom & tag) == tag
    return (bloom & tag) == tag;
}

} // namespace Bloom

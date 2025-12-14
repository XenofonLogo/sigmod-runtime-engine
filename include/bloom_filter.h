#pragma once
#include <cstdint>
#include <array>

namespace Bloom {

//
// 1) Precomputed popcount table for all 16-bit values
// 
//
// Υπολογίζουμε *κατά τη μεταγλώττιση* (constexpr) έναν πίνακα 65.536 θέσεων,
// όπου table[x] = αριθμός των set bits στο x (popcount).
//
// Αυτό μας επιτρέπει να κάνουμε popcount σε έναν 16-bit bloom filter:
//
//      popcount(bloom) → O(1), χωρίς καμία bitwise πράξη.
//
// Ο υπολογισμός γίνεται μία φορά, compile-time, οπότε δεν υπάρχει κόστος run-time.
//
static constexpr std::array<uint8_t, 65536> make_popcount_table() {
    std::array<uint8_t, 65536> table{};
    for (uint32_t i = 0; i < 65536; i++) {
        uint32_t x = i;
        uint32_t c = 0;

        // Brian Kernighan bit trick: μειώνει το x αφαιρώντας κάθε φορά το lowest set bit
        while (x) {
            x &= (x - 1);
            c++;
        }

        table[i] = static_cast<uint8_t>(c);
    }
    return table;
}

// Ο προ-υπολογισμένος πίνακας popcount για 16-bit bloom filters
static constexpr auto POPCOUNT16 = make_popcount_table();


//
// 2) Δημιουργία Bloom tag για κάθε tuple (4 bits per tuple)
// ---------------------------------------------------------
//
// Σε κάθε tuple αντιστοιχούμε *4 bits* του bloom filter.
// Τα bits επιλέγονται από διαφορετικές περιοχές του hash,
// ώστε να πετύχουμε καλή διασπορά, χαμηλές συγκρούσεις
// και χαμηλό false-positive rate.
//
// Η επιλογή των positions είναι σταθερή και επαναλαμβανόμενη
// (όχι τυχαία), ώστε τα hash joins να είναι ντετερμινιστικά.
//
inline uint16_t make_tag_from_hash(uint64_t h) {
    // Παίρνουμε 4 ανεξάρτητα 4-bit blocks από το hash.
    // Αυτά καθορίζουν σε ποιες θέσεις του bloom filter θα μπουν bits.
    uint16_t b1 = (h >>  4) & 0xF;
    uint16_t b2 = (h >> 12) & 0xF;
    uint16_t b3 = (h >> 20) & 0xF;
    uint16_t b4 = (h >> 28) & 0xF;

    // Ενεργοποιούμε τα αντίστοιχα 4 bits στο bloom filter.
    return (uint16_t)(
        (1u << b1) |
        (1u << b2) |
        (1u << b3) |
        (1u << b4)
    );
}


//
// 3) Bloom filter membership check (πιθανοτική απόρριψη)
// ------------------------------------------------------
//
// Αν ΚΑΠΟΙΟ από τα bits του tag *λείπει*, τότε:
//
//   → 100% σίγουρα το στοιχείο ΔΕΝ υπάρχει στο bucket
//
// Αν ΟΛΑ τα bits υπάρχουν:
//
//   → Μπορεί να υπάρχει (potential match)
//   → Μπορεί να είναι false positive (σπάνιο)
//
// Αυτός ο έλεγχος είναι εξαιρετικά φτηνός: 1 bitwise AND.
//
inline bool maybe_contains(uint16_t bloom, uint16_t tag) {
    return (bloom & tag) == tag;
}


//
// 4) Popcount: πόσα bits είναι set
// --------------------------------
//
// Δεν χρησιμοποιείται άμεσα στο hashtable,
// αλλά είναι χρήσιμο για debugging,
// tuning και unit testing.
//
inline uint8_t popcount(uint16_t bloom) {
    return POPCOUNT16[bloom];
}

} // namespace Bloom

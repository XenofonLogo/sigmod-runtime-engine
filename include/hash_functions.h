#pragma once
#include <cstdint>
#include <cstddef>

namespace Hash {

//
// Fibonacci (Knuth) multiplicative hashing
//
// Πρόκειται για μια από τις ταχύτερες και καλύτερα κατανεμημένες
// 32-bit → 64-bit hash functions.
// 
// Χρησιμοποιεί τον "magic constant" του Donald Knuth:
//   11400714819323198485ULL
// ο οποίος είναι 2^64 * (φ - 1),
// όπου φ = χρυσή τομή.
//
// Πλεονεκτήματα:
// - Εξαιρετική κατανομή (bit diffusion)
// - Πάρα πολύ γρήγορη (1 multiplication)
// - Ιδανική για hashtable prefix partitioning
//

struct Fibonacci32 {
    inline uint64_t operator()(int32_t x) const noexcept {
        // Μετατρέπουμε το signed int32 σε uint32 για σταθερή bit-αναπαράσταση.
        uint64_t v = static_cast<uint64_t>(static_cast<uint32_t>(x));

        // Knuth multiplicative hashing
        return v * 11400714819323198485ULL;
    }
};

//
// CRC32 (προαιρετικός hash), π.χ. για benchmarking
//
// Είναι βαρύτερος αλλά έχει άλλες χρήσεις (ελέγχους ακεραιότητας, SIMD CRC).
// Δεν χρησιμοποιείται ως default μέσα στο project.
//
//
uint32_t crc32_u32(uint32_t x);

struct CRC32Hasher {
    inline uint64_t operator()(int32_t x) const noexcept {
        return crc32_u32(static_cast<uint32_t>(x));
    }
};

//
// Επιλογή default hasher
//
// Εδώ ορίζουμε ποια hash function χρησιμοποιείται παντού.
// Μετά από benchmark, επιλέξαμε το Fibonacci32.
//
// Αν χρειαστεί αλλαγή, αρκεί να αλλάξει αυτό το typedef.
//
using Hasher32 = Fibonacci32;

} // namespace Hash

#pragma once
#include <cstdint>
#include <cstddef>

namespace Hash {

//
// Fibonacci (Knuth) multiplicative hashing
//
// One of the fastest and best-distributed 32-bit -> 64-bit hash functions.
//
// Uses Donald Knuth's "magic constant":
//   11400714819323198485ULL
// which is 2^64 * (φ - 1), where φ is the golden ratio.
//
// Advantages:
// - Excellent distribution (bit diffusion)
// - Very fast (single multiplication)
// - Well-suited for hashtable prefix partitioning
//

struct Fibonacci32 {
    inline uint64_t operator()(int32_t x) const noexcept {
        // Convert signed int32 to uint32 for stable bit representation.
        uint64_t v = static_cast<uint64_t>(static_cast<uint32_t>(x));

        // Knuth multiplicative hashing
        return v * 11400714819323198485ULL;
    }
};

//
// CRC32 (optional hash), e.g. for benchmarking
//
// Heavier weight but useful for other purposes (integrity checks, SIMD CRC).
// Not used as the project default.
//
//
uint32_t crc32_u32(uint32_t x);

struct CRC32Hasher {
    inline uint64_t operator()(int32_t x) const noexcept {
        return crc32_u32(static_cast<uint32_t>(x));
    }
};

//
// Default hasher selection
//
// Defines which hash function is used across the project.
// After benchmarking we selected Fibonacci32.
//
// To change the default, update this typedef.
//
using Hasher32 = Fibonacci32;

} // namespace Hash

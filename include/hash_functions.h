#pragma once
#include <cstdint>
#include <cstddef>

namespace Hash {

/// Knuth multiplicative hashing (Fibonacci hashing)
/// h(x) = (uint64_t(x) * 11400714819323198485ULL)

struct Fibonacci32 {
    inline uint64_t operator()(int32_t x) const noexcept {
        uint64_t v = static_cast<uint64_t>(static_cast<uint32_t>(x));
        return v * 11400714819323198485ULL;
    }
};

/// Εναλλακτική CRC32 αν ποτέ χρειαστεί (όχι default)
uint32_t crc32_u32(uint32_t x);

struct CRC32Hasher {
    inline uint64_t operator()(int32_t x) const noexcept {
        return crc32_u32(static_cast<uint32_t>(x));
    }
};

/// Wrapper για όποιο 32-bit hasher θέλουμε
using Hasher32 = Fibonacci32;

} // namespace Hash

#ifndef JOIN_BLOOM_FILTER_H
#define JOIN_BLOOM_FILTER_H

#include <cstdint>
#include <vector>

namespace Contest {

// Global bloom filter for join early rejection
// Uses two hash positions per key for better filtering
struct GlobalBloom {
    uint32_t bits = 0;
    uint64_t mask = 0;
    std::vector<uint64_t> words;

    // Initialize bloom filter with specified bit size
    void init(uint32_t bits_);

    // Fast multiplicative hash for bloom indexing
    static inline uint64_t hash32(uint32_t x) {
        // (GR) Ο bloom δεν χρειάζεται κρυπτογραφική ποιότητα, μόνο γρήγορο mixing.
        return static_cast<uint64_t>(x) * 11400714819323198485ull;
    }

    // Add a key to the bloom filter
    inline void add_i32(int32_t key);

    // Check if key might be in the set (may have false positives)
    inline bool maybe_contains_i32(int32_t key) const;
};

// Inline implementations for performance
inline void GlobalBloom::init(uint32_t bits_) {
    bits = bits_;
    mask = (bits_ == 64) ? ~0ull : ((1ull << bits_) - 1ull);
    const size_t nbits = 1ull << bits_;
    const size_t nwords = (nbits + 63ull) / 64ull;
    words.assign(nwords, 0ull);
}

inline void GlobalBloom::add_i32(int32_t key) {
    const uint64_t h = hash32(static_cast<uint32_t>(key));
    const uint64_t i1 = (h)&mask;
    const uint64_t i2 = (h >> 32)&mask;
    words[i1 >> 6] |= (1ull << (i1 & 63ull));
    words[i2 >> 6] |= (1ull << (i2 & 63ull));
}

inline bool GlobalBloom::maybe_contains_i32(int32_t key) const {
    const uint64_t h = hash32(static_cast<uint32_t>(key));
    const uint64_t i1 = (h)&mask;
    const uint64_t i2 = (h >> 32)&mask;
    const uint64_t w1 = words[i1 >> 6];
    const uint64_t w2 = words[i2 >> 6];
    return (w1 & (1ull << (i1 & 63ull))) && (w2 & (1ull << (i2 & 63ull)));
}

} // namespace Contest

#endif // JOIN_BLOOM_FILTER_H

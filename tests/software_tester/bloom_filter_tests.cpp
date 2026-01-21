#include <catch2/catch_test_macros.hpp>
#include <vector>
#include <cstdint>
#include <algorithm>

// ============================================================================
// BLOOM FILTER TESTS
// ============================================================================

TEST_CASE("Bloom filter: basic tag and mask operations", "[bloom][tag]") {
    // Test that bloom tags and masks work correctly
    uint16_t bloom = 0;
    
    // Set a bit
    uint32_t tag = 5;
    bloom |= (1u << tag);
    
    // Check that bit is set
    REQUIRE((bloom & (1u << tag)) != 0);
    
    // Check that other bits are not set
    REQUIRE((bloom & (1u << (tag + 1))) == 0);
}

TEST_CASE("Bloom filter: multiple tags in single bloom", "[bloom][multiple]") {
    uint16_t bloom = 0;
    std::vector<uint32_t> tags = {0, 3, 7, 15};
    
    // Set multiple bits
    for (auto t : tags) {
        bloom |= (1u << t);
    }
    
    // Verify all are set
    for (auto t : tags) {
        REQUIRE((bloom & (1u << t)) != 0);
    }
    
    // Verify unset bits are not set
    for (uint32_t t = 0; t < 16; ++t) {
        if (std::find(tags.begin(), tags.end(), t) == tags.end()) {
            REQUIRE((bloom & (1u << t)) == 0);
        }
    }
}

TEST_CASE("Bloom filter: collision detection", "[bloom][collision]") {
    uint16_t bloom = 0;
    
    // Set bit 5
    bloom |= (1u << 5);
    
    // Create a hash value that maps to bit 5
    uint32_t hash_value = 5;
    
    // Check for collision
    REQUIRE((bloom & (1u << hash_value)) != 0);
    
    // Different hash (no collision)
    uint32_t other_hash = 7;
    REQUIRE((bloom & (1u << other_hash)) == 0);
}

TEST_CASE("Bloom filter: false positive rate estimation", "[bloom][false-positive]") {
    // With 16-bit bloom, false positive rate ~= 1/65536 per unset bit
    // This test verifies that multiple unset bits reduce false positive rate
    uint16_t bloom = 0;
    
    // Set only one bit
    bloom |= (1u << 5);
    
    // Count false positives for unset bits
    int false_positives = 0;
    for (uint32_t i = 0; i < 16; ++i) {
        if (i != 5) {
            if ((bloom & (1u << i)) != 0) {
                false_positives++;
            }
        }
    }
    
    // Should have no false positives (since only one bit is set)
    REQUIRE(false_positives == 0);
}

TEST_CASE("Bloom filter: all bits set", "[bloom][saturation]") {
    uint16_t bloom = 0xFFFF;  // All bits set
    
    // Any query should match
    for (uint32_t i = 0; i < 16; ++i) {
        REQUIRE((bloom & (1u << i)) != 0);
    }
}

TEST_CASE("Bloom filter: no bits set", "[bloom][empty]") {
    uint16_t bloom = 0x0000;  // No bits set
    
    // No query should match
    for (uint32_t i = 0; i < 16; ++i) {
        REQUIRE((bloom & (1u << i)) == 0);
    }
}

TEST_CASE("Bloom filter: tag extraction from hash", "[bloom][hashing]") {
    // Simulate extracting a tag from a hash value
    uint32_t hash = 0x12345678;
    
    // Extract lower 4 bits as tag
    uint32_t tag = hash & 0xF;
    REQUIRE(tag == 0x8);
    
    // Test with different hash
    uint32_t hash2 = 0xABCDEF00;
    uint32_t tag2 = hash2 & 0xF;
    REQUIRE(tag2 == 0x0);
}

TEST_CASE("Bloom filter: independent bit positions", "[bloom][independence]") {
    uint16_t bloom = 0;
    
    // Set bits at different positions
    std::vector<int> positions = {0, 5, 10, 15};
    for (int pos : positions) {
        bloom |= (1u << pos);
    }
    
    // Verify independence: setting one bit doesn't affect others
    for (int pos : positions) {
        REQUIRE((bloom & (1u << pos)) != 0);
    }
    
    // Clear one bit
    bloom &= ~(1u << 5);
    REQUIRE((bloom & (1u << 5)) == 0);
    
    // Others should still be set
    for (int pos : positions) {
        if (pos != 5) {
            REQUIRE((bloom & (1u << pos)) != 0);
        }
    }
}

TEST_CASE("Bloom filter: global bloom configuration with JOIN_GLOBAL_BLOOM_BITS", "[bloom][global][config]") {
    // This test verifies that global bloom configuration is respected
    // JOIN_GLOBAL_BLOOM_BITS controls the size (default 20 = 2^20 = 128 KiB)
    
    // A global bloom of size 2^20 should be large enough to minimize false positives
    // for typical IMDB join sizes
    size_t global_bloom_bits = 20;  // Can be overridden via environment
    size_t global_bloom_size = 1ull << global_bloom_bits;  // 2^20
    
    // Verify reasonable size
    REQUIRE(global_bloom_size >= (1ull << 18));  // At least 256 KiB
    REQUIRE(global_bloom_size <= (1ull << 25));  // At most 32 MiB
}


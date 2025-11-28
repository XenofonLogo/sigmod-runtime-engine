#include "test_framework.h"
#include "bloom_filter.h"
#include "hash_functions.h"
#include "unchained_hashtable.h"

#include <random>
#include <unordered_map>
#include <cmath>

int main() {

    //
    // ===============================================================
    //  BASIC BLOOM FILTER TESTS
    // ===============================================================
    //
    TEST("Bloom: 4 bits set in tag")
    {
        uint64_t h = 0x123456789ABCDEF0ULL;
        uint16_t tag = Bloom::make_tag_from_hash(h);
        CHECK(Bloom::popcount(tag) == 4);
    }

    TEST("Bloom: maybe_contains positive/negative")
    {
        uint16_t bloom = 0;
        uint64_t h = 0xCAFEBABEULL;
        uint16_t tag = Bloom::make_tag_from_hash(h);

        bloom |= tag;
        CHECK(Bloom::maybe_contains(bloom, tag));
        CHECK(!Bloom::maybe_contains(bloom, tag << 1));
    }


    //
    // ===============================================================
    //  BLOOM: FALSE POSITIVE RATE TEST
    // ===============================================================
    //
    TEST("Bloom: estimated false-positive rate")
    {
        std::mt19937_64 rng(123);
        uint16_t bloom = 0;

        // insert 200 random tags → bloom grows to a realistic load
        for (int i = 0; i < 200; i++)
            bloom |= Bloom::make_tag_from_hash(rng());

        int trials = 200000;
        int false_pos = 0;

        for (int i = 0; i < trials; i++) {
            uint16_t tag = Bloom::make_tag_from_hash(rng());
            if (Bloom::maybe_contains(bloom, tag))
                false_pos++;
        }

        double rate = double(false_pos) / trials;
        std::cout << "  Bloom false-positive rate ≈ " << rate << "\n";

        CHECK(rate < 0.20);  // must be under approx 20%
    }


    //
    // ===============================================================
    //  HASH FUNCTION TESTS
    // ===============================================================
    //
    TEST("Fibonacci hash produces stable results")
    {
        Hash::Fibonacci32 h;
        CHECK(h(12345) == h(12345));
    }

    TEST("Fibonacci hash produces different hashes for different keys")
    {
        Hash::Fibonacci32 h;
        CHECK(h(1) != h(2));
    }

    TEST("Fibonacci hash uniform distribution histogram")
    {
        Hash::Fibonacci32 h;
        constexpr int N = 200000;
        constexpr int B = 1024;
        std::vector<int> buckets(B, 0);

        for (int i = 0; i < N; i++) {
            uint64_t x = h(i);
            buckets[x % B]++;
        }

        double avg = double(N) / B;
        double max_dev = 0;

        for (int b : buckets) {
            double dev = std::abs(b - avg) / avg;
            if (dev > max_dev) max_dev = dev;
        }

        std::cout << "  Max histogram deviation = " << max_dev << "\n";
        CHECK(max_dev < 0.10); // within ±10%
    }


    //
    // ===============================================================
    //  UNCHAINED HASHTABLE BASIC TESTS
    // ===============================================================
    //
    TEST("UnchainedHashTable: basic build + probe")
    {
        UnchainedHashTable<int32_t> ht;

        std::vector<std::pair<int32_t,size_t>> entries{
            {10,0}, {20,1}, {10,2}, {30,3}
        };

        ht.build_from_entries(entries);

        size_t len = 0;
        auto* p = ht.probe(10, len);

        CHECK(p != nullptr);
        CHECK(len == 2);
    }

    TEST("UnchainedHashTable: probe_exact returns correct row_ids")
    {
        UnchainedHashTable<int32_t> ht;

        std::vector<std::pair<int32_t,size_t>> entries{
            {10,0}, {20,1}, {10,2}, {30,3}
        };

        ht.build_from_entries(entries);

        auto result = ht.probe_exact(10);

        CHECK(result.size() == 2);
        CHECK(result[0] == 0);
        CHECK(result[1] == 2);
    }


    
    


    
   

    END_TESTS();
    return tests_failed;
}

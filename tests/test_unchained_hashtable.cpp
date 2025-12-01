//
// ===============================================================
//  Test Suite για Bloom Filter, Hash Functions και UnchainedHashTable
//  Με επαρκή κάλυψη edge cases & stress tests
// ===============================================================
//

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
    //  BLOOM FILTER TESTS
    // ===============================================================
    //

    TEST("Bloom: 4 bits set in tag")
    {
        uint64_t h = 0x123456789ABCDEF0ULL;
        uint16_t tag = Bloom::make_tag_from_hash(h);

        // Ελέγχουμε ότι το tag έχει πάντα 4 bits ενεργά
        CHECK(Bloom::popcount(tag) == 4);
    }

    TEST("Bloom: maybe_contains positive/negative")
    {
        uint16_t bloom = 0;
        uint64_t h = 0xCAFEBABEULL;
        uint16_t tag = Bloom::make_tag_from_hash(h);

        bloom |= tag;
        CHECK(Bloom::maybe_contains(bloom, tag));     // πρέπει να υπάρχει
        CHECK(!Bloom::maybe_contains(bloom, tag << 1)); // ψεύτικο tag → πρέπει να απορριφθεί
    }

    // FPR test πιο κοντά στη θεωρία για bucket size 1–3
    TEST("Bloom: realistic FPR for small buckets (1–3 inserts)")
    {
        std::mt19937_64 rng(123);

        constexpr int INSERTS = 3;   // τυπικό μικρό bucket
        uint16_t bloom = 0;

        for (int i = 0; i < INSERTS; i++)
            bloom |= Bloom::make_tag_from_hash(rng());

        int trials = 200000;
        int false_pos = 0;

        for (int i = 0; i < trials; i++) {
            uint16_t tag = Bloom::make_tag_from_hash(rng());
            if (Bloom::maybe_contains(bloom, tag))
                false_pos++;
        }

        double rate = double(false_pos) / trials;
        std::cout << "  Bloom false-positive rate (3 inserts) ≈ " << rate << "\n";

        CHECK(rate < 0.40); // επιτρεπτό (12 bits set → ~15–35%)
    }

    // ΝΕΟ TEST: Tag collisions σε διαφορετικά keys (στατιστικό, όχι ακριβές)
    TEST("Bloom: independent keys tend not to share identical tag")
    {
        std::mt19937_64 rng(32145);

        int collisions = 0;
        int trials = 50000;

        for (int i = 0; i < trials; i++) {
            uint16_t t1 = Bloom::make_tag_from_hash(rng());
            uint16_t t2 = Bloom::make_tag_from_hash(rng());
            if (t1 == t2) collisions++;
        }

        // Πολύ χαμηλή πιθανότητα για 16-bit tag με μόνο 4 bits set
        CHECK(collisions < 200); 
    }


    //
    // ===============================================================
    //  HASH FUNCTION TESTS
    // ===============================================================
    //

    TEST("Fibonacci hash produces stable results")
    {
        Hash::Fibonacci32 h;
        CHECK(h(12345) == h(12345)); // deterministic
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
        CHECK(max_dev < 0.10); // ±10% = αρκετά ομοιόμορφο
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

    // ΝΕΟ TEST: Empty table probe
    TEST("UnchainedHashTable: probe on empty table")
    {
        UnchainedHashTable<int32_t> ht;
        size_t len = 0;
        auto* p = ht.probe(123, len);

        CHECK(p == nullptr);
        CHECK(len == 0);
    }

    // ΝΕΟ TEST: Key που δεν υπάρχει
    TEST("UnchainedHashTable: probe for non-existing key")
    {
        UnchainedHashTable<int32_t> ht;

        std::vector<std::pair<int32_t,size_t>> entries{
            {1,0}, {2,1}, {3,2}
        };
        ht.build_from_entries(entries);

        size_t len = 0;
        auto* p = ht.probe(999, len);

        CHECK(p == nullptr);
        CHECK(len == 0);
    }


    //
    // ===============================================================
    //  UNCHAINED HASHTABLE STRESS TESTS
    // ===============================================================
    //

    TEST("UnchainedHashTable: random stress test with 50k keys")
    {
        std::mt19937 rng(123);
        UnchainedHashTable<uint32_t> ht;

        static constexpr int N = 50000;
        std::vector<std::pair<uint32_t,size_t>> entries;
        std::unordered_map<uint32_t, std::vector<size_t>> mirror;

        for (int i = 0; i < N; i++) {
            uint32_t k = rng();
            entries.emplace_back(k, i);
            mirror[k].push_back(i);
        }

        ht.build_from_entries(entries);

        for (int t = 0; t < 5000; t++) {
            uint32_t key = entries[t].first;
            auto expected = mirror[key];
            auto result = ht.probe_exact(key);

            CHECK(result.size() == expected.size());
        }
    }

    TEST("UnchainedHashTable: heavy collision test (all keys same prefix)")
    {
        UnchainedHashTable<int32_t> ht;
        const int N = 1000;

        std::vector<std::pair<int32_t,size_t>> entries;
        entries.reserve(N);

        // Όλα τα keys έχουν ίδιο prefix → πέφτουν στην ίδια bucket
        for (int i = 0; i < N; i++)
            entries.emplace_back(12345, i);

        ht.build_from_entries(entries);

        size_t len = 0;
        auto* p = ht.probe(12345, len);

        CHECK(p != nullptr);
        CHECK(len == N);
    }

    TEST("UnchainedHashTable: large scale 100k")
    {
        // Λίγο μεγαλύτερο directory (2^17 buckets)
        UnchainedHashTable<uint32_t> ht(Hash::Fibonacci32(), 17);

        std::vector<std::pair<uint32_t,size_t>> entries;
        entries.reserve(100000);

        for (uint32_t i = 0; i < 100000; i++)
            entries.emplace_back(i, i);

        ht.build_from_entries(entries);

        for (uint32_t i = 0; i < 10000; i++) {
            size_t len = 0;
            auto* p = ht.probe(i, len);

            CHECK(p != nullptr);
            CHECK(len == 1);
            CHECK(p[0].key == i);
            CHECK(p[0].row_id == i);
        }
    }


    //
    // ===============================================================
    //  END OF TESTS
    // ===============================================================
    //
    END_TESTS();
    return tests_failed;
}

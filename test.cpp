#include <iostream>
#include <vector>

#include "unchained_hashtable.h"
#include "hash_functions.h"
#include "bloom_filter.h"

int main() {
    using Key = int32_t;

    std::cout << "=== TEST: Unchained Hash Table ===\n\n";

    // Rows for build_from_rows
    std::vector<std::vector<Key>> left_records = {
        {10},
        {42},
        {10},
        {7},
        {42},
        {99}
    };

    // Create hashtable
    UnchainedHashTable<Key, Hash::Hasher32> ht;

    std::cout << "[1] Reserving...\n";
    ht.reserve(left_records.size());

    std::cout << "[2] Building (build_from_rows)...\n";
    ht.build_from_rows(left_records, 0);  // key_col = 0

    auto probe_and_print = [&](Key key) {
        std::cout << "Probe(" << key << ") -> ";

        std::vector<std::size_t> matches = ht.probe_exact(key);

        if (matches.empty()) {
            std::cout << "NO MATCH\n";
            return;
        }

        std::cout << matches.size() << " match(es): [ ";
        for (auto r : matches) std::cout << r << " ";
        std::cout << "]\n";
    };

    std::cout << "\n=== PROBING ===\n";

    probe_and_print(10);   // expect indexes {0,2}
    probe_and_print(42);   // expect indexes {1,4}
    probe_and_print(7);    // expect {3}
    probe_and_print(99);   // expect {5}
    probe_and_print(123);  // expect empty

    std::cout << "\nDone.\n";
    return 0;
}

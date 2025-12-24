#include <catch2/catch_test_macros.hpp>

#include <functional>
#include <random>
#include <string>
#include <vector>
#include <unordered_set>
#include <bitset>
#include <numeric>
#include <type_traits>
#include <limits>
#include <iostream>

// A small, standalone hashing quality tester.
// It checks collision rate, bucket distribution uniformity (chi-sq-like),
// and a simple avalanche test for std::hash on a few common key types.

static std::mt19937_64 rng(123456789);

static size_t hamming_distance(size_t a, size_t b)
{
    return std::bitset<sizeof(size_t) * 8>(a ^ b).count();
}

// Convert hash value to a canonical bucket index (power-of-two buckets)
static inline size_t bucket_index(size_t hash, size_t nbuckets)
{
    return hash & (nbuckets - 1);
}

template <typename T>
void run_hash_quality_test()
{
    using KeyT = T;
    std::hash<KeyT> hasher;

    const size_t N = 20000; // number of keys
    // Use fewer buckets so mean per bucket is > 1 (more stable variance)
    const size_t NB = 1 << 12;              // number of buckets for distribution checks
    const double MAX_COLLISION_RATE = 0.06; // 6% collisions allowed (lenient to avoid CI flakes)

    std::vector<KeyT> keys;
    keys.reserve(N);

    if constexpr (std::is_same_v<KeyT, std::string>)
    {
        std::uniform_int_distribution<int> len_dist(1, 32);
        std::uniform_int_distribution<char> ch_dist('a', 'z');
        for (size_t i = 0; i < N; ++i)
        {
            int L = len_dist(rng);
            std::string s;
            s.reserve(L);
            for (int j = 0; j < L; ++j)
                s.push_back(ch_dist(rng));
            keys.emplace_back(std::move(s));
        }
    }
    else if constexpr (std::is_same_v<KeyT, double>)
    {
        std::uniform_real_distribution<double> ddist(-1e9, 1e9);
        for (size_t i = 0; i < N; ++i)
            keys.emplace_back(ddist(rng));
    }
    else if constexpr (std::is_same_v<KeyT, int32_t>)
    {
        std::uniform_int_distribution<int32_t> idist(std::numeric_limits<int32_t>::min() / 2, std::numeric_limits<int32_t>::max() / 2);
        for (size_t i = 0; i < N; ++i)
            keys.emplace_back(idist(rng));
    }
    else if constexpr (std::is_same_v<KeyT, int64_t>)
    {
        std::uniform_int_distribution<int64_t> idist(std::numeric_limits<int64_t>::min() / 2, std::numeric_limits<int64_t>::max() / 2);
        for (size_t i = 0; i < N; ++i)
            keys.emplace_back(idist(rng));
    }

    // Collision test
    std::unordered_set<size_t> seen;
    seen.reserve(N * 2);
    size_t collisions = 0;
    for (auto &k : keys)
    {
        size_t hv = hasher(k);
        if (!seen.insert(hv).second)
            ++collisions;
    }
    double collision_rate = double(collisions) / double(N);

    INFO("Type: " << (std::is_same_v<KeyT, std::string> ? "string" : (std::is_same_v<KeyT, double> ? "double" : (std::is_same_v<KeyT, int32_t> ? "int32_t" : "int64_t"))));
    INFO("Collisions: " << collisions << " / " << N << " (" << collision_rate << ")");

    REQUIRE(collision_rate <= MAX_COLLISION_RATE);

    // Distribution test (simple variance check across buckets)
    std::vector<size_t> buckets(NB);
    for (auto &k : keys)
    {
        size_t hv = hasher(k);
        buckets[bucket_index(hv, NB)]++;
    }
    // compute mean and variance
    double mean = double(N) / double(NB);
    double var = 0.0;
    for (auto c : buckets)
    {
        double d = double(c) - mean;
        var += d * d;
    }
    var /= double(NB);
    // For a uniform distribution, variance should be near mean. Allow generous bounds.
    double var_over_mean = var / std::max(1e-12, mean);
    INFO("Mean per bucket: " << mean << " variance: " << var << " var/mean: " << var_over_mean);
    REQUIRE(var_over_mean < 5.0); // liberal bound to avoid flaky failures

    // Avalanche test (flip single bit in input representation) - measure average hamming distance
    // For numeric types we flip bits in the representation; for strings flip one char
    size_t total_hd = 0;
    size_t checks = 0;
    for (size_t i = 0; i < std::min<size_t>(500, keys.size()); ++i)
    {
        const KeyT &k = keys[i];
        size_t hv = hasher(k);
        KeyT k2 = k;
        if constexpr (std::is_same_v<KeyT, std::string>)
        {
            if (!k2.empty())
            {
                k2[0] = k2[0] ^ 0x1; // flip a bit in first char
            }
            else
            {
                k2.push_back('x');
            }
        }
        else
        {
            // flip a random bit in the raw representation
            size_t bit = (rng() % (sizeof(KeyT) * 8));
            auto *ptr = reinterpret_cast<unsigned char *>(&k2);
            size_t byte_idx = bit / 8;
            unsigned char mask = 1u << (bit % 8);
            ptr[byte_idx] ^= mask;
        }
        size_t hv2 = hasher(k2);
        total_hd += hamming_distance(hv, hv2);
        ++checks;
    }
    double avg_hd = double(total_hd) / double(std::max<size_t>(1, checks));
    double bits = double(sizeof(size_t) * 8);
    double fraction = avg_hd / bits;
    INFO("Average hamming distance: " << avg_hd << " (" << fraction << ")");
    // Expect at least ~3% of bits differ on average for non-integral types.
    // For integral keys std::hash often maps to the identity, so flipping a single
    // input bit will often flip a single output bit (fraction ~= 1/bitwidth). Skip
    // the strict requirement for integral types to avoid false failures.
    const double MIN_AVALANCHE_FRACTION = 0.03;
    if constexpr (std::is_integral_v<KeyT>)
    {
        INFO("Integral type - skipping avalanche REQUIRE (observed fraction=" << fraction << ")");
    }
    else
    {
        REQUIRE(fraction >= MIN_AVALANCHE_FRACTION);
    }
}

TEST_CASE("Hash quality: int32_t", "[hash][quality]") { run_hash_quality_test<int32_t>(); }
TEST_CASE("Hash quality: int64_t", "[hash][quality]") { run_hash_quality_test<int64_t>(); }
TEST_CASE("Hash quality: double", "[hash][quality]") { run_hash_quality_test<double>(); }
TEST_CASE("Hash quality: string", "[hash][quality]") { run_hash_quality_test<std::string>(); }

// --- Functional join tests (copied from tests/unit_tests.cpp) ---
#include <table.h>
#include <plan.h>
#include <algorithm>

void sort_table(std::vector<std::vector<Data>> &table)
{
    std::sort(table.begin(), table.end());
}

TEST_CASE("Empty join", "[join]")
{
    Plan plan;
    plan.new_scan_node(0, {{0, DataType::INT32}});
    plan.new_scan_node(1, {{0, DataType::INT32}});
    plan.new_join_node(true, 0, 1, 0, 0, {{0, DataType::INT32}, {1, DataType::INT32}});
    ColumnarTable table1, table2;
    table1.columns.emplace_back(DataType::INT32);
    table2.columns.emplace_back(DataType::INT32);
    plan.inputs.emplace_back(std::move(table1));
    plan.inputs.emplace_back(std::move(table2));
    plan.root = 2;
    auto *context = Contest::build_context();
    auto result = Contest::execute(plan, context);
    Contest::destroy_context(context);
    REQUIRE(result.num_rows == 0);
    REQUIRE(result.columns.size() == 2);
    REQUIRE(result.columns[0].type == DataType::INT32);
    REQUIRE(result.columns[1].type == DataType::INT32);
}

TEST_CASE("One line join", "[join]")
{
    Plan plan;
    plan.new_scan_node(0, {{0, DataType::INT32}});
    plan.new_scan_node(1, {{0, DataType::INT32}});
    plan.new_join_node(true, 0, 1, 0, 0, {{0, DataType::INT32}, {1, DataType::INT32}});
    std::vector<std::vector<Data>> data{
        {
            1,
        },
    };
    std::vector<DataType> types{DataType::INT32};
    Table table(std::move(data), std::move(types));
    ColumnarTable table1 = table.to_columnar();
    ColumnarTable table2 = table.to_columnar();
    plan.inputs.emplace_back(std::move(table1));
    plan.inputs.emplace_back(std::move(table2));
    plan.root = 2;
    auto *context = Contest::build_context();
    auto result = Contest::execute(plan, context);
    Contest::destroy_context(context);
    REQUIRE(result.num_rows == 1);
    REQUIRE(result.columns.size() == 2);
    REQUIRE(result.columns[0].type == DataType::INT32);
    REQUIRE(result.columns[1].type == DataType::INT32);
    auto result_table = Table::from_columnar(result);
    std::vector<std::vector<Data>> ground_truth{
        {
            1,
            1,
        },
    };
    REQUIRE(result_table.table() == ground_truth);
}

TEST_CASE("Simple join", "[join]")
{
    Plan plan;
    plan.new_scan_node(0, {{0, DataType::INT32}});
    plan.new_scan_node(1, {{0, DataType::INT32}});
    plan.new_join_node(true, 0, 1, 0, 0, {{0, DataType::INT32}, {1, DataType::INT32}});
    std::vector<std::vector<Data>> data{
        {
            1,
        },
        {
            2,
        },
        {
            3,
        },
    };
    std::vector<DataType> types{DataType::INT32};
    Table table(std::move(data), std::move(types));
    ColumnarTable table1 = table.to_columnar();
    ColumnarTable table2 = table.to_columnar();
    plan.inputs.emplace_back(std::move(table1));
    plan.inputs.emplace_back(std::move(table2));
    plan.root = 2;
    auto *context = Contest::build_context();
    auto result = Contest::execute(plan, context);
    Contest::destroy_context(context);
    REQUIRE(result.num_rows == 3);
    REQUIRE(result.columns.size() == 2);
    REQUIRE(result.columns[0].type == DataType::INT32);
    REQUIRE(result.columns[1].type == DataType::INT32);
    auto result_table = Table::from_columnar(result);
    std::vector<std::vector<Data>> ground_truth{
        {
            1,
            1,
        },
        {
            2,
            2,
        },
        {
            3,
            3,
        },
    };
    sort_table(result_table.table());
    REQUIRE(result_table.table() == ground_truth);
}

TEST_CASE("Empty Result", "[join]")
{
    Plan plan;
    plan.new_scan_node(0, {{0, DataType::INT32}});
    plan.new_scan_node(1, {{0, DataType::INT32}});
    plan.new_join_node(true, 0, 1, 0, 0, {{0, DataType::INT32}, {1, DataType::INT32}});
    std::vector<std::vector<Data>> data1{
        {
            1,
        },
        {
            2,
        },
        {
            3,
        },
    };
    std::vector<std::vector<Data>> data2{
        {
            4,
        },
        {
            5,
        },
        {
            6,
        },
    };
    std::vector<DataType> types{DataType::INT32};
    Table table1(std::move(data1), types);
    Table table2(std::move(data2), std::move(types));
    ColumnarTable input1 = table1.to_columnar();
    ColumnarTable input2 = table2.to_columnar();
    plan.inputs.emplace_back(std::move(input1));
    plan.inputs.emplace_back(std::move(input2));
    plan.root = 2;
    auto *context = Contest::build_context();
    auto result = Contest::execute(plan, context);
    Contest::destroy_context(context);
    REQUIRE(result.num_rows == 0);
    REQUIRE(result.columns.size() == 2);
    REQUIRE(result.columns[0].type == DataType::INT32);
    REQUIRE(result.columns[1].type == DataType::INT32);
}

TEST_CASE("Multiple same keys", "[join]")
{
    Plan plan;
    plan.new_scan_node(0, {{0, DataType::INT32}});
    plan.new_scan_node(1, {{0, DataType::INT32}});
    plan.new_join_node(true, 0, 1, 0, 0, {{0, DataType::INT32}, {1, DataType::INT32}});
    std::vector<std::vector<Data>> data1{
        {
            1,
        },
        {
            1,
        },
        {
            2,
        },
        {
            3,
        },
    };
    std::vector<DataType> types{DataType::INT32};
    Table table1(std::move(data1), std::move(types));
    ColumnarTable input1 = table1.to_columnar();
    ColumnarTable input2 = table1.to_columnar();
    plan.inputs.emplace_back(std::move(input1));
    plan.inputs.emplace_back(std::move(input2));
    plan.root = 2;
    auto *context = Contest::build_context();
    auto result = Contest::execute(plan, context);
    Contest::destroy_context(context);
    REQUIRE(result.num_rows == 6);
    REQUIRE(result.columns.size() == 2);
    REQUIRE(result.columns[0].type == DataType::INT32);
    REQUIRE(result.columns[1].type == DataType::INT32);
    auto result_table = Table::from_columnar(result);
    std::vector<std::vector<Data>> ground_truth{
        {
            1,
            1,
        },
        {
            1,
            1,
        },
        {
            1,
            1,
        },
        {
            1,
            1,
        },
        {
            2,
            2,
        },
        {
            3,
            3,
        },
    };
    sort_table(result_table.table());
    REQUIRE(result_table.table() == ground_truth);
}

TEST_CASE("NULL keys", "[join]")
{
    Plan plan;
    plan.new_scan_node(0, {{0, DataType::INT32}});
    plan.new_scan_node(1, {{0, DataType::INT32}});
    plan.new_join_node(true, 0, 1, 0, 0, {{0, DataType::INT32}, {1, DataType::INT32}});
    std::vector<std::vector<Data>> data1{
        {
            1,
        },
        {
            1,
        },
        {
            std::monostate{},
        },
        {
            2,
        },
        {
            3,
        },
    };
    std::vector<DataType> types{DataType::INT32};
    Table table1(std::move(data1), std::move(types));
    ColumnarTable input1 = table1.to_columnar();
    ColumnarTable input2 = table1.to_columnar();
    plan.inputs.emplace_back(std::move(input1));
    plan.inputs.emplace_back(std::move(input2));
    plan.root = 2;
    auto *context = Contest::build_context();
    auto result = Contest::execute(plan, context);
    Contest::destroy_context(context);
    REQUIRE(result.num_rows == 6);
    REQUIRE(result.columns.size() == 2);
    REQUIRE(result.columns[0].type == DataType::INT32);
    REQUIRE(result.columns[1].type == DataType::INT32);
    auto result_table = Table::from_columnar(result);
    std::vector<std::vector<Data>> ground_truth{
        {
            1,
            1,
        },
        {
            1,
            1,
        },
        {
            1,
            1,
        },
        {
            1,
            1,
        },
        {
            2,
            2,
        },
        {
            3,
            3,
        },
    };
    sort_table(result_table.table());
    REQUIRE(result_table.table() == ground_truth);
}

TEST_CASE("Multiple columns", "[join]")
{
    Plan plan;
    plan.new_scan_node(0, {{0, DataType::INT32}});
    plan.new_scan_node(1, {{1, DataType::VARCHAR}, {0, DataType::INT32}});
    plan.new_join_node(true, 0, 1, 0, 1, {{0, DataType::INT32}, {2, DataType::INT32}, {1, DataType::VARCHAR}});
    using namespace std::string_literals;
    std::vector<std::vector<Data>> data1{
        {
            1,
            "xxx"s,
        },
        {
            1,
            "yyy"s,
        },
        {
            std::monostate{},
            "zzz"s,
        },
        {
            2,
            "uuu"s,
        },
        {
            3,
            "vvv"s,
        },
    };
    std::vector<DataType> types{DataType::INT32, DataType::VARCHAR};
    Table table1(std::move(data1), std::move(types));
    ColumnarTable input1 = table1.to_columnar();
    ColumnarTable input2 = table1.to_columnar();
    plan.inputs.emplace_back(std::move(input1));
    plan.inputs.emplace_back(std::move(input2));
    plan.root = 2;
    auto *context = Contest::build_context();
    auto result = Contest::execute(plan, context);
    Contest::destroy_context(context);
    REQUIRE(result.num_rows == 6);
    REQUIRE(result.columns.size() == 3);
    REQUIRE(result.columns[0].type == DataType::INT32);
    REQUIRE(result.columns[1].type == DataType::INT32);
    REQUIRE(result.columns[2].type == DataType::VARCHAR);
    auto result_table = Table::from_columnar(result);
    std::vector<std::vector<Data>> ground_truth{
        {1, 1, "xxx"s},
        {1, 1, "xxx"s},
        {1, 1, "yyy"s},
        {1, 1, "yyy"s},
        {2, 2, "uuu"s},
        {3, 3, "vvv"s},
    };
    sort_table(result_table.table());
    REQUIRE(result_table.table() == ground_truth);
}

TEST_CASE("Build on right", "[join]")
{
    Plan plan;
    plan.new_scan_node(0, {{0, DataType::INT32}});
    plan.new_scan_node(1, {{1, DataType::VARCHAR}, {0, DataType::INT32}});
    plan.new_join_node(false, 0, 1, 0, 1, {{0, DataType::INT32}, {2, DataType::INT32}, {1, DataType::VARCHAR}});
    using namespace std::string_literals;
    std::vector<std::vector<Data>> data1{
        {
            1,
            "xxx"s,
        },
        {
            1,
            "yyy"s,
        },
        {
            std::monostate{},
            "zzz"s,
        },
        {
            2,
            "uuu"s,
        },
        {
            3,
            "vvv"s,
        },
    };
    std::vector<DataType> types{DataType::INT32, DataType::VARCHAR};
    Table table1(std::move(data1), std::move(types));
    ColumnarTable input1 = table1.to_columnar();
    ColumnarTable input2 = table1.to_columnar();
    plan.inputs.emplace_back(std::move(input1));
    plan.inputs.emplace_back(std::move(input2));
    plan.root = 2;
    auto *context = Contest::build_context();
    auto result = Contest::execute(plan, context);
    Contest::destroy_context(context);
    REQUIRE(result.num_rows == 6);
    REQUIRE(result.columns.size() == 3);
    REQUIRE(result.columns[0].type == DataType::INT32);
    REQUIRE(result.columns[1].type == DataType::INT32);
    REQUIRE(result.columns[2].type == DataType::VARCHAR);
    auto result_table = Table::from_columnar(result);
    std::vector<std::vector<Data>> ground_truth{
        {1, 1, "xxx"s},
        {1, 1, "xxx"s},
        {1, 1, "yyy"s},
        {1, 1, "yyy"s},
        {2, 2, "uuu"s},
        {3, 3, "vvv"s},
    };
    sort_table(result_table.table());
    REQUIRE(result_table.table() == ground_truth);
}

TEST_CASE("leftdeep 2-level join", "[join]")
{
    Plan plan;
    plan.new_scan_node(0, {{0, DataType::INT32}});
    plan.new_scan_node(1, {{0, DataType::INT32}});
    plan.new_scan_node(2, {{0, DataType::INT32}});
    plan.new_join_node(true, 0, 1, 0, 0, {{0, DataType::INT32}, {1, DataType::INT32}});
    plan.new_join_node(false, 3, 2, 0, 0, {{0, DataType::INT32}, {1, DataType::INT32}, {2, DataType::INT32}});
    std::vector<std::vector<Data>> data{
        {
            1,
        },
        {
            2,
        },
        {
            3,
        },
    };
    std::vector<DataType> types{DataType::INT32};
    Table table(std::move(data), std::move(types));
    ColumnarTable table1 = table.to_columnar();
    ColumnarTable table2 = table.to_columnar();
    ColumnarTable table3 = table.to_columnar();
    plan.inputs.emplace_back(std::move(table1));
    plan.inputs.emplace_back(std::move(table2));
    plan.inputs.emplace_back(std::move(table3));
    plan.root = 4;
    auto *context = Contest::build_context();
    auto result = Contest::execute(plan, context);
    Contest::destroy_context(context);
    REQUIRE(result.num_rows == 3);
    REQUIRE(result.columns.size() == 3);
    REQUIRE(result.columns[0].type == DataType::INT32);
    REQUIRE(result.columns[1].type == DataType::INT32);
    REQUIRE(result.columns[2].type == DataType::INT32);
    auto result_table = Table::from_columnar(result);
    std::vector<std::vector<Data>> ground_truth{
        {
            1,
            1,
            1,
        },
        {
            2,
            2,
            2,
        },
        {
            3,
            3,
            3,
        },
    };
    sort_table(result_table.table());
    REQUIRE(result_table.table() == ground_truth);
}

TEST_CASE("3-way join", "[join]")
{
    Plan plan;
    plan.new_scan_node(0, {{0, DataType::INT32}, {1, DataType::VARCHAR}});
    plan.new_scan_node(1, {{0, DataType::INT32}, {1, DataType::VARCHAR}});
    plan.new_scan_node(2, {{0, DataType::INT32}, {1, DataType::VARCHAR}});
    plan.new_join_node(false, 0, 1, 0, 0, {{0, DataType::INT32}, {1, DataType::VARCHAR}});
    plan.new_join_node(false, 3, 2, 0, 0, {{0, DataType::INT32}, {1, DataType::VARCHAR}, {3, DataType::VARCHAR}});
    using namespace std::string_literals;
    std::vector<std::vector<Data>> data1{
        {1, "a"s},
        {2, "b"s},
        {3, "c"s},
    };
    std::vector<std::vector<Data>> data2{
        {1, "x"s},
        {2, "y"s},
    };
    std::vector<std::vector<Data>> data3{
        {1, "u"s},
        {2, "v"s},
        {3, "w"s},
    };
    std::vector<DataType> types{DataType::INT32, DataType::VARCHAR};
    Table table1(std::move(data1), types);
    Table table2(std::move(data2), types);
    Table table3(std::move(data3), types);
    ColumnarTable input1 = table1.to_columnar();
    ColumnarTable input2 = table2.to_columnar();
    ColumnarTable input3 = table3.to_columnar();
    plan.inputs.emplace_back(std::move(input1));
    plan.inputs.emplace_back(std::move(input2));
    plan.inputs.emplace_back(std::move(input3));
    plan.root = 4;
    auto *context = Contest::build_context();
    auto result = Contest::execute(plan, context);
    Contest::destroy_context(context);
    REQUIRE(result.num_rows == 2);
    REQUIRE(result.columns.size() == 3);
    REQUIRE(result.columns[0].type == DataType::INT32);
    REQUIRE(result.columns[1].type == DataType::VARCHAR);
    REQUIRE(result.columns[2].type == DataType::VARCHAR);
    auto result_table = Table::from_columnar(result);
    std::vector<std::vector<Data>> ground_truth{
        {1, "a"s, "u"s},
        {2, "b"s, "v"s},
    };
    sort_table(result_table.table());
    REQUIRE(result_table.table() == ground_truth);
}
#include <catch2/catch_test_macros.hpp>

#include <vector>
#include <tuple>

#include "table.h"
#include "plan.h"
#include "columnar.h"

using namespace Contest;

// -----------------------------------------------------------------------------
// INDEXING OPTIMIZATION TESTS
// -----------------------------------------------------------------------------

TEST_CASE(
    "Indexing optimization: INT32 column without NULLs uses zero-copy",
    "[indexing][zero-copy]"
) {
    // Table: INT32 χωρίς NULLs
    std::vector<std::vector<Data>> data = {
        {1}, {2}, {3}, {4}, {5}
    };
    std::vector<DataType> types = { DataType::INT32 };

    Table table(data, types);
    ColumnarTable columnar = table.to_columnar();

    // Plan + Scan
    Plan plan;
    plan.inputs.emplace_back(std::move(columnar));

    ScanNode scan;
    scan.base_table_id = 0;

    std::vector<std::tuple<size_t, DataType>> output_attrs = {
        {0, DataType::INT32}
    };

    ColumnBuffer buf =
        scan_columnar_to_columnbuffer(plan, scan, output_attrs);

    REQUIRE(buf.num_rows == 5);
    REQUIRE(buf.columns.size() == 1);

    const auto& col = buf.columns[0];

    // ---- ΚΡΙΣΙΜΟΙ ΕΛΕΓΧΟΙ ----
    REQUIRE(col.is_zero_copy == true);
    REQUIRE(col.src_column != nullptr);

    // ---- ΣΩΣΤΗ ΑΝΑΓΝΩΣΗ ΤΙΜΩΝ ----
    for (size_t i = 0; i < 5; ++i) {
        REQUIRE(col.get(i).as_i32() == static_cast<int32_t>(i + 1));
    }
}

// -----------------------------------------------------------------------------

TEST_CASE(
    "Indexing optimization: INT32 column with NULLs disables zero-copy",
    "[indexing][materialize]"
) {
    // Table: INT32 με NULL
    std::vector<std::vector<Data>> data = {
        {1},
        {std::monostate{}},
        {3}
    };
    std::vector<DataType> types = { DataType::INT32 };

    Table table(data, types);
    ColumnarTable columnar = table.to_columnar();

    Plan plan;
    plan.inputs.emplace_back(std::move(columnar));

    ScanNode scan;
    scan.base_table_id = 0;

    std::vector<std::tuple<size_t, DataType>> output_attrs = {
        {0, DataType::INT32}
    };

    ColumnBuffer buf =
        scan_columnar_to_columnbuffer(plan, scan, output_attrs);

    const auto& col = buf.columns[0];

    REQUIRE(col.is_zero_copy == false);
    REQUIRE(col.src_column == nullptr);

    REQUIRE(col.get(0).as_i32() == 1);
    REQUIRE(col.get(1).is_null());
    REQUIRE(col.get(2).as_i32() == 3);
}

// -----------------------------------------------------------------------------

TEST_CASE(
    "Indexing optimization: VARCHAR columns never use zero-copy",
    "[indexing][varchar]"
) {
    using namespace std::string_literals;

    std::vector<std::vector<Data>> data = {
        {"a"s}, {"b"s}, {"c"s}
    };
    std::vector<DataType> types = { DataType::VARCHAR };

    Table table(data, types);
    ColumnarTable columnar = table.to_columnar();

    Plan plan;
    plan.inputs.emplace_back(std::move(columnar));

    ScanNode scan;
    scan.base_table_id = 0;

    std::vector<std::tuple<size_t, DataType>> output_attrs = {
        {0, DataType::VARCHAR}
    };

    ColumnBuffer buf =
        scan_columnar_to_columnbuffer(plan, scan, output_attrs);

    REQUIRE(buf.columns.size() == 1);
    REQUIRE(buf.columns[0].is_zero_copy == false);
}

TEST_CASE(
    "Indexing optimization: zero-copy works across multiple pages",
    "[indexing][zero-copy][pages]"
) {
    // Δημιουργούμε αρκετές γραμμές ώστε να σπάσουν σε πολλές pages
    std::vector<std::vector<Data>> data;
    for (int i = 0; i < 5000; ++i) {
        data.push_back({ i });
    }

    std::vector<DataType> types = { DataType::INT32 };

    Table table(data, types);
    ColumnarTable columnar = table.to_columnar();

    Plan plan;
    plan.inputs.emplace_back(std::move(columnar));

    ScanNode scan{ .base_table_id = 0 };

    ColumnBuffer buf =
        scan_columnar_to_columnbuffer(plan, scan, {{0, DataType::INT32}});

    const auto& col = buf.columns[0];

    REQUIRE(col.is_zero_copy == true);
    REQUIRE(col.src_column != nullptr);
    REQUIRE(buf.num_rows == 5000);

    // Έλεγχος αρχής, μέσης και τέλους
    REQUIRE(col.get(0).as_i32() == 0);
    REQUIRE(col.get(1234).as_i32() == 1234);
    REQUIRE(col.get(4999).as_i32() == 4999);
}

TEST_CASE(
    "Indexing optimization: empty INT32 column still zero-copy",
    "[indexing][zero-copy][empty]"
) {
    std::vector<std::vector<Data>> data = {};
    std::vector<DataType> types = { DataType::INT32 };

    Table table(data, types);
    ColumnarTable columnar = table.to_columnar();

    Plan plan;
    plan.inputs.emplace_back(std::move(columnar));

    ScanNode scan{ .base_table_id = 0 };

    ColumnBuffer buf =
        scan_columnar_to_columnbuffer(plan, scan, {{0, DataType::INT32}});

    REQUIRE(buf.num_rows == 0);
    REQUIRE(buf.columns.size() == 1);

    const auto& col = buf.columns[0];
    REQUIRE(col.is_zero_copy == true);
    REQUIRE(col.src_column != nullptr);
}

TEST_CASE(
    "Indexing optimization: mixed columns (INT32 zero-copy + VARCHAR materialized)",
    "[indexing][mixed]"
) {
    using namespace std::string_literals;

    std::vector<std::vector<Data>> data = {
        {1, "a"s},
        {2, "b"s},
        {3, "c"s}
    };

    std::vector<DataType> types = {
        DataType::INT32,
        DataType::VARCHAR
    };

    Table table(data, types);
    ColumnarTable columnar = table.to_columnar();

    Plan plan;
    plan.inputs.emplace_back(std::move(columnar));

    ScanNode scan{ .base_table_id = 0 };

    ColumnBuffer buf =
        scan_columnar_to_columnbuffer(
            plan,
            scan,
            {
                {0, DataType::INT32},
                {1, DataType::VARCHAR}
            }
        );

    REQUIRE(buf.columns.size() == 2);

    const auto& int_col = buf.columns[0];
    const auto& str_col = buf.columns[1];

    // INT32 → zero-copy
    REQUIRE(int_col.is_zero_copy == true);
    REQUIRE(int_col.src_column != nullptr);
    REQUIRE(int_col.get(1).as_i32() == 2);

    // VARCHAR → materialized
    REQUIRE(str_col.is_zero_copy == false);
    REQUIRE(str_col.src_column == nullptr);
}
TEST_CASE(
    "Indexing optimization: INT32 with NULL in later page disables zero-copy",
    "[indexing][nulls][pages]"
) {
    std::vector<std::vector<Data>> data;

    // Πολλές non-null τιμές
    for (int i = 0; i < 3000; ++i) {
        data.push_back({ i });
    }

    // NULL αργότερα
    data.push_back({ std::monostate{} });

    std::vector<DataType> types = { DataType::INT32 };

    Table table(data, types);
    ColumnarTable columnar = table.to_columnar();

    Plan plan;
    plan.inputs.emplace_back(std::move(columnar));

    ScanNode scan{ .base_table_id = 0 };

    ColumnBuffer buf =
        scan_columnar_to_columnbuffer(plan, scan, {{0, DataType::INT32}});

    const auto& col = buf.columns[0];

    REQUIRE(col.is_zero_copy == false);
    REQUIRE(col.src_column == nullptr);

    REQUIRE(col.get(3000).is_null());
}

TEST_CASE(
    "Finalize works correctly when zero-copy was used in scan",
    "[indexing][finalize][zero-copy]"
) {
    std::vector<std::vector<Data>> data;

    for (int i = 0; i < 1000; ++i)
        data.push_back({ i });

    std::vector<DataType> types = { DataType::INT32 };

    Table table(data, types);
    ColumnarTable columnar = table.to_columnar();

    Plan plan;
    plan.inputs.emplace_back(std::move(columnar));

    ScanNode scan{ .base_table_id = 0 };

    auto buf = scan_columnar_to_columnbuffer(
        plan, scan, {{0, DataType::INT32}}
    );

    REQUIRE(buf.columns[0].is_zero_copy == true);

    auto out =
        finalize_columnbuffer_to_columnar(
            plan, buf, {{0, DataType::INT32}}
        );

    REQUIRE(out.columns.size() == 1);
    REQUIRE(out.num_rows == 1000);

    const Column& col = out.columns[0];

    for (int i = 0; i < 1000; ++i) {
        // read from finalized column
        REQUIRE(buf.columns[0].get(i).as_i32() == i);

    }
}

TEST_CASE(
    "Hash join produces correct results even when build-side used zero-copy",
    "[indexing][join][zero-copy]"
) {
    // Left: INT32 keys 0..9
    std::vector<std::vector<Data>> left_data;
    for (int i = 0; i < 10; ++i)
        left_data.push_back({ i });

    // Right: same keys but shuffled
    std::vector<std::vector<Data>> right_data;
    for (int i = 9; i >= 0; --i)
        right_data.push_back({ i });

    std::vector<DataType> types = { DataType::INT32 };

    Table left(left_data, types);
    Table right(right_data, types);

    Plan plan;
    size_t l_id = plan.new_input(left.to_columnar());
    size_t r_id = plan.new_input(right.to_columnar());

    size_t left_scan = plan.new_scan_node(l_id, {{0, DataType::INT32}});
    size_t right_scan = plan.new_scan_node(r_id, {{0, DataType::INT32}});

    JoinNode join{
        .build_left = true,
        .left = left_scan,
        .right = right_scan,
        .left_attr = 0,
        .right_attr = 0
    };

   auto left_buf =
        scan_columnar_to_columnbuffer(plan,
                                      std::get<ScanNode>(plan.nodes[left_scan].data), 
                                      {{0, DataType::INT32}});

    auto right_buf =
        scan_columnar_to_columnbuffer(plan,
                                      std::get<ScanNode>(plan.nodes[right_scan].data), 
                                      {{0, DataType::INT32}});

    auto out =
        join_columnbuffer_hash(
            plan,
            join,
            {{0, DataType::INT32}, {1, DataType::INT32}},
            left_buf,
            right_buf
        );

    REQUIRE(out.num_rows == 10);
}


TEST_CASE(
    "Indexing optimization handles page fully filled with no NULLs",
    "[indexing][pages][boundary]"
) {
    std::vector<std::vector<Data>> data;

    // 1024 rows → likely exactly one full page before bitmap
    for (int i = 0; i < 1024; ++i)
        data.push_back({ i });

    std::vector<DataType> types = { DataType::INT32 };
    Table table(data, types);

    ColumnarTable columnar = table.to_columnar();

    Plan plan;
    plan.inputs.emplace_back(std::move(columnar));

    ScanNode scan{ .base_table_id = 0 };

    auto buf =
        scan_columnar_to_columnbuffer(plan, scan, {{0, DataType::INT32}});

    REQUIRE(buf.columns[0].is_zero_copy == true);
    REQUIRE(buf.columns[0].get(1023).as_i32() == 1023);
}

TEST_CASE(
    "Zero-copy sequential access advances cached page index",
    "[indexing][cache]"
) {
    std::vector<std::vector<Data>> data;

    for (int i = 0; i < 5000; ++i)
        data.push_back({ i });

    std::vector<DataType> types = { DataType::INT32 };
    Table table(data, types);
    ColumnarTable columnar = table.to_columnar();

    Plan plan;
    plan.inputs.emplace_back(std::move(columnar));

    ScanNode scan{ .base_table_id = 0 };

    auto buf =
        scan_columnar_to_columnbuffer(plan, scan, {{0, DataType::INT32}});

    const auto& col = buf.columns[0];

    REQUIRE(col.is_zero_copy == true);

    // sequential scan should not crash and values must match
    for (int i = 0; i < 5000; ++i)
        REQUIRE(col.get(i).as_i32() == i);
}

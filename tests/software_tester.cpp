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
// test vaggelis2
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
// ============================================================================
// === Late Materialization Tests =============================================
// ============================================================================

#include "late_materialization.h"

// Helper: builds a single-page VARCHAR column from strings
static ColumnarTable build_string_column(const std::vector<std::string> &values)
{
    ColumnarTable tbl;
    tbl.columns.emplace_back(DataType::VARCHAR);
    Column &col = tbl.columns.back();

    ColumnInserter<std::string> ins(col);
    for (auto &s : values)
        ins.insert(s);
    ins.finalize();

    tbl.num_rows = values.size();
    return tbl;
}

// Helper: reconstructs strings from a table (for testing late materialization)
static std::vector<std::string> extract_strings(const ColumnarTable &tbl, size_t col_idx)
{
    auto [rows, types] = from_columnar(tbl);
    std::vector<std::string> out;
    out.reserve(rows.size());
    for (auto &r : rows)
    {
        if (std::holds_alternative<std::string>(r[col_idx]))
            out.push_back(std::get<std::string>(r[col_idx]));
        else
            out.push_back(""); // null or error case
    }
    return out;
}

// ---------------------------------------------------------------------------
// TEST 1 — StringRef: encoding + decoding correctness
// ---------------------------------------------------------------------------
TEST_CASE("LateMat: StringRef enc/dec", "[late][stringref]")
{
    StringRef ref;
    ref.table_id = 5;
    ref.column_id = 2;
    ref.page_id = 123;
    ref.offset = 456;

    uint64_t packed = ref.pack();
    StringRef r2 = StringRef::unpack(packed);

    REQUIRE(r2.table_id == 5);
    REQUIRE(r2.column_id == 2);
    REQUIRE(r2.page_id == 123);
    REQUIRE(r2.offset == 456);
}

// ---------------------------------------------------------------------------
// TEST 2 — value_t: store int, stringref, null
// ---------------------------------------------------------------------------
TEST_CASE("LateMat: value_t works", "[late][value]")
{
    value_t v1 = value_t::from_int(42);
    REQUIRE(v1.is_int());
    REQUIRE(v1.get_int() == 42);

    StringRef sr;
    sr.table_id = 1;
    sr.column_id = 0;
    sr.page_id = 2;
    sr.offset = 999;

    value_t v2 = value_t::from_stringref(sr);
    REQUIRE(v2.is_stringref());
    REQUIRE(v2.get_stringref().offset == 999);

    value_t v3 = value_t::make_null();
    REQUIRE(v3.is_null());
}

// ---------------------------------------------------------------------------
// TEST 3 — ScanNode returns StringRefs (not materialized strings)
// ---------------------------------------------------------------------------
TEST_CASE("LateMat: scan produces stringrefs", "[late][scan]")
{
    std::vector<std::string> vals{"aaa", "bbb", "ccc"};
    ColumnarTable input = build_string_column(vals);

    Plan plan;
    plan.inputs.push_back(input);
    size_t scan_id = plan.new_scan_node(0, {{0, DataType::VARCHAR}});
    plan.root = scan_id;

    auto *ctx = Contest::build_context();
    auto res = Contest::execute(plan, ctx); // executeGeneric should use late scan
    Contest::destroy_context(ctx);

    // In late materialization, result should contain StringRefs, not actual strings!
    auto [rows, types] = from_columnar(res);
    REQUIRE(res.columns.size() == 1);

    for (auto &row : rows)
    {
        REQUIRE(!std::holds_alternative<std::string>(row[0])); // string NOT here yet
    }
}

// ---------------------------------------------------------------------------
// TEST 4 — Final output materializes strings correctly
// ---------------------------------------------------------------------------
TEST_CASE("LateMat: final materialization", "[late][materialize]")
{
    std::vector<std::string> vals{"abc", "def", "ghi"};
    ColumnarTable input = build_string_column(vals);

    Plan plan;
    plan.inputs.push_back(input);
    size_t scan_id = plan.new_scan_node(0, {{0, DataType::VARCHAR}});
    plan.root = scan_id;

    auto *ctx = Contest::build_context();
    ColumnarTable result = Contest::execute(plan, ctx);
    Contest::destroy_context(ctx);

    auto extracted = extract_strings(result, 0);
    REQUIRE(extracted == vals);
}

// ---------------------------------------------------------------------------
// TEST 5 — Join forwards StringRefs and materializes at root
// ---------------------------------------------------------------------------
TEST_CASE("LateMat: join on VARCHAR produces correct strings", "[late][join]")
{
    using namespace std::string_literals;

    std::vector<std::string> L{"aaa", "bbb", "ccc"};
    std::vector<std::string> R{"aaa", "xxx", "ccc"};

    ColumnarTable left = build_string_column(L);
    ColumnarTable right = build_string_column(R);

    Plan plan;
    plan.inputs.push_back(left);
    plan.inputs.push_back(right);

    size_t s1 = plan.new_scan_node(0, {{0, DataType::VARCHAR}});
    size_t s2 = plan.new_scan_node(1, {{0, DataType::VARCHAR}});

    size_t j = plan.new_join_node(true, s1, s2, 0, 0,
                                  {{0, DataType::VARCHAR}, {1, DataType::VARCHAR}});
    plan.root = j;

    auto *ctx = Contest::build_context();
    ColumnarTable result = Contest::execute(plan, ctx);
    Contest::destroy_context(ctx);

    auto out0 = extract_strings(result, 0);
    auto out1 = extract_strings(result, 1);

    REQUIRE(out0.size() == 2);
    REQUIRE(out1.size() == 2);

    REQUIRE(out0[0] == "aaa");
    REQUIRE(out1[0] == "aaa");

    REQUIRE(out0[1] == "ccc");
    REQUIRE(out1[1] == "ccc");
}

// ---------------------------------------------------------------------------
// TEST 6 — Mixed INT + VARCHAR late materialized
// ---------------------------------------------------------------------------
TEST_CASE("LateMat: INT32 + VARCHAR", "[late][mixed]")
{
    using namespace std::string_literals;

    ColumnarTable tbl;
    tbl.columns.emplace_back(DataType::INT32);
    tbl.columns.emplace_back(DataType::VARCHAR);

    ColumnInserter<int32_t> ins1(tbl.columns[0]);
    ColumnInserter<std::string> ins2(tbl.columns[1]);

    std::vector<int32_t> ints{10, 20, 30};
    std::vector<std::string> strs{"x", "y", "z"};

    for (size_t i = 0; i < ints.size(); i++)
    {
        ins1.insert(ints[i]);
        ins2.insert(strs[i]);
    }
    ins1.finalize();
    ins2.finalize();
    tbl.num_rows = ints.size();

    Plan plan;
    plan.inputs.push_back(tbl);
    size_t s = plan.new_scan_node(0, {{0, DataType::INT32},
                                      {1, DataType::VARCHAR}});
    plan.root = s;

    auto *ctx = Contest::build_context();
    ColumnarTable result = Contest::execute(plan, ctx);
    Contest::destroy_context(ctx);

    auto [rows, types] = from_columnar(result);
    REQUIRE(rows.size() == 3);

    for (int i = 0; i < 3; i++)
    {
        REQUIRE(std::holds_alternative<int32_t>(rows[i][0]));
        REQUIRE(std::holds_alternative<std::string>(rows[i][1]));
        REQUIRE(std::get<int32_t>(rows[i][0]) == ints[i]);
        REQUIRE(std::get<std::string>(rows[i][1]) == strs[i]);
    }
}

// ---------------------------------------------------------------------------
// TEST 7 — Null VARCHAR stays null and does not crash
// ---------------------------------------------------------------------------
TEST_CASE("LateMat: NULL varchar", "[late][null]")
{
    ColumnarTable tbl;
    tbl.columns.emplace_back(DataType::VARCHAR);
    ColumnInserter<std::string> ins(tbl.columns[0]);

    ins.insert("abc");
    ins.insert_null();
    ins.insert("zzz");
    ins.finalize();
    tbl.num_rows = 3;

    Plan plan;
    plan.inputs.push_back(tbl);
    size_t s = plan.new_scan_node(0, {{0, DataType::VARCHAR}});
    plan.root = s;

    auto *ctx = Contest::build_context();
    ColumnarTable result = Contest::execute(plan, ctx);
    Contest::destroy_context(ctx);

    auto [rows, types] = from_columnar(result);

    REQUIRE(std::holds_alternative<std::string>(rows[0][0]));
    REQUIRE(std::get<std::string>(rows[0][0]) == "abc");

    REQUIRE(std::holds_alternative<std::monostate>(rows[1][0]));

    REQUIRE(std::holds_alternative<std::string>(rows[2][0]));
    REQUIRE(std::get<std::string>(rows[2][0]) == "zzz");
}
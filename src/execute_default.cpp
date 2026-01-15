// execute.cpp
#include <hardware.h>
#include <plan.h>
#include <table.h>
#include <atomic>
#include <cstdlib>
#include <cstdio>
#include <thread>
#include "columnar.h"
#include "hashtable_interface.h" 

// Hash Table Implementations:
#include "unchained_hashtable_wrapper.h"
//#include "robinhood_wrapper.h"
//#include "cuckoo_wrapper.h"
// #include "hopscotch_wrapper.h" 
namespace Contest {

namespace {
struct QueryTelemetry {
    uint64_t joins = 0;
    uint64_t build_rows = 0;
    uint64_t probe_rows = 0;
    uint64_t out_rows = 0;
    uint64_t out_cells = 0;
    uint64_t bytes_strict_min = 0; // keys + output writes
    uint64_t bytes_likely = 0;     // + output reads (value_t)
};

static inline bool join_telemetry_enabled() {
    // (GR) Προαιρετικά stats για να δούμε αν είμαστε memory-bandwidth bound.
    // Default: ENABLED for performance. Set JOIN_TELEMETRY=0 to disable.
    static const bool enabled = [] {
        const char* v = std::getenv("JOIN_TELEMETRY");
        if (!v) return true;  // Default to enabled
        return *v && *v != '0';
    }();
    return enabled;
}

static inline bool auto_build_side_enabled() {
    // Default: enabled. Set AUTO_BUILD_SIDE=0 to disable.
    // (GR) Αυτόματο "optimizer-ish" heuristic: χτίζουμε hash table στη μικρότερη πλευρά.
    static const bool enabled = [] {
        const char* v = std::getenv("AUTO_BUILD_SIDE");
        if (!v) return true;
        return *v && *v != '0';
    }();
    return enabled;
}

static inline bool join_global_bloom_enabled() {
    // Default: ENABLED for performance. Set JOIN_GLOBAL_BLOOM=0 to disable.
    // (GR) Global bloom πριν το probe: μειώνει probes σε άσχετα keys.
    // Χρήσιμο όταν probe πλευρά είναι τεράστια και το selectivity είναι μικρό.
    static const bool enabled = [] {
        const char* v = std::getenv("JOIN_GLOBAL_BLOOM");
        if (!v || !*v) return true;  // Default: enabled
        return *v != '0';
    }();
    return enabled;
}

static inline bool req_build_from_pages_enabled() {
    // Default: enabled (meets assignment requirement for INT32 no-NULL columns).
    // Set REQ_BUILD_FROM_PAGES=0 to force the old vector<HashEntry> build path.
    static const bool enabled = [] {
        const char* v = std::getenv("REQ_BUILD_FROM_PAGES");
        if (!v) return true;
        return *v && *v != '0';
    }();
    return enabled;
}

static inline uint32_t join_global_bloom_bits() {
    // Default: 20 -> 1,048,576 bits (128 KiB).
    // Clamp to a reasonable range so it doesn't blow up memory.
    // (GR) Περισσότερα bits -> λιγότερα false positives αλλά περισσότερη μνήμη.
    static const uint32_t bits = [] {
        const char* v = std::getenv("JOIN_GLOBAL_BLOOM_BITS");
        if (!v || !*v) return 20u;
        const long parsed = std::strtol(v, nullptr, 10);
        if (parsed < 16) return 16u;
        if (parsed > 24) return 24u;
        return static_cast<uint32_t>(parsed);
    }();
    return bits;
}

static std::atomic<uint64_t> g_query_seq{0};
static thread_local QueryTelemetry g_qt;
static thread_local uint64_t g_query_id = 0;

static inline void qt_begin_query() {
    g_query_id = ++g_query_seq;
    g_qt = QueryTelemetry{};
}

static inline void qt_add_join(uint64_t build_rows,
                               uint64_t probe_rows,
                               uint64_t out_rows,
                               uint64_t out_cols) {
    g_qt.joins += 1;
    g_qt.build_rows += build_rows;
    g_qt.probe_rows += probe_rows;
    g_qt.out_rows += out_rows;

    const uint64_t out_cells = out_rows * out_cols;
    g_qt.out_cells += out_cells;

    const uint64_t bytes_keys = (build_rows + probe_rows) * 4ull;     // INT32 join keys
    const uint64_t bytes_out_write = out_cells * 8ull;               // value_t writes (64-bit)
    const uint64_t bytes_out_read = out_cells * 8ull;                // value_t reads (64-bit)

    g_qt.bytes_strict_min += bytes_keys + bytes_out_write;
    g_qt.bytes_likely += bytes_keys + bytes_out_read + bytes_out_write;
}

static inline void qt_end_query() {
    const auto bytes_to_gib = [](uint64_t bytes) -> double {
        return static_cast<double>(bytes) / (1024.0 * 1024.0 * 1024.0);
    };
    const auto ms_at_gbps = [](uint64_t bytes, double gb_per_s) -> double {
        const double seconds = static_cast<double>(bytes) / (gb_per_s * 1e9);
        return seconds * 1000.0;
    };

    // Bandwidth-only lower bounds (very optimistic for hash joins).
    const double bw10 = 10.0;
    const double bw20 = 20.0;
    const double bw40 = 40.0;

    std::fprintf(stderr,
                 "[telemetry q%llu] joins=%llu build=%llu probe=%llu out=%llu out_cells=%llu\n",
                 (unsigned long long)g_query_id,
                 (unsigned long long)g_qt.joins,
                 (unsigned long long)g_qt.build_rows,
                 (unsigned long long)g_qt.probe_rows,
                 (unsigned long long)g_qt.out_rows,
                 (unsigned long long)g_qt.out_cells);

    std::fprintf(stderr,
                 "[telemetry q%llu] bytes_strict_min=%.3f GiB  bytes_likely=%.3f GiB\n",
                 (unsigned long long)g_query_id,
                 bytes_to_gib(g_qt.bytes_strict_min),
                 bytes_to_gib(g_qt.bytes_likely));

    std::fprintf(stderr,
                 "[telemetry q%llu] BW LB strict: %.2f/%.2f/%.2f ms @ %.0f/%.0f/%.0f GB/s\n",
                 (unsigned long long)g_query_id,
                 ms_at_gbps(g_qt.bytes_strict_min, bw10),
                 ms_at_gbps(g_qt.bytes_strict_min, bw20),
                 ms_at_gbps(g_qt.bytes_strict_min, bw40),
                 bw10, bw20, bw40);

    std::fprintf(stderr,
                 "[telemetry q%llu] BW LB likely: %.2f/%.2f/%.2f ms @ %.0f/%.0f/%.0f GB/s\n",
                 (unsigned long long)g_query_id,
                 ms_at_gbps(g_qt.bytes_likely, bw10),
                 ms_at_gbps(g_qt.bytes_likely, bw20),
                 ms_at_gbps(g_qt.bytes_likely, bw40),
                 bw10, bw20, bw40);
}
} // namespace

using ExecuteResult = ColumnBuffer;
ExecuteResult execute_impl(const Plan& plan, size_t node_idx);

// JoinAlgorithm (INT32-only)
struct JoinAlgorithm {
    bool                                               build_left;
    ExecuteResult&                                     left;      // build-side if build_left=true
    ExecuteResult&                                     right;     // probe-side if build_left=true
    ExecuteResult&                                     results;
    size_t                                             left_col, right_col;
    const std::vector<std::tuple<size_t, DataType>>& output_attrs;

    void run_int32() {
        using Key = int32_t;

        struct GlobalBloom {
            uint32_t bits = 0;
            uint64_t mask = 0;
            std::vector<uint64_t> words;

            void init(uint32_t bits_) {
                bits = bits_;
                mask = (bits_ == 64) ? ~0ull : ((1ull << bits_) - 1ull);
                const size_t nbits = 1ull << bits_;
                const size_t nwords = (nbits + 63ull) / 64ull;
                words.assign(nwords, 0ull);
            }

            static inline uint64_t hash32(uint32_t x) {
                // Fast multiplicative hash; good enough for bloom indexing.
                // (GR) Ο bloom δεν χρειάζεται κρυπτογραφική ποιότητα, μόνο γρήγορο mixing.
                return static_cast<uint64_t>(x) * 11400714819323198485ull;
            }

            inline void add_i32(int32_t key) {
                const uint64_t h = hash32(static_cast<uint32_t>(key));
                const uint64_t i1 = (h)&mask;
                const uint64_t i2 = (h >> 32)&mask;
                words[i1 >> 6] |= (1ull << (i1 & 63ull));
                words[i2 >> 6] |= (1ull << (i2 & 63ull));
            }

            inline bool maybe_contains_i32(int32_t key) const {
                const uint64_t h = hash32(static_cast<uint32_t>(key));
                const uint64_t i1 = (h)&mask;
                const uint64_t i2 = (h >> 32)&mask;
                const uint64_t w1 = words[i1 >> 6];
                const uint64_t w2 = words[i2 >> 6];
                return (w1 & (1ull << (i1 & 63ull))) && (w2 & (1ull << (i2 & 63ull)));
            }
        };

        const ColumnBuffer* build_buf = build_left ? &left : &right;
        const ColumnBuffer* probe_buf = build_left ? &right : &left;
        size_t build_key_col = build_left ? left_col : right_col;
        size_t probe_key_col = build_left ? right_col : left_col;

        auto table = create_hashtable<Key>();

        const auto &build_col = build_buf->columns[build_key_col];

        GlobalBloom bloom;
        const bool use_global_bloom = join_global_bloom_enabled();
        if (use_global_bloom) bloom.init(join_global_bloom_bits());

        // -----------------------------------------------------------------
        // Build side: prefer building directly from the input INT32 pages
        // (no NULLs) to avoid copying/materializing the column.
        // Toggle: REQ_BUILD_FROM_PAGES=0 disables this.
        // -----------------------------------------------------------------
        const bool can_build_from_pages = req_build_from_pages_enabled() &&
                                          build_col.is_zero_copy && build_col.src_column != nullptr &&
                                          build_col.page_offsets.size() >= 2;

        std::vector<HashEntry<Key>> entries;
        size_t build_rows_effective = 0;

        if (can_build_from_pages) {
            // Build bloom from pages (still needed for early reject in probe).
            if (use_global_bloom) {
                const auto &offs = build_col.page_offsets;
                const size_t npages = offs.size() - 1;
                for (size_t page_idx = 0; page_idx < npages; ++page_idx) {
                    const size_t base = offs[page_idx];
                    const size_t end = offs[page_idx + 1];
                    const size_t n = end - base;
                    auto *page = build_col.src_column->pages[page_idx]->data;
                    auto *data = reinterpret_cast<const int32_t *>(page + 4);
                    for (size_t slot = 0; slot < n; ++slot) bloom.add_i32(data[slot]);
                }
            }

            const bool built = table->build_from_zero_copy_int32(build_col.src_column,
                                                                build_col.page_offsets,
                                                                build_buf->num_rows);
            if (!built) {
                // Fall back to the old entries path if the table doesn't support it.
                entries.reserve(build_buf->num_rows);
                for (size_t i = 0; i < build_buf->num_rows; ++i) {
                    const value_t &v = build_col.pages[i / build_col.values_per_page][i % build_col.values_per_page];
                    if (!v.is_null()) {
                        entries.push_back(HashEntry<Key>{v.as_i32(), static_cast<uint32_t>(i)});
                    }
                }
                if (entries.empty()) return;
                if (use_global_bloom) {
                    // Rebuild bloom from entries for correctness.
                    bloom.init(join_global_bloom_bits());
                    for (const auto &e : entries) bloom.add_i32(e.key);
                }
                table->reserve(entries.size());
                table->build_from_entries(entries);
                build_rows_effective = entries.size();
            } else {
                build_rows_effective = build_buf->num_rows;
            }
        } else {
            entries.reserve(build_buf->num_rows);
            for (size_t i = 0; i < build_buf->num_rows; ++i) {
                const value_t &v = build_col.pages[i / build_col.values_per_page][i % build_col.values_per_page];
                if (!v.is_null()) {
                    entries.push_back(HashEntry<Key>{v.as_i32(), static_cast<uint32_t>(i)});
                    if (use_global_bloom) bloom.add_i32(v.as_i32());
                }
            }

            if (entries.empty()) return;
            table->reserve(entries.size());
            table->build_from_entries(entries);
            build_rows_effective = entries.size();
        }

        struct OutPair {
            uint32_t lidx;
            uint32_t ridx;
        };

        const size_t probe_n = probe_buf->num_rows;
        size_t hw = std::thread::hardware_concurrency();
        if (!hw) hw = 4;

        // Parallelize only when it pays off.
        // (GR) Για μικρά inputs το overhead των threads είναι μεγαλύτερο από το κέρδος.
        const size_t nthreads = (probe_n >= (1u << 18)) ? hw : 1;
        std::vector<std::vector<OutPair>> out_by_thread(nthreads);

        // Work stealing with atomic counter for dynamic load balancing
        std::atomic<size_t> work_counter{0};
        const size_t work_block_size = std::max(size_t(256), probe_n / (nthreads * 16)); // balanced block size for load stealing

        auto probe_range_with_stealing = [&](size_t tid) {
            auto &local = out_by_thread[tid];
            local.reserve(probe_n / nthreads + 256);

            while (true) {
                // Try to steal a block of work
                size_t begin_j = work_counter.fetch_add(work_block_size, std::memory_order_acquire);
                if (begin_j >= probe_n) break;
                
                size_t end_j = std::min(probe_n, begin_j + work_block_size);

                const auto &probe_col = probe_buf->columns[probe_key_col];

                if (probe_col.is_zero_copy && probe_col.src_column != nullptr && probe_col.page_offsets.size() >= 2) {
                    // (GR) Probe range είναι συνεχές (contiguous) -> κρατάμε per-thread page cursor,
                    // ώστε να αποφεύγουμε binary search στο page_offsets για κάθε row.
                    const auto &offs = probe_col.page_offsets;
                    size_t page_idx = 0;
                    if (begin_j >= offs[1]) {
                        size_t left = 0, right = offs.size() - 1;
                        while (left < right - 1) {
                            size_t mid = (left + right) / 2;
                            if (begin_j < offs[mid]) right = mid;
                            else left = mid;
                        }
                        page_idx = left;
                    }

                    size_t base = offs[page_idx];
                    size_t next = offs[page_idx + 1];
                    auto *page = probe_col.src_column->pages[page_idx]->data;
                    auto *data = reinterpret_cast<const int32_t *>(page + 4);

                    for (size_t j = begin_j; j < end_j; ++j) {
                        while (j >= next) {
                            ++page_idx;
                            base = offs[page_idx];
                            next = offs[page_idx + 1];
                            page = probe_col.src_column->pages[page_idx]->data;
                            data = reinterpret_cast<const int32_t *>(page + 4);
                        }

                        const int32_t probe_key = data[j - base];

                        if (use_global_bloom && !bloom.maybe_contains_i32(probe_key)) continue;

                        size_t len = 0;
                        const auto *bucket = table->probe(probe_key, len);
                        if (!bucket || len == 0) continue;

                        for (size_t k = 0; k < len; ++k) {
                            if (bucket[k].key != probe_key) continue;
                            const uint32_t build_row = bucket[k].row_id;
                            if (build_left)
                                local.push_back(OutPair{build_row, static_cast<uint32_t>(j)});
                            else
                                local.push_back(OutPair{static_cast<uint32_t>(j), build_row});
                        }
                    }
                } else {
                    // Materialized probe path
                    for (size_t j = begin_j; j < end_j; ++j) {
                        const value_t &v = probe_col.pages[j / probe_col.values_per_page][j % probe_col.values_per_page];
                        if (v.is_null()) continue;
                        const int32_t probe_key = v.as_i32();

                        if (use_global_bloom && !bloom.maybe_contains_i32(probe_key)) continue;

                        size_t len = 0;
                        const auto *bucket = table->probe(probe_key, len);
                        if (!bucket || len == 0) continue;

                        for (size_t k = 0; k < len; ++k) {
                            if (bucket[k].key != probe_key) continue;
                            const uint32_t build_row = bucket[k].row_id;
                            if (build_left)
                                local.push_back(OutPair{build_row, static_cast<uint32_t>(j)});
                            else
                                local.push_back(OutPair{static_cast<uint32_t>(j), build_row});
                        }
                    }
                }
            }
        };

        if (nthreads == 1) {
            probe_range_with_stealing(0);
        } else {
            std::vector<std::thread> threads;
            threads.reserve(nthreads);
            for (size_t t = 0; t < nthreads; ++t) {
                threads.emplace_back(probe_range_with_stealing, t);
            }
            for (auto &th : threads) th.join();
        }

        size_t total_out = 0;
        for (auto &v : out_by_thread) total_out += v.size();
        if (total_out == 0) return;

        // Allocate output columns once, then fill.
        // (GR) Το output materialization είναι συχνά bottleneck. Προ-δεσμεύουμε ακριβώς όση μνήμη χρειάζεται
        // (total_out rows) και γράφουμε με άμεσο indexing αντί για append() σε value_t.
        const size_t num_output_cols = output_attrs.size();

        if (join_telemetry_enabled()) {
            qt_add_join(static_cast<uint64_t>(build_rows_effective),
                        static_cast<uint64_t>(probe_n),
                        static_cast<uint64_t>(total_out),
                        static_cast<uint64_t>(num_output_cols));
        }

        struct OutputMap {
            bool from_left;
            uint32_t idx;
        };
        std::vector<OutputMap> out_map;
        out_map.reserve(num_output_cols);

        for (size_t col = 0; col < num_output_cols; ++col) {
            auto &dst = results.columns[col];
            dst.pages.clear();
            dst.page_offsets.clear();
            dst.src_column = nullptr;
            dst.is_zero_copy = false;
            dst.cached_page_idx = 0;
            dst.num_values = total_out;

            const size_t page_sz = dst.values_per_page;
            size_t written = 0;
            while (written < total_out) {
                const size_t take = std::min(page_sz, total_out - written);
                dst.pages.emplace_back(take);
                written += take;
            }

            const size_t left_cols = left.num_cols();
            const size_t src = std::get<0>(output_attrs[col]);
            if (src < left_cols)
                out_map.push_back(OutputMap{true, static_cast<uint32_t>(src)});
            else
                out_map.push_back(OutputMap{false, static_cast<uint32_t>(src - left_cols)});
        }

        // All columns use the same page size (ColumnBuffer constructs them with 1024).
        const size_t out_page_sz = results.columns.empty() ? 1024 : results.columns[0].values_per_page;
        // For very large outputs, materialization can dominate; do it in parallel.
        // (GR) Όταν το αποτέλεσμα είναι τεράστιο, το copy των value_t κυριαρχεί -> parallel fill.
        const bool parallel_materialize = (nthreads > 1) && (total_out >= (1u << 20));

        if (!parallel_materialize) {
            size_t out_idx = 0;
            for (size_t t = 0; t < nthreads; ++t) {
                for (const auto &op : out_by_thread[t]) {
                    const size_t lidx = op.lidx;
                    const size_t ridx = op.ridx;

                    const size_t page_idx = out_idx / out_page_sz;
                    const size_t off = out_idx % out_page_sz;

                    for (size_t col = 0; col < num_output_cols; ++col) {
                        const auto m = out_map[col];
                        if (m.from_left)
                            results.columns[col].pages[page_idx][off] = left.columns[m.idx].get(lidx);
                        else
                            results.columns[col].pages[page_idx][off] = right.columns[m.idx].get(ridx);
                    }
                    ++out_idx;
                }
            }
        } else {
            std::vector<size_t> base(nthreads + 1, 0);
            for (size_t t = 0; t < nthreads; ++t) base[t + 1] = base[t] + out_by_thread[t].size();

            std::vector<std::thread> threads;
            threads.reserve(nthreads);
            for (size_t t = 0; t < nthreads; ++t) {
                threads.emplace_back([&, t]() {
                    const size_t start = base[t];
                    size_t out_idx = start;
                    std::vector<size_t> caches(num_output_cols, 0);

                    for (const auto &op : out_by_thread[t]) {
                        const size_t lidx = op.lidx;
                        const size_t ridx = op.ridx;

                        const size_t page_idx = out_idx / out_page_sz;
                        const size_t off = out_idx % out_page_sz;

                        for (size_t col = 0; col < num_output_cols; ++col) {
                            const auto m = out_map[col];
                            if (m.from_left)
                                results.columns[col].pages[page_idx][off] = left.columns[m.idx].get_cached(lidx, caches[col]);
                            else
                                results.columns[col].pages[page_idx][off] = right.columns[m.idx].get_cached(ridx, caches[col]);
                        }
                        ++out_idx;
                    }
                });
            }
            for (auto &th : threads) th.join();
        }

        results.num_rows = total_out;
    }
};


ExecuteResult execute_hash_join(const Plan&                plan,
    const JoinNode&                                    join,
    const std::vector<std::tuple<size_t, DataType>>& output_attrs)
{
    size_t left_idx  = join.left;
    size_t right_idx = join.right;

    auto& left_node  = plan.nodes[left_idx];
    auto& right_node = plan.nodes[right_idx];

    auto left  = execute_impl(plan, left_idx);
    auto right = execute_impl(plan, right_idx);

    // Optimizer-ish: prefer building the hash table on the smaller side.
    // Only override the plan hint when there is a clear size win.
    bool effective_build_left = join.build_left;
    if (auto_build_side_enabled()) {
        const size_t l = left.num_rows;
        const size_t r = right.num_rows;
        if (l * 10ull <= r * 9ull) effective_build_left = true;
        else if (r * 10ull <= l * 9ull) effective_build_left = false;
    }

    // prepare output ColumnBuffer
    ColumnBuffer results(output_attrs.size(), 0);
    results.types.reserve(output_attrs.size());
    for (auto& t : output_attrs)
        results.types.push_back(std::get<1>(t));

    JoinAlgorithm ja {
        .build_left   = effective_build_left,
        .left         = left,
        .right        = right,
        .results      = results,
        .left_col     = join.left_attr,
        .right_col    = join.right_attr,
        .output_attrs = output_attrs
    };

    // enforce INT32 join key
    if (effective_build_left) {
        if (std::get<1>(left_node.output_attrs[join.left_attr]) != DataType::INT32)
            throw std::runtime_error("Only INT32 join columns supported.");
    } else {
        if (std::get<1>(right_node.output_attrs[join.right_attr]) != DataType::INT32)
            throw std::runtime_error("Only INT32 join columns supported.");
    }

    // run the hashtable join
    ja.run_int32();

    return results;
}


ExecuteResult execute_scan(const Plan&                  plan,
    const ScanNode&                                    scan,
    const std::vector<std::tuple<size_t, DataType>>& output_attrs)
{
    return scan_columnar_to_columnbuffer(plan, scan, output_attrs);
}


ExecuteResult execute_impl(const Plan& plan, size_t node_idx) {
    auto& node = plan.nodes[node_idx];
    return std::visit(
        [&](auto const& value) -> ExecuteResult {
            using T = std::decay_t<decltype(value)>;
            if constexpr (std::is_same_v<T, JoinNode>)
                return execute_hash_join(plan, value, node.output_attrs);
            else
                return execute_scan(plan, value, node.output_attrs);
        },
        node.data);
}

ColumnarTable execute(const Plan& plan, void* context) {
    (void)context;
    if (join_telemetry_enabled()) qt_begin_query();
    auto buf = execute_impl(plan, plan.root);
    if (join_telemetry_enabled()) qt_end_query();
    return finalize_columnbuffer_to_columnar(
        plan, buf, plan.nodes[plan.root].output_attrs
    );
}

void* build_context() { return nullptr; }
void destroy_context(void*) {}

} // namespace Contest
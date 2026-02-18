// execute.cpp — basic hash join implementation
#include <hardware.h>              
#include <plan.h>                  
#include <table.h>                 
#include <atomic>                  
#include <cstdlib>                 
#include <cstdio>                  
#include <thread>                  
#include "columnar.h"             
#include "hashtable_interface.h"  
#include "join_telemetry.h"       
#include "work_stealing.h"        

#include "unchained_hashtable_wrapper.h"   // Parallel unchained (default)
// Hash Table implementations (keep the fastest)
//#include "robinhood_wrapper.h"
//#include "cuckoo_wrapper.h"
//#include "hopscotch_wrapper.h"
namespace Contest {

using ExecuteResult = ColumnBuffer;                       // Intermediate results buffer
ExecuteResult execute_impl(const Plan& plan, size_t node_idx); // Forward declaration

// JoinAlgorithm (INT32-only)
// Handles build, probe, and result materialization phases
struct JoinAlgorithm {
    bool build_left;                                      // Which side builds the hash table
    ExecuteResult& left;                                  // Left input
    ExecuteResult& right;                                 // Right input
    ExecuteResult& results;                               // Output buffer
    size_t left_col, right_col;                           // Join columns
    const std::vector<std::tuple<size_t, DataType>>& output_attrs;  // Output schema

    // Execute join with INT32 keys
    void run_int32() {
        using Key = int32_t;
        const ColumnBuffer* build_buf = build_left ? &left : &right;   // Select build side
        const ColumnBuffer* probe_buf = build_left ? &right : &left;   // Select probe side
        size_t build_key_col = build_left ? left_col : right_col;      // Build key column
        size_t probe_key_col = build_left ? right_col : left_col;      // Probe key column

        auto table = create_hashtable<Key>();                          // Create hash table

        const auto &build_col = build_buf->columns[build_key_col];     // Build column

        // BUILD: prefer zero-copy INT32 without NULLs
        const bool can_build_from_pages = build_col.is_zero_copy && build_col.src_column != nullptr &&
                                          build_col.page_offsets.size() >= 2;

        std::vector<HashEntry<Key>> entries;                           // Fallback entries list
        size_t build_rows_effective = 0;                               // Effective number of build rows

        if (can_build_from_pages) {
            // Build directly from pages (no copy)
            const bool built = table->build_from_zero_copy_int32(build_col.src_column,
                                                                build_col.page_offsets,
                                                                build_buf->num_rows);
            if (!built) {
                // If the implementation doesn't support it, fallback to copying
                entries.reserve(build_buf->num_rows);
                for (size_t i = 0; i < build_buf->num_rows; ++i) {
                    const value_t &v = build_col.pages[i / build_col.values_per_page][i % build_col.values_per_page];
                    if (!v.is_null()) entries.push_back(HashEntry<Key>{v.as_i32(), static_cast<uint32_t>(i)});
                }
                if (entries.empty()) return;              // Nothing to build
                table->reserve(entries.size());           // Pre-reserve
                table->build_from_entries(entries);       // Regular build
                build_rows_effective = entries.size();    // Number of build rows
            } else {
                build_rows_effective = build_buf->num_rows; // All rows were used
            }
        } else {
            // Copy-based implementation (supports NULLs / non-zero-copy)
            entries.reserve(build_buf->num_rows);
            for (size_t i = 0; i < build_buf->num_rows; ++i) {
                const value_t &v = build_col.pages[i / build_col.values_per_page][i % build_col.values_per_page];
                if (!v.is_null()) entries.push_back(HashEntry<Key>{v.as_i32(), static_cast<uint32_t>(i)});
            }

            if (entries.empty()) return;                  // No entries
            table->reserve(entries.size());               // Pre-reserve
            table->build_from_entries(entries);           // Build
            build_rows_effective = entries.size();        // How many were inserted
        }

        struct OutPair {
            uint32_t lidx;
            uint32_t ridx;
        };

        const size_t probe_n = probe_buf->num_rows;       // Number of probe rows
        size_t hw = std::thread::hardware_concurrency();  // Available threads
        if (!hw) hw = 4;                                   // Fallback

        // Allow override for experiments
        const char* force_threads_env = std::getenv("FORCE_THREADS");
        size_t forced_threads = 0;
        if (force_threads_env && *force_threads_env) {
            forced_threads = static_cast<size_t>(std::atoi(force_threads_env));
        }

        // Parallelize only when it pays off.
        // For small inputs thread overhead may outweigh benefits.
        const size_t nthreads = forced_threads > 0 ? forced_threads : 
                    ((probe_n >= (1u << 18)) ? hw : 1); // Parallelize only when beneficial
        std::vector<std::vector<OutPair>> out_by_thread(nthreads); // Per-thread local results

        // PROBE PHASE: work stealing with atomic counter for dynamic load balancing
        WorkStealingConfig ws_config{                      // Work stealing settings
            .total_work = probe_n,
            .num_threads = nthreads,
            .min_block_size = 256,
            .blocks_per_thread = 16
        };
        WorkStealingCoordinator ws_coordinator(ws_config); // Work stealing coordinator

        auto probe_range_with_stealing = [&](size_t tid) { // Probe function with dynamic blocks
            auto &local = out_by_thread[tid];
            local.reserve(probe_n / nthreads + 256);       // Pre-reserve local output

            size_t begin_j, end_j;
            while (ws_coordinator.steal_block(begin_j, end_j)) { // Steal a work block

                const auto &probe_col = probe_buf->columns[probe_key_col];

                if (probe_col.is_zero_copy && probe_col.src_column != nullptr && probe_col.page_offsets.size() >= 2) {
                    // Probe range is contiguous -> keep a per-thread page cursor
                    // to avoid binary searching page_offsets for each row.
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

                    for (size_t j = begin_j; j < end_j; ++j) { // Scan the block
                        while (j >= next) {
                            ++page_idx;
                            base = offs[page_idx];
                            next = offs[page_idx + 1];
                            page = probe_col.src_column->pages[page_idx]->data;
                            data = reinterpret_cast<const int32_t *>(page + 4);
                        }

                        const int32_t probe_key = data[j - base];      // Read key

                        size_t len = 0;
                        const auto *bucket = table->probe(probe_key, len); // Lookup in hash
                        if (!bucket || len == 0) continue;                // No match

                        for (size_t k = 0; k < len; ++k) {               // Linear scan of bucket
                            if (bucket[k].key != probe_key) continue;     // Confirm same key
                            const uint32_t build_row = bucket[k].row_id;  // Build row
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
                        if (v.is_null()) continue;                       // Ignore NULL
                        const int32_t probe_key = v.as_i32();

                        size_t len = 0;
                        const auto *bucket = table->probe(probe_key, len); // Lookup
                        if (!bucket || len == 0) continue;

                        for (size_t k = 0; k < len; ++k) {
                            if (bucket[k].key != probe_key) continue;     // Check same key
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
            probe_range_with_stealing(0);                 // Serial probe
        } else {
            std::vector<std::thread> threads;
            threads.reserve(nthreads);
            for (size_t t = 0; t < nthreads; ++t) threads.emplace_back(probe_range_with_stealing, t);
            for (auto &th : threads) th.join();           // Synchronize
        }

        size_t total_out = 0;                             // Total results
        for (auto &v : out_by_thread) total_out += v.size();
        if (total_out == 0) return;                       // No matches -> empty result

        // Allocate output columns once, then fill.
        // Output materialization is often a bottleneck. Pre-reserve exactly the memory needed
        // (total_out rows) and write with direct indexing instead of append() on value_t.
        const size_t num_output_cols = output_attrs.size(); // Number of output columns

        if (Contest::join_telemetry_enabled()) {          // Record telemetry if enabled
            Contest::qt_add_join(static_cast<uint64_t>(build_rows_effective),
                                 static_cast<uint64_t>(probe_n),
                                 static_cast<uint64_t>(total_out),
                                 static_cast<uint64_t>(num_output_cols));
        }

        struct OutputMap { bool from_left; uint32_t idx; }; // Source column (left/right) and index
        std::vector<OutputMap> out_map;
        out_map.reserve(num_output_cols);

        for (size_t col = 0; col < num_output_cols; ++col) { // Prepare each output column
            auto &dst = results.columns[col];
            dst.pages.clear();
            dst.page_offsets.clear();
            dst.src_column = nullptr;
            dst.is_zero_copy = false;
            dst.cached_page_idx = 0;
            dst.num_values = total_out;

            const size_t page_sz = dst.values_per_page; // Page size (fixed)
            size_t written = 0;
            while (written < total_out) {               // Allocate pages to fit all
                const size_t take = std::min(page_sz, total_out - written);
                dst.pages.emplace_back(take);
                written += take;
            }

            const size_t left_cols = left.num_cols();   // Number of columns from left input
            const size_t src = std::get<0>(output_attrs[col]); // Source index
            if (src < left_cols)
                out_map.push_back(OutputMap{true, static_cast<uint32_t>(src)});           // From left
            else
                out_map.push_back(OutputMap{false, static_cast<uint32_t>(src - left_cols)}); // From right
        }

        // All columns use the same page size (ColumnBuffer constructs them with 1024).
        const size_t out_page_sz = results.columns.empty() ? 1024 : results.columns[0].values_per_page; // Output page size
        const bool parallel_materialize = false; // Single-threaded output materialization (work required)

        if (!parallel_materialize) {
            size_t out_idx = 0;                           // Current output index
            for (size_t t = 0; t < nthreads; ++t) {
                for (const auto &op : out_by_thread[t]) {
                    const size_t lidx = op.lidx;
                    const size_t ridx = op.ridx;

                    const size_t page_idx = out_idx / out_page_sz; // Page
                    const size_t off = out_idx % out_page_sz;      // Offset within page

                    for (size_t col = 0; col < num_output_cols; ++col) {
                        const auto m = out_map[col];
                        if (m.from_left)
                            results.columns[col].pages[page_idx][off] = left.columns[m.idx].get(lidx);
                        else
                            results.columns[col].pages[page_idx][off] = right.columns[m.idx].get(ridx);
                    }
                    ++out_idx; // Next output row
                }
            }
        } else {
            std::vector<size_t> base(nthreads + 1, 0);          // Prefix sums for per-thread offsets
            for (size_t t = 0; t < nthreads; ++t) base[t + 1] = base[t] + out_by_thread[t].size();

            std::vector<std::thread> threads;
            threads.reserve(nthreads);
            for (size_t t = 0; t < nthreads; ++t) {
                threads.emplace_back([&, t]() {
                    const size_t start = base[t];
                    size_t out_idx = start;
                    std::vector<size_t> caches(num_output_cols, 0); // Page caches for get_cached

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
    size_t left_idx  = join.left;   // Index of left child
    size_t right_idx = join.right;  // Index of right child

    auto& left_node  = plan.nodes[left_idx];   // Plan node for left
    auto& right_node = plan.nodes[right_idx];  // Plan node for right

    auto left  = execute_impl(plan, left_idx);  // Execute left subtree
    auto right = execute_impl(plan, right_idx); // Execute right subtree

    bool effective_build_left = join.build_left; // Build side as chosen by planner

    // prepare output ColumnBuffer
    ColumnBuffer results(output_attrs.size(), 0); // Prepare output buffer
    results.types.reserve(output_attrs.size());
    for (auto& t : output_attrs) results.types.push_back(std::get<1>(t));

    JoinAlgorithm ja {
        .build_left   = effective_build_left,
        .left         = left,
        .right        = right,
        .results      = results,
        .left_col     = join.left_attr,
        .right_col    = join.right_attr,
        .output_attrs = output_attrs
    };

    // Ensure the join key is INT32 on the build side
    if (effective_build_left) {
        if (std::get<1>(left_node.output_attrs[join.left_attr]) != DataType::INT32)
            throw std::runtime_error("Only INT32 join columns supported.");
    } else {
        if (std::get<1>(right_node.output_attrs[join.right_attr]) != DataType::INT32)
            throw std::runtime_error("Only INT32 join columns supported.");
    }

    ja.run_int32(); // Execute hash join

    return results;
}


ExecuteResult execute_scan(const Plan&                  plan,
    const ScanNode&                                    scan,
    const std::vector<std::tuple<size_t, DataType>>& output_attrs)
{
    return scan_columnar_to_columnbuffer(plan, scan, output_attrs); // Convert scan to ColumnBuffer
}


ExecuteResult execute_impl(const Plan& plan, size_t node_idx) {
    auto& node = plan.nodes[node_idx];                           // Get the node
    return std::visit(                                           // Examine variant
        [&](auto const& value) -> ExecuteResult {
            using T = std::decay_t<decltype(value)>;
            if constexpr (std::is_same_v<T, JoinNode>)           // If it's a Join
                return execute_hash_join(plan, value, node.output_attrs);
            else                                                 // Otherwise Scan
                return execute_scan(plan, value, node.output_attrs);
        },
        node.data);
}

ColumnarTable execute(const Plan& plan, void* context) {
    (void)context;                                              // Unused
    if (Contest::join_telemetry_enabled()) Contest::qt_begin_query(); // Begin telemetry
    auto buf = execute_impl(plan, plan.root);                   // Execute plan root
    if (Contest::join_telemetry_enabled()) Contest::qt_end_query();   // End telemetry
    return Contest::finalize_columnbuffer_to_columnar(          // Convert to ColumnarTable
        plan, buf, plan.nodes[plan.root].output_attrs
    );
}

void* build_context() { return nullptr; }
void destroy_context(void*) {}

} // namespace Contest
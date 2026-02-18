# SIGMOD-Style Query Runtime (C++)

High-performance analytical query runtime in modern C++, optimized for join-heavy workloads.

This repository focuses on a single execution path: optimized mode.

## Highlights

- Custom hash tables: unchained (flat), robin hood, cuckoo, and hopscotch variants.
- Columnar execution path for reduced cache misses and less row-wise overhead.
- Late materialization to defer expensive payload work until final output.
- Zero-copy INT32 build path to avoid intermediate tuple materialization.
- Parallel probing with work-stealing for better load balance on large workloads.
- Optional runtime instrumentation with `JOIN_TELEMETRY=1`.

## Build

```bash
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release -Wno-dev
cmake --build build --target fast -j "$(nproc)"
```

## Run

```bash
./build/fast plans.json
```

Enable telemetry:

```bash
JOIN_TELEMETRY=1 ./build/fast plans.json
```

## Tests

```bash
cmake --build build --target software_tester -j "$(nproc)"
./build/software_tester --reporter compact
```

## Project Layout

- `include/`: core runtime components (hash tables, bloom filter, allocator, execution helpers)
- `src/`: execution pipeline, parser integration, columnar/runtime implementations
- `tests/`: software tester and runtime-focused test suites
- `docs/`: implementation notes and supplementary technical documentation
- `job/`: benchmark SQL workloads

## Measurements (Optimized Pipeline)

Representative results from the optimization path:

| Iteration | Technique | Runtime |
|-----------|-----------|---------|
| 0 | `std::unordered_map` baseline | 242.85s |
| 1 | Robin Hood hashing | 233.25s |
| 2 | Column-store layout | 132.53s |
| 3 | Late materialization | 64.33s |
| 4 | Unchained hash table | 46.12s |
| 5 | Zero-copy INT32 build | 27.24s |
| 6 | Final optimized mode | 12.1s |

Thread scaling snapshot (optimized mode):

| Threads | Query Runtime | Speedup vs 1T |
|---------|---------------|---------------|
| 1 | 18.3s | 1.00x |
| 2 | 14.7s | 1.24x |
| 4 | 12.1s | 1.51x |
| 8 | 12.2s | 1.50x |

## Key Implementation Files

## Performance Changes (Before → After)

- **Before:** The original implementation (see `original_execute.md`) used row-wise in-memory tables (`std::vector<std::vector<Data>>`), generic `std::unordered_map` for joins, and materialized full tuples early. Execution relied on variant-driven dynamic dispatch and performed many copies of records, which increased memory traffic and reduced cache locality.

- **After (what changed):**
	- **Columnar layout:** columns are stored separately to improve cache locality and reduce per-row overhead.
	- **Late materialization:** payloads are assembled only when required, avoiding unnecessary copies.
	- **Unchained flat hash table + zero-copy INT32 build:** replaced `std::unordered_map` with a contiguous, cache-friendly flat hash table and a zero-copy build path for 32-bit keys to eliminate intermediate tuple materialization.
	- **Parallel probing with work-stealing:** probe phase is parallelized with a work-stealing scheduler to improve load balance and CPU utilization.
	- **Type-specialized fast paths:** minimized generic variant dispatch by using specialized code paths for primitive types.
	- **Telemetry:** added lightweight instrumentation (enable with `JOIN_TELEMETRY=1`) to measure bytes processed, bandwidth, and per-operator runtimes.

- **Effect:** The combined changes dramatically reduce memory traffic and improve cache behavior; representative numbers show end-to-end runtime dropping from the baseline 242.85s to 12.1s in the final optimized mode. Thread scaling demonstrates substantial gains up to 4 cores.


- `include/parallel_unchained_hashtable.h` - optimized unchained hash table and zero-copy build path
- `include/work_stealing.h` / `src/work_stealing.cpp` - probe-side work-stealing scheduler
- `include/columnar.h` / `src/columnar.cpp` - columnar storage and access path
- `src/execute_default.cpp` - main optimized join execution pipeline
- `src/late_materialization.cpp` - materialization logic

## Notes

- This codebase originates from a university database systems project and is maintained here as a personal/public portfolio.

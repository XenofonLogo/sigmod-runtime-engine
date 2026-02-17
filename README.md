# SIGMOD-Style Query Runtime (C++)

High-performance analytical query runtime in modern C++, optimized for join-heavy workloads.

This repository includes custom hash-table strategies, column-oriented execution, late materialization, zero-copy build paths, and multithreaded probe/build optimizations.

## Highlights

- **Custom hash tables**: unchained (flat), robin hood, cuckoo, and hopscotch variants.
- **Columnar execution path**: reduced cache misses and less row-wise overhead.
- **Late materialization**: defers expensive payload work until final output.
- **Zero-copy INT32 build**: avoids intermediate tuple materialization where possible.
- **Parallel runtime**: partition-based build and work-stealing probe strategy.
- **Two execution modes**:
  - `OPTIMIZED` (default): best runtime throughput.
  - `STRICT_PROJECT=1`: requirement-focused path with stricter partitioning behavior.

## Build

```bash
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release -Wno-dev
cmake --build build --target fast -j "$(nproc)"
```

## Run

Default (optimized mode):

```bash
./build/fast plans.json
```

Strict mode:

```bash
STRICT_PROJECT=1 ./build/fast plans.json
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

## Technical Focus Areas

- Cache-aware join execution
- Hash-based indexing and probing
- Thread-local and partition-aware allocation
- Work-stealing scheduling for skew handling
- Correctness/performance trade-offs across execution modes

## Notes

- This codebase originates from a university database systems project and is maintained here as a personal/public engineering portfolio.
- Historical commit authorship is preserved intentionally.

## Contributing

See `CONTRIBUTING.md` for development workflow and contribution guidelines.

## Measurements: Performance Analysis and Optimization Path

### Optimization History (7 Iterations to Final Optimized Runtime)

The project followed a step-by-step optimization path, ending at `12.1s` query runtime in optimized mode.

| Iteration | Technique | Runtime | Improvement | Cumulative |
|-----------|-----------|---------|-------------|------------|
| 0 | `std::unordered_map` (baseline) | 242.85s | — | — |
| 1 | Robin Hood hashing | 233.25s | 4.0% | 4.0% |
| 2 | Column-store layout | 132.53s | 43.5% | 45.4% |
| 3 | Late materialization | 64.33s | 51.4% | 73.5% |
| 4 | Unchained hash table | 46.12s | 28.3% | 81.0% |
| 5 | Zero-copy INT32 build | 27.24s | 40.9% | 88.8% |
| 6 | STRICT mode reference | 38.6s | - | 84.1% |
| 7 | OPTIMIZED mode final | 12.1s | 68.7% vs strict | 95.0% |

### Experiment 1: Hash Table Structures

Comparison of hash-table implementations on equivalent workloads.

| Hash Table | Total Time | Memory |
|------------|------------|--------|
| `std::unordered_map` | 242.8s | ~900 MB |
| Robin Hood | 233.2s | ~850 MB |
| Cuckoo | 237.2s | ~870 MB |
| Hopscotch | 238.0s | ~880 MB |
| Unchained + Column + Late | 46.12s | ~410 MB |
| Robin Hood (optimized pipeline) | 37.914s | — |
| Cuckoo (optimized pipeline) | 36.163s | — |
| Hopscotch (optimized pipeline) | 38.670s | — |
| Unchained (optimized pipeline) | 13.2s | 234 MB |

Observations:
- Unchained + zero-copy is dramatically faster than baseline.
- Robin Hood alone gives modest gains compared with deeper pipeline changes.
- Strict mode trades raw speed for stronger requirement compliance.

### Experiment 2: Column-Store vs Row-Store

| Storage Layout | Total Runtime |
|----------------|---------------|
| Row-oriented baseline | 292.6s |
| Column-oriented | 132.5s |
| + Late materialization | 64.3s |
| + Zero-copy INT32 | 27.2s |

Observations:
- Columnar layout provides major runtime reduction.
- Late materialization further cuts processing overhead.
- Zero-copy INT32 build gives another substantial improvement.

### Experiment 3: Parallel Scaling (Optimized Mode)

| Threads | Query Runtime | Speedup vs 1T | Efficiency | Wall Time |
|---------|---------------|---------------|------------|-----------|
| 1 | 18.3s | 1.00x | 100% | 68.7s |
| 2 | 14.7s | 1.24x | 62.0% | 61.4s |
| 4 | 12.1s | 1.51x | 37.8% | 59.4s |
| 8 | 12.2s | 1.50x | 18.8% | 61.3s |
| 12 | 12.4s | 1.48x | 12.3% | 61.9s |
| 20 | 12.8s | 1.43x | 7.2% | 62.0s |

Observations:
- Best point is around `4` threads for this workload.
- Returns diminish after 4 threads due to I/O and parsing overheads.
- Wall time remains dominated by dataset load + non-query overhead.

### Experiment 4: Partition Count (Strict Mode)

| Partitions | Query Runtime | Wall Time | Memory | Trade-off |
|------------|---------------|-----------|--------|-----------|
| 16 | 34.6s | 84.6s | 4.29 GB | Higher contention |
| 64 | 32.4s | 80.3s | 3.89 GB | Best balance |
| 128 | 35.3s | 84.0s | 4.06 GB | Coordination overhead |
| 256 | 35.8s | 85.9s | 4.22 GB | Higher overhead |

Observation: `64` partitions provides the best balance between contention and overhead in strict mode.

### Experiment 5: Memory Footprint Breakdown

**Optimized mode**
- Peak memory: 4.34 GB (4,442 MB)
- Query runtime: 12.1s (4 threads)
- Wall time: 59.4s (including I/O)
- CPU time: 62.6s total

**Strict mode**
- Peak memory: 3.89 GB (3,990 MB)
- Query runtime: 32.4s
- Wall time: 80.3s
- CPU time: 98.4s total

Interpretation:
- Optimized mode uses more memory but executes significantly faster.
- Strict mode is slower due to stronger partitioned execution constraints.
- Peak memory includes loaded CSV data (~3.6 GB) in both modes.

## Key Implementation Files

### Header Files (`include/`)

**Core execution and configuration**
- `project_config.h` - mode flags (`STRICT_PROJECT`, optimized path, telemetry)
- `hashtable_interface.h` - interface for hash-table implementations
- `hash_functions.h` - hash function definitions
- `hash_common.h` - shared structures and constants

**Column storage and data management**
- `columnar.h` - `ColumnBuffer` definition (pages, offsets, caches)
- `inner_column.h` - internal column representation
- `table.h` - table structure and metadata
- `table_entity.h` - table entity definitions
- `attribute.h` - attribute metadata
- `late_materialization.h` - late materialization helpers

**Hash table implementations**
- `unchained_hashtable.h` - base unchained hash table (single-pass)
- `parallel_unchained_hashtable.h` - partition-based unchained hash table (strict mode)
- `partition_hash_builder.h` - parallel partition build helpers
- `robinhood.h`, `robinhood_wrapper.h` - robin hood hashing
- `cuckoo.h`, `cuckoo_wrapper.h` - cuckoo hashing
- `hopscotch.h`, `hopscotch_wrapper.h` - hopscotch hashing
- `unchained_hashtable_wrapper.h` - production wrapper for unchained path
- `cuckoo_map.h` - alternative cuckoo-based map

**Optimization components**
- `bloom_filter.h` - bloom filtering for fast pre-rejection
- `work_stealing.h` - work-stealing scheduler
- `slab_allocator.h` - three-level slab allocator (strict mode path)
- `join_telemetry.h` - runtime telemetry data

**Infrastructure**
- `plan.h` - query plan structures
- `statement.h` - SQL statement parsing
- `csv_parser.h` - CSV data loading
- `hardware.h` - hardware capability detection
- `common.h` - common utilities

### Source Files (`src/`)

- `execute_default.cpp` - main join execution path (strict + optimized)
- `columnar.cpp` - column data management
- `work_stealing.cpp` - load balancing coordinator
- `slab_allocator.cpp` - slab allocator implementation
- `late_materialization.cpp` - materialization logic
- `join_telemetry.cpp` - telemetry tracking/reporting
- `build_table.cpp` - table construction
- `statement.cpp` - statement processing
- `csv_parser.cpp` - CSV parsing

### Documentation (`docs/`)

- `PARADOTEO_1.md` - hash table analysis and robin hood details
- `PARADOTEO_2.md` - column-store and late materialization
- `PARADOTEO_3.md` - parallel execution and zero-copy path
- `ADDITIONAL_IMPLEMENTATIONS.md` - additional optimized implementations

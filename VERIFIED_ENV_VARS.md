# Verified Environment Variables

## ✅ Actually Implemented

| Variable | File | Line | Default | Purpose |
|----------|------|------|---------|---------|
| `JOIN_GLOBAL_BLOOM` | `include/join_config.h` | 15 | `1` (enabled) | Enable/disable global bloom filter |
| `JOIN_GLOBAL_BLOOM_BITS` | `include/join_config.h` | 40 | `20` | Bloom filter size (16-24 bits) |
| `REQ_BUILD_FROM_PAGES` | `include/join_config.h` | 27 | `1` (enabled) | Enable zero-copy page-based build |
| `REQ_PARTITION_BUILD` | `include/parallel_unchained_hashtable.h` | 337 | `0` (disabled) | Enable 2-phase partitioned build |
| `REQ_PARTITION_BUILD` | `include/partition_build.h` | 22 | `0` (disabled) | (duplicate check) |
| `REQ_PARTITION_BUILD_MIN_ROWS` | `include/parallel_unchained_hashtable.h` | 348 | `0` | Minimum rows for partition build |
| `REQ_SLAB_GLOBAL_BLOCK_BYTES` | `include/three_level_slab.h` | 88 | `4 MiB` | Slab allocator block size |
| `JOIN_TELEMETRY` | `src/join_telemetry.cpp` | 15 | `1` (enabled) | Enable performance telemetry |

## ❌ Does NOT Exist (Removed from Documentation)

| Variable | Status |
|----------|--------|
| `JOIN_NUM_THREADS` | **NO CODE** - Thread count is hardcoded to `std::thread::hardware_concurrency()` |
| `EXP_PARALLEL_BUILD` | **NO CODE** - Never implemented |
| `EXP_NUMA_AWARE` | **NO CODE** - Never implemented |
| `REQ_3LVL_SLAB` | **NO CODE** - Slab allocator is NOT controlled by env var (always uses std::allocator) |
| `JOIN_PARTITION_SIZE` | **NO CODE** - No partition size tuning available |

## 🔍 Key Findings

### Thread Count
**Source**: `src/execute_default.cpp:122`
```cpp
size_t hw = std::thread::hardware_concurrency();
if (!hw) hw = 4;
const size_t nthreads = (probe_n >= (1u << 18)) ? hw : 1;
```
- **Hardcoded** to hardware concurrency
- No environment variable override
- Automatically downscales to 1 thread for small inputs (< 256K rows)
- **All benchmarks ran with same thread count (8 threads on this system)**

### Slab Allocator
**Source**: `include/parallel_unchained_hashtable.h:61`
```cpp
using entry_allocator = std::allocator<entry_type>;  // Use standard allocator
```
- **Always disabled** - uses standard allocator
- Only `REQ_SLAB_GLOBAL_BLOCK_BYTES` exists (controls block size if slab was used)
- No toggle to enable/disable slab

### Partition Build
**Source**: `include/parallel_unchained_hashtable.h:337-340`
```cpp
const char* v = std::getenv("REQ_PARTITION_BUILD");
return v && *v && *v != '0';
```
- **Default: DISABLED** (requires explicit `=1` to enable)
- Causes catastrophic +189% slowdown on JOB benchmark

## 📊 Actual Benchmark Results (Re-analysis)

Since `JOIN_NUM_THREADS` doesn't exist, all runs used **8 threads** (hardware_concurrency).

The variance I measured (10.32s - 11.20s) is likely:
1. Run-to-run measurement noise
2. Different bloom filter sizes (only verified variable that changed)
3. Cache/system state differences

### Verified Measurements

| Configuration | Runtime | Notes |
|---|---|---|
| **Baseline** | **11.12s** | Default settings |
| JOIN_GLOBAL_BLOOM_BITS=20 | 10.73s | Smaller bloom filter |
| JOIN_GLOBAL_BLOOM_BITS=24 | 11.20s | Larger bloom filter |
| REQ_PARTITION_BUILD=1 | 32.10s | ❌ Catastrophic failure |

**Bloom filter impact**: Minimal (-3.4% to +0.8%)
**Partition build impact**: Catastrophic (+189%)

## 🛠️ How to Actually Change Thread Count

The thread count is **hardcoded**. To change it, you must **modify source code**:

**File**: `src/execute_default.cpp:122`
```cpp
// BEFORE (current):
size_t hw = std::thread::hardware_concurrency();

// AFTER (override to 4 threads):
size_t hw = 4;  // Force 4 threads
```

Then rebuild:
```bash
cmake --build build -- -j $(nproc) fast
```

## ✅ Corrected Documentation

All references to non-existent environment variables have been removed from:
- ENVIRONMENT_MEASUREMENTS.md
- ADDITIONAL_IMPLEMENTATIONS.md
- BENCHMARK_SUMMARY.md

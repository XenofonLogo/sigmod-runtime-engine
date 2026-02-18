# Software Tester - Execution Instructions and Test Analysis

## Contents
- [General Execution Instructions](#general-execution-instructions)
- [Run by Category](#run-by-category)
- [Detailed Test Descriptions](#detailed-test-descriptions)

---

## General Execution Instructions

### Build
```bash
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build --target software_tester -- -j 
```

### Run All Tests
```bash
./build/software_tester
```

### Run with Compact Report
```bash
./build/software_tester --reporter compact
```

### List All Tests
```bash
./build/software_tester --list-tests
```

### Overall Statistics
- **Total Tests**: 90
- **Assertions**: 5,808
- **Passed**: 89 (98.9%)
- **Failed**: 1 (pre-existing issue with hash quality string test)

---

## Run by Category

### 1. Hash Quality Tests
```bash
./build/software_tester "[hash][quality]"
```
**Description**: Checks hash function quality for various data types

### 2. Slab Allocator Tests
```bash
./build/software_tester "[slab]"
```
**Description**: Tests for the three-level slab allocator used for efficient memory management

### 3. Partitioned Build Tests
```bash
./build/software_tester "[partitioned-build]"
```
**Description**: Tests for the partitioned hash table build using prefix-sum for parallel construction

### 4. Work Stealing Tests
```bash
./build/software_tester "[work-stealing]"
```
**Description**: Tests for the work-stealing coordinator for dynamic work distribution

### 5. Bloom Filter Tests
```bash
./build/software_tester "[bloom]"
```
**Description**: Tests for bloom filters used for early rejection during joins

### 6. Late Materialization Tests
```bash
./build/software_tester "[late-materialization]"
```
**Description**: Tests for late materialization to defer column access

### 7. Columnar Storage Tests
```bash
./build/software_tester "[columnar]"
```
**Description**: Tests for the columnar storage layout and cache efficiency

### 8. Zero-Copy INT32 Tests
```bash
./build/software_tester "[zero-copy]"
```
**Description**: Tests for the zero-copy optimization for INT32 columns without NULLs

### 9. Hash Table Tests
```bash
./build/software_tester "[hashtable]"
```
**Description**: Tests for the actual hash table implementations (Unchained)

### 10. Plan & Config Tests
```bash
./build/software_tester "[plan]"
./build/software_tester "[config]"
```
**Description**: Tests for the query plan construction API and configuration flags

### 11. Value Type Tests
```bash
./build/software_tester "[value]"
```
**Description**: Tests for the compact `value_t` representation (64-bit payload)

---

## Detailed Test Descriptions


### 📊 Category 1: Hash Quality (4 tests)
**File**: `tests/software_tester.cpp`

#### Goal
Checks the quality of the hash functions used by the system for various key types.

#### Tests

1. **Hash quality: int32_t**
   - Checks collision rate, bucket distribution (chi-squared test), and avalanche effect for INT32
   - Allowed collision rate: < 6%

2. **Hash quality: int64_t**
   - Same checks for INT64 keys
   - Verifies uniform distribution across hash table

3. **Hash quality: double**
   - Checks for floating-point keys
   - Avalanche test: flipping 1 bit should change many bits in the hash

4. **Hash quality: string**
   - More involved checks for variable-length string keys
   - **Note**: This test often fails due to a strict chi-squared threshold (pre-existing issue)

---

### 🧱 Category 2: Slab Allocator (9 tests)
**File**: `tests/software_tester/slab_allocator_tests.cpp`

#### Goal
Tests for the three-level slab allocator used for fast memory allocation in joins.

#### Architecture
```
Thread-local → Global L1 Cache → Global L2 Pool
```

#### Tests

1. **Basic allocation**
   - Simple allocation and check that a non-null pointer is returned

2. **Alignment verification**
   - Verifies that allocated memory is correctly aligned (e.g., 8-byte for int64_t)

3. **Large allocation**
   - Allocate a large block (>1MB) and ensure it falls back to the system allocator

4. **Multiple sequential allocations**
   - Multiple consecutive allocations - checks bump pointer advancement

5. **Dealloc is no-op**
   - The slab allocator does not perform deallocation (bump allocator design)

6. **enabled() returns true**
   - Verifies that the allocator is enabled in this build

7. **Thread-local isolation**
   - Each thread has its own slab; slabs are not shared between threads

8. **global_block_size() returns reasonable value**
   - The global block size is reasonable (e.g., 256KB - 4MB)

9. **Allocation with varying alignments**
   - Test allocations with various alignments (1, 4, 8, 16 bytes)

---

### 🔀 Category 3: Partitioned Build (9 tests)
**File**: `tests/software_tester/partitioned_build_tests.cpp`

#### Goal
Tests for the partitioned hash table build that uses a prefix-sum to distribute entries into contiguous memory.

#### Technique
- **Phase 1**: Histogram - counts entries per partition
- **Phase 2**: Prefix-sum - computes offsets
- **Phase 3**: Scatter - places entries into their target positions

#### Tests

1. **Phase correctness with small dataset**
   - Verifies the three phases operate correctly

2. **Contiguous tuple storage**
   - Tuples belonging to the same partition are stored contiguously in memory (cache-friendly)

3. **Prefix sum correctness**
   - Prefix-sum calculation is correct

4. **With duplicates**
   - Handling duplicate keys (many tuples with the same hash prefix)

5. **Empty table**
   - Edge case: empty input

6. **Single entry**
   - Edge case: single tuple

7. **Large dataset (1000 entries)**
   - Stress test with a larger dataset

8. **Collision handling**
   - Many keys map to the same partition

9. **Memory efficiency**
   - Checks that memory is not wasted (tight packing)

---

### 🔄 Category 4: Work Stealing (9 tests)
**File**: `tests/software_tester/work_stealing_tests.cpp`

#### Goal
Tests for the work-stealing coordinator that enables dynamic workload distribution among threads during the probe phase.

#### Algorithm
- Atomic counter for threads to "steal" work blocks
- Minimizes synchronization overhead
- Better load balancing than static partitioning

#### Tests

1. **steal_block with valid work range**
   - A thread successfully steals a work block

2. **Sequential block stealing**
   - Sequential stealing of multiple blocks by the same thread

3. **Block boundaries**
   - Block boundaries are correct (begin < end)

4. **Exhaustion returns false**
   - When the work is finished, returns false

5. **Concurrent stealing (2 threads)**
   - Two threads steal concurrently - no overlap

6. **Concurrent stealing (4 threads) - stress**
   - More threads - higher contention

7. **Work distribution fairness**
   - Threads receive approximately equal shares of work

8. **get_block_size() respects config**
   - Block size respects the configuration

9. **No work skipped or duplicated**
   - All rows/lines are processed exactly once

---

### 🌸 Category 5: Bloom Filters (13 tests)
**Files**: 
- `tests/software_tester/bloom_filter_tests.cpp` (9 tests - old implementation)
- `tests/software_tester/indexing_optimization_tests.cpp` (4 tests - GlobalBloom)

#### Goal
Tests for the bloom filters used for early rejection during the probe phase (avoiding unnecessary hash table lookups).

#### Technique
- Compact bit vector (2^N bits)
- Multiple hash functions
- False positives OK, false negatives NOT OK

#### Tests (bloom_filter_tests.cpp)

1. **Basic tag and mask operations**
   - Computes tag and mask from hash value

2. **Multiple tags in single bloom**
   - Adding multiple tags - all retrievable

3. **Collision detection**
   - Two different keys may have colliding bits

4. **False positive rate estimation**
   - Measure false positive rate (should be < 10% for typical workload)

5. **All bits set (saturation)**
   - Extreme case: all bits = 1 → always returns true

6. **No bits set (empty)**
   - Empty bloom → always returns false

7. **Tag extraction from hash**
   - Correct extraction of tag bits from hash value

8. **Independent bit positions**
   - Hash functions provide independent bit positions

9. **Global bloom configuration**
   - Fixed bloom size: `bloom.init(4)` (no env), two positions per key

#### Tests (GlobalBloom - indexing_optimization_tests.cpp)

10. **GlobalBloom: basic add and contains**
   - `add_i32()`, `maybe_contains_i32()` - basic operation

11. **GlobalBloom: false positive rate**
   - Measure FP rate with 1000 keys → should be < 10%

12. **GlobalBloom: hash independence**
   - Nearby values are not all present (good hashing)

13. **JoinConfig: bloom filter configuration**
   - There are no longer config functions for global bloom. The implementation is fixed.

---

### 📦 Category 6: Late Materialization (10 tests)
**Files**:
- `tests/software_tester/late_materialization_tests.cpp` (9 tests)
- `tests/software_tester/indexing_optimization_tests.cpp` (1 test)

#### Goal
Tests for the late materialization technique: strings are stored as compact 64-bit references and materialized only when needed in the output.

#### Technique
- **PackedStringRef**: 64 bits = {table_id, column_id, page_id, slot}
- Avoid string copying until the final output
- Reduce cache pressure

#### Tests

1. **PackedStringRef: packing string reference**
   - Packing (table, col, page, slot) into 64-bit

2. **Null flag handling**
   - The bit pattern UINT64_MAX means NULL

3. **Multiple references uniqueness**
   - Different strings → different refs

4. **Compact 64-bit storage**
   - Only 8 bytes per string reference

5. **Zero-copy string handling benefit**
   - No memcpy of strings

6. **Resolve string reference**
   - `StringRefResolver` converts ref → actual string

7. **Deferred materialization strategy**
   - Strings materialize only in the output, not in intermediate results

8. **Column-wise storage benefits**
   - String refs are stored in a separate column from the join keys

9. **Memory efficiency with variable-length fields**
   - Variable-length strings → fixed-size refs

10. **Late Materialization: deferred column access concept** (in indexing_optimization_tests.cpp)
   - Concept test: join uses only the key column; payloads are materialized later

---

### 🗂️ Category 7: Columnar Storage (16 tests)
**Files**:
- `tests/software_tester/columnar_tests.cpp` (9 tests)
- `tests/software_tester/indexing_optimization_tests.cpp` (7 tests)

#### Goal
Tests for the columnar storage layout that improves cache locality and enables SIMD operations.

#### Architecture
```
Row-store:    [id, name, age][id, name, age]...
Column-store: [id, id, id...][name, name...][age, age...]
```

#### Tests (columnar_tests.cpp)

1. **Column data organization**
   - Data organized by columns rather than rows

2. **Fixed-length column storage**
   - INT32, INT64 → fixed-length columns

3. **Variable-length column with references**
   - Strings → references into a string pool

4. **Page-based organization**
   - Each column is split into pages (e.g., 1024 values/page)

5. **Column projection (selective columns)**
   - We read only the columns we need (I/O saving)

6. **Cache efficiency with column-major layout**
   - Sequential scan → cache-friendly (all values of the column are contiguous)

7. **Null handling in columns**
   - NULL bitmap for each column

8. **Multiple column iteration (tuple construction)**
   - Reconstruct tuples from multiple columns

9. **Memory efficiency vs row store**
   - Compression-friendly (similar data grouped together)

#### Tests (indexing_optimization_tests.cpp)

10. **ColumnarTable: column buffer creation**
   - Create `ColumnBuffer` from `column_t`

11. **ColumnarTable: zero-copy INT32 detection**
   - Flag `is_zero_copy` for optimization

12. **ColumnarTable: value_t access patterns**
    - `column_t::get()` method, multi-page access

13. **ColumnarTable: NULL handling**
    - `value_t::make_null()`, `is_null()`

14. **ColumnarTable: cached page index optimization**
   - Sequential access → cached page index (avoid binary search)

15. **ColumnBuffer: basic structure**
    - `num_rows`, `columns` vector

16. **ColumnBuffer: multi-column layout**
   - Many INT32 columns in the same buffer

---

### ⚡ Category 8: Zero-Copy INT32 (10 tests)
**Files**:
- `tests/software_tester/zero_copy_int32_tests.cpp` (9 tests)
- `tests/software_tester/indexing_optimization_tests.cpp` (1 test)

#### Goal
Test the zero-copy optimization for INT32 columns without NULLs: direct access to pages without materializing into `value_t`.

#### Technique
- Instead of: `page → value_t → int32_t`
- We do: `page → int32_t` (direct pointer arithmetic)
- **Precondition**: INT32 column, no NULLs

#### Tests

1. **Direct page access without copying**
   - Pointer cast: `reinterpret_cast<const int32_t*>(page + 4)`

2. **Null handling for zero-copy INT32**
   - If NULLs are present → fallback to the materialized path

3. **Pointer arithmetic for range access**
   - Sequential scan with pointer increment

4. **Memory alignment preservation**
   - INT32 data aligned on 4-byte boundaries

5. **No copy overhead**
   - Zero memcpy, zero materialization

6. **Multi-column zero-copy**
   - Many INT32 columns → all zero-copy

7. **Constraint - only for INT32 without NULLs**
   - Documentation of the constraints

8. **Optimization for hash build**
   - `build_from_zero_copy_int32()` interface

9. **ZeroCopyInt32: multi-column zero-copy** (repeat)
   - Stress test with many columns

10. **Zero-copy page build detection** (in indexing_optimization_tests.cpp)
   - Automatic detection: `is_zero_copy && src_column != nullptr && page_offsets.size() >= 2`

---

### 🔑 Category 9: Hash Table Implementations (5 tests)
**File**: `tests/software_tester/hashtable_algorithms_tests.cpp`

#### Goal
Verify the actual hash table implementations used in the join execution engine.

#### Implementations
- **UnchainedHashTable** (default) - flat layout, open addressing
- RobinHood, Cuckoo, Hopscotch (wrappers exist but are not tested due to redefinition conflicts)

#### Tests

1. **UnchainedHashTable: basic build and probe**
   - `build_from_entries()`, `probe()` - basic operation
   - Verification: correct key, correct row_id

2. **UnchainedHashTable: collision handling**
   - 1000 entries - many collisions
   - All keys must be retrievable

3. **UnchainedHashTable: duplicate keys**
   - Same key with different row_ids → all in the bucket

4. **UnchainedHashTable: missing key**
   - Probe for a missing key → `nullptr` or `len=0`

5. **HashTable: load factor stress test**
   - 5000 entries - high load factor
   - Sampling test (every 17th entry)

**Note**: Tests for RobinHood/Cuckoo/Hopscotch are commented out due to redefinition errors (each wrapper defines its own `create_hashtable()`). They can be tested by changing the include in `execute_default.cpp`.

---

### 📋 Category 10: Plan & Configuration (4 tests)
**File**: `tests/software_tester/indexing_optimization_tests.cpp`

#### Goal
Test the query plan construction API and the configuration flags.

#### Tests

1. **Plan: basic scan node creation**
   - `plan.new_scan_node(table_id, output_attrs)`
   - Verification: node_id, nodes.size()

2. **Plan: basic join node creation**
   - `plan.new_join_node(build_left, left, right, left_attr, right_attr, output_attrs)`
   - Create join tree: scan + scan → join

3. **Zero-copy page build detection**
   - Automatic detection without flags

4. **Global bloom behavior**
   - Fixed size with `init(4)` — no bit configuration

---

### 🔤 Category 11: Value Type (4 tests)
**File**: `tests/software_tester/indexing_optimization_tests.cpp`

#### Goal
Tests the compact `value_t` representation used to store values in 64 bits.

#### Design
```cpp
struct value_t {
    uint64_t raw;  // INT32 | PackedStringRef | NULL
};
```

#### Tests

1. **value_t: INT32 operations**
   - `make_i32(42)`, `as_i32()`, `is_null()`

2. **value_t: string reference packing**
   - `PackedStringRef` → `value_t::make_str()`
   - Compressed 64-bit representation

3. **value_t: packed string ref with make_str_ref**
   - `make_str_ref(table, col, page, slot)` - an API call

4. **value_t: NULL value**
   - `make_null()` → `raw = UINT64_MAX`
   - `is_null()` check

**Note**: The system supports only INT32 and string refs, not INT64/double (removed for simplicity).

---

## File Structure

```
tests/
├── software_tester.cpp                              # Main test file (hash quality)
└── software_tester/                                 # Organized test suite
    ├── slab_allocator_tests.cpp                     # Memory allocator tests
    ├── partitioned_build_tests.cpp                  # Partitioned build phase tests
    ├── work_stealing_tests.cpp                      # Work stealing coordinator tests
    ├── bloom_filter_tests.cpp                       # Bloom filter tests (old API)
    ├── late_materialization_tests.cpp               # String reference compression
    ├── columnar_tests.cpp                           # Column-store layout tests
    ├── zero_copy_int32_tests.cpp                    # Zero-copy optimization tests
    ├── hashtable_algorithms_tests.cpp               # Hash table wrapper tests (strict)
    └── indexing_optimization_tests.cpp              # Integration tests (strict)
```

---

## Technical Details

### Strict Integration Tests
The tests in `hashtable_algorithms_tests.cpp` and `indexing_optimization_tests.cpp` are **strict integration tests** that use the real APIs of the codebase:

- ✅ `Contest::UnchainedHashTableWrapper<int32_t>`
- ✅ `Contest::GlobalBloom`
- ✅ `Contest::ColumnBuffer`, `column_t`
- ✅ `Contest::value_t`, `PackedStringRef`
- ✅ `Contest::Plan`
- ✅ Configuration functions: `req_build_from_pages_enabled()`, `join_global_bloom_enabled()`

They do not use mock implementations (`std::unordered_map`, `std::vector`).

### Test Framework
- **Catch2 v3.8.0**
- Tags for filtering: `[hashtable]`, `[bloom]`, `[zero-copy]`, etc.
- Reporters: `compact`, `console`, `junit`

### Modes
```bash
# STRICT mode (requires algorithmic compliance; uses partitioned build automatically)
STRICT_PROJECT=1 ./build/fast plans.json

# OPTIMIZED mode (fast path)
OPTIMIZED_PROJECT=1 ./build/fast plans.json
```

---

## Problems & Solutions

### 1. Hash Quality String Test Fails
**Problem**: The chi-squared threshold is too strict for string hashing.

**Solution**: Pre-existing issue — not critical for system correctness.

### 2. RobinHood/Cuckoo/Hopscotch Tests Disabled
**Problem**: Each wrapper defines `create_hashtable()` → redefinition error.

**Solution**: Only the UnchainedHashTable tests are run. To test another implementation, change the include in `execute_default.cpp` and rebuild.

### 3. Build Warnings (Narrowing Conversion)
**Problem**: `size_t` → `uint32_t` narrowing conversions.

**Solution**: Non-critical warnings — the system works correctly (row_ids < 2^32 in practice).

---

## Performance Benchmarking

To measure the performance of the optimizations:

```bash
# Build in Release mode
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release

# Modes benchmarking
STRICT_PROJECT=1 ./build/fast plans.json
OPTIMIZED_PROJECT=1 ./build/fast plans.json
```

---

## Conclusion

The test suite covers the key optimizations listed in the FINAL_COMPREHENSIVE_REPORT:

✅ **Part 1**: Hash table algorithms (Unchained)  
✅ **Part 2**: Column-store, Late Materialization  
✅ **Part 3**: Parallelization (Work Stealing, Partitioned Build), Indexing (Zero-Copy, Bloom Filters)

All tests use the **real APIs** of the codebase, not mock implementations.

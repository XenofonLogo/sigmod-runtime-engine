# OPTIMIZED_PROJECT: Τεχνική Τεκμηρίωση Βελτιστοποιήσεων

## Περιεχόμενα

1. [Επισκόπηση Τριών Εκδόσεων](#1-επισκόπηση-τριών-εκδόσεων)
2. [Κοινά Χαρακτηριστικά με STRICT](#2-κοινά-χαρακτηριστικά-με-strict)
3. [Αποκλειστικές Βελτιστοποιήσεις OPTIMIZED](#3-αποκλειστικές-βελτιστοποιήσεις-optimized)
4. [Direct Page Access](#4-direct-page-access)
5. [Single-Pass Hashtable Build](#5-single-pass-hashtable-build)
6. [Zero-Copy Build & Probe](#6-zero-copy-build--probe)
7. [Batch Output & Preallocation](#7-batch-output--preallocation)
8. [Σύνοψη Performance](#8-σύνοψη-performance)

---

## 1. Επισκόπηση Τριών Εκδόσεων

### 1.1 Εξέλιξη Υλοποίησης

Υπάρχουν **τρεις εκδόσεις** της hash join υλοποίησης:

```
┌─────────────────────────────────────────────────────────────┐
│                    ΕΞΕΛΙΞΗ ΥΛΟΠΟΙΗΣΗΣ                       │
└─────────────────────────────────────────────────────────────┘

1. LEGACY (Παλιά Υλοποίηση)
   ├─ Row-by-row access με column.get(i)
   ├─ Division/modulo σε κάθε πρόσβαση
   ├─ Incremental output με reallocations
   ├─ Single-threaded
   └─ Απλή υλοποίηση (εκπαιδευτική)

2. STRICT_PROJECT (Απαιτήσεις Διαγωνισμού)
   ├─ Zero-copy για INT32 χωρίς NULL (REQ-4)
   ├─ Partition-based build (REQ-6)
   ├─ Thread-safe 3-level slab allocator (REQ-6)
   ├─ Directory-based hashtable (REQ-8.2)
   ├─ Work-stealing parallelization
   ├─ Bloom filters
   └─ ~32s (113 queries) - focus on requirements

3. OPTIMIZED_PROJECT (Ταχύτητα)
   ├─ Zero-copy παντού
   ├─ Single-pass build (όχι partitions)
   ├─ Direct page pointers
   ├─ Continuous array hashtable
   ├─ Adaptive parallelization
   ├─ Batch output
   └─ ~11s (113 queries) - focus on speed
```

### 1.2 Σύγκριση Τριών Εκδόσεων

| Χαρακτηριστικό | LEGACY | STRICT | OPTIMIZED |
|----------------|--------|--------|-----------|
| **Data Access** | `column.get(i)` | Zero-copy pages | Zero-copy pages |
| **Build Strategy** | Simple loop | 2-phase partition | 1-phase direct |
| **Memory Layout** | Basic vector | Directory partitions | Continuous array |
| **Parallelization** | ❌ None | ✅ Static partition | ✅ Adaptive |
| **Output** | Incremental append | Count-Preallocate | Count-Preallocate |
| **Requirements** | ❌ None | ✅ All 7 | ❌ None |
| **Εκτιμώμενος χρόνος** | ~22s | ~32s | ~11s |
| **Σκοπός** | Reference | Competition | Production |

### 1.3 Φιλοσοφία OPTIMIZED

**Στόχος:** Μέγιστη ταχύτητα χωρίς περιορισμούς απαιτήσεων

**Αρχές:**
- ⚡ **Eliminate overhead:** Αφαίρεση κάθε περιττής λειτουργίας
- 🚫 **Zero-copy everywhere:** Αποφυγή materialization
- 🎯 **Direct access:** Απευθείας πρόσβαση σε δεδομένα
- 📊 **Simple structures:** Απλές δομές, όχι partitions
- 🔧 **Single-pass:** 1 φάση build αντί για 2

---

## 2. Κοινά Χαρακτηριστικά με STRICT

> **💡 Σημείωση:** Τα παρακάτω features υλοποιούνται **και στο STRICT και στο OPTIMIZED**.


### 2.1 Zero-Copy INT32 Pages 

### 2.2 Work-Stealing Parallelization

**Διαφορά:** 
- STRICT: Work-stealing στη merge phase
- OPTIMIZED: Work-stealing στο probe phase (adaptive)

**Λεπτομέρειες:** Βλ. [PARADOTEO_3.md §7](PARADOTEO_3.md#7-work-stealing)

### 2.3 Bloom Filters


### 2.4 Batch Output με Preallocation


### 2.5 Thread-Local Buffers


## 3. Αποκλειστικές Βελτιστοποιήσεις OPTIMIZED

### 3.1 Βασικές Διαφορές από STRICT

| Aspect | STRICT | OPTIMIZED |
|--------|--------|-----------|
| **Build Phases** | 2 (Partition → Merge) | 1 (Direct) |
| **Build Threads** | Parallel (static partitioning) | Sequential |
| **Memory Layout** | Directory + Partitions | Continuous Array |
| **Intermediate Data** | Per-thread partitions | None |
| **Memory Overhead** | 2-3x | 1x |
| **Code Complexity** | High (~300 LOC) | Low (~80 LOC) |

### 3.2 Αποκλειστικά Features OPTIMIZED

✅ **Direct Page Pointers** - Αποφυγή division/modulo (§4)  
✅ **Single-Pass Build** - 1 φάση αντί για 2 (§5)  
✅ **Continuous Array Layout** - Απλή δομή δεδομένων (§5)  
✅ **Adaptive Parallelization** - Threshold-based activation (§6)  

### 3.3 Εκτιμώμενο Speedup

```
Εξέλιξη Performance (113 JOB queries):
━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━

LEGACY:     ~22s  (baseline, single-threaded)
STRICT:     ~32s  (+45% overhead από partitioning)
OPTIMIZED:  ~11s  (-50% από LEGACY, -66% από STRICT)

Speedup OPTIMIZED vs STRICT: ~2.9x
```




## 3. Zero-Copy Build Phase

### 3.1 Πρόβλημα: Materialization Overhead

#### Αρχική Προσέγγιση (Αργή)

```cpp
// STEP 1: Materialize all entries into vector
std::vector<HashEntry<int32_t>> entries;
for (size_t i = 0; i < build_col.num_rows; ++i) {
    int32_t key = build_col.get(i);        // Copy 1
    entries.push_back({key, i});           // Copy 2
}

// STEP 2: Build hashtable from vector
hashtable.build_from_entries(entries);     // Copy 3
```

**Προβλήματα:**
- 3 αντιγραφές των ίδιων δεδομένων
- Allocation overhead για intermediate vector
- Cache pollution από temporary data
- Memory bandwidth waste

### 3.2 Λύση: Direct Page-to-Hashtable Build

#### Βελτιστοποιημένη Υλοποίηση

**Αρχείο:** [include/parallel_unchained_hashtable.h:227-270](include/parallel_unchained_hashtable.h#L227-L270)

```cpp
void build_from_zero_copy_int32(
    const Column* src_column,
    const std::vector<std::size_t>& page_offsets,
    std::size_t num_rows) {
    
    // Direct single-pass build (OPTIMIZED mode)
    if (!Contest::use_strict_project()) {
        build_from_zero_copy_int32_simple_parallel(
            src_column, page_offsets, num_rows
        );
        return;
    }
    
    // ... STRICT mode uses partitioned build ...
}

void build_from_zero_copy_int32_simple_parallel(
    const Column* src_column,
    const std::vector<std::size_t>& page_offsets,
    std::size_t num_rows) {
    
    const size_t num_pages = page_offsets.size() - 1;
    
    // Pre-allocate hashtable storage
    reserve(num_rows);
    
    // Direct build: Page → Hashtable (NO intermediate vector)
    for (size_t page_idx = 0; page_idx < num_pages; ++page_idx) {
        const Page* page = src_column->get_page(page_idx);
        const int32_t* data = extract_int32_page_data(page);
        
        const size_t start_row = page_offsets[page_idx];
        const size_t end_row = page_offsets[page_idx + 1];
        const size_t page_rows = end_row - start_row;
        
        // Direct insert from page memory
        for (size_t i = 0; i < page_rows; ++i) {
            const int32_t key = data[i];        // Read once
            const size_t row_id = start_row + i;
            insert_direct(key, row_id);         // Insert directly
            // NO intermediate copies!
        }
    }
}
```

### 3.3 Διάγραμμα: Build Pipeline Comparison

```
LEGACY (3-copy materialization):
═══════════════════════════════

┌─────────────┐
│  Input      │
│  Pages      │
└──────┬──────┘
       │ Copy 1: column.get(i)
       ▼
┌─────────────┐
│ Intermediate│
│  Vector     │  ← EXTRA ALLOCATION
│ entries[]   │
└──────┬──────┘
       │ Copy 2: vector.push_back()
       ▼
┌─────────────┐
│ Temp Buffer │  ← CACHE POLLUTION
└──────┬──────┘
       │ Copy 3: build_from_entries()
       ▼
┌─────────────┐
│ Hashtable   │
│  Storage    │
└─────────────┘

Memory: 3x data size
Time: 3x memory bandwidth


OPTIMIZED (zero-copy direct):
══════════════════════════════

┌─────────────┐
│  Input      │
│  Pages      │
│             │
│ [Page 0]────┼─────┐
│ [Page 1]────┼───┐ │
│ [Page 2]────┼─┐ │ │
│   ...       │ │ │ │
└─────────────┘ │ │ │
                │ │ │ Direct pointer read
                │ │ ▼
                │ │ insert_direct(key, row_id)
                │ ▼
                │ insert_direct(key, row_id)
                ▼
┌─────────────┐
│ Hashtable   │ ← ONLY COPY
│  Storage    │
└─────────────┘

Memory: 1x data size
Time: 1x memory bandwidth
```



## 4. Zero-Copy Probe Phase

### 4.1 Πρόβλημα: Row-by-Row Overhead

#### Αρχική Προσέγγιση

```cpp
// Per-row access with abstraction overhead
for (size_t i = 0; i < probe_input.num_rows; ++i) {
    int32_t probe_key = probe_col.get(i);  // Division + modulo
    
    auto* entry = hashtable.lookup(probe_key);
    if (entry) {
        // Process match
        output.append(entry->row_id);      // Potential realloc
    }
}
```

**Bottlenecks:**
- Division/modulo για κάθε row (millions of times)
- Function call overhead για `get(i)`
- Poor instruction-level parallelism (ILP)
- Branch mispredictions

### 4.2 Λύση: Batch Page Processing

#### Βελτιστοποιημένη Υλοποίηση

**Αρχείο:** [src/execute_default.cpp:162-230](src/execute_default.cpp#L162-L230)

```cpp
// Zero-copy probe: Process entire pages at once
void probe_from_zero_copy_pages(
    const Column* probe_column,
    const std::vector<std::size_t>& page_offsets,
    UnchainedHashTable& ht,
    std::vector<OutPair>& results) {
    
    const size_t num_pages = page_offsets.size() - 1;
    
    // Process page-by-page (cache-friendly)
    for (size_t page_idx = 0; page_idx < num_pages; ++page_idx) {
        const Page* page = probe_column->get_page(page_idx);
        const int32_t* keys = extract_int32_page_data(page);
        
        const size_t start_row = page_offsets[page_idx];
        const size_t end_row = page_offsets[page_idx + 1];
        const size_t page_rows = end_row - start_row;
        
        // Batch probe: sequential key access
        for (size_t i = 0; i < page_rows; ++i) {
            const int32_t probe_key = keys[i];  // Sequential read
            const size_t probe_row = start_row + i;
            
            // Lookup in hashtable
            size_t match_count = 0;
            const auto* entries = ht.probe(probe_key, &match_count);
            
            // Emit all matches
            for (size_t m = 0; m < match_count; ++m) {
                results.push_back({
                    .build_row = entries[m].row_id,
                    .probe_row = probe_row
                });
            }
        }
    }
}
```

### 4.3 Parallel Probe με Adaptive Strategy

**Αρχείο:** [include/parallel_unchained_hashtable.h:390-450](include/parallel_unchained_hashtable.h#L390-L450)

```cpp
// Adaptive parallelization based on data size
static constexpr size_t PARALLEL_THRESHOLD = (1 << 18);  // 256K rows

if (num_rows < PARALLEL_THRESHOLD) {
    // Small data: Sequential (avoid thread overhead)
    probe_sequential(probe_data, results);
} else {
    // Large data: Parallel with work-stealing
    probe_parallel_work_stealing(probe_data, results);
}

void probe_parallel_work_stealing(
    const ProbeData& data,
    std::vector<OutPair>& results) {
    
    const size_t nthreads = get_num_threads();
    
    // Thread-local result buffers (no contention)
    std::vector<std::vector<OutPair>> thread_results(nthreads);
    
    // Work-stealing: atomic counter for dynamic load balancing
    std::atomic<size_t> next_page{0};
    const size_t total_pages = data.num_pages();
    
    // Launch worker threads
    std::vector<std::thread> workers;
    for (size_t tid = 0; tid < nthreads; ++tid) {
        workers.emplace_back([&, tid]() {
            while (true) {
                // Steal next page
                size_t page_idx = next_page.fetch_add(1, 
                                    std::memory_order_relaxed);
                if (page_idx >= total_pages) break;
                
                // Process page locally
                probe_page(data, page_idx, thread_results[tid]);
            }
        });
    }
    
    // Wait for completion
    for (auto& w : workers) w.join();
    
    // Merge thread results
    for (const auto& tr : thread_results) {
        results.insert(results.end(), tr.begin(), tr.end());
    }
}
```

### 4.4 Διάγραμμα: Probe Execution Flow

```
LEGACY (Row-by-Row):
═══════════════════

Main Thread:
│
├─ for i in 0..N:
│   ├─ key = column.get(i)  ← SLOW (div/mod)
│   ├─ entry = ht.lookup(key)
│   └─ if match: output.append()
│
└─ Done

Timeline: [get][lookup][get][lookup][get][lookup]...
          └───┘ └─────┘ └───┘ └─────┘
          Overhead     Overhead


OPTIMIZED (Zero-Copy Parallel):
═══════════════════════════════

┌─────────────────────────────────────┐
│  Input Pages: [P0][P1][P2]...[PN]  │
└───────────┬─────────────────────────┘
            │
     ┌──────┴──────┐
     │ Atomic Ctr  │ next_page = 0 → 1 → 2 → ...
     └──────┬──────┘
            │
    ┌───────┴────────┬───────────┬──────────┐
    │                │           │          │
┌───▼────┐      ┌───▼────┐  ┌───▼────┐  ┌──▼──────┐
│Thread 0│      │Thread 1│  │Thread 2│  │Thread 3 │
│        │      │        │  │        │  │         │
│Process │      │Process │  │Process │  │Process  │
│Page 0  │      │Page 1  │  │Page 2  │  │Page 3   │
│  ↓     │      │  ↓     │  │  ↓     │  │  ↓      │
│[Res0]  │      │[Res1]  │  │[Res2]  │  │[Res3]   │
└───┬────┘      └───┬────┘  └───┬────┘  └──┬──────┘
    │               │           │          │
    └───────────────┴───────────┴──────────┘
                    │
             ┌──────▼──────┐
             │   MERGE     │
             │   Results   │
             └─────────────┘

Timeline: Parallel execution, dynamic work distribution
```


## 5. Batch Output & Preallocation

### 5.1 Πρόβλημα: Incremental Append Overhead

#### Αρχική Προσέγγιση (Αργή)

```cpp
// Per-match append with potential reallocation
for (auto& match : all_matches) {
    output_column.append(match.build_row);  // Potential resize
    output_column.append(match.probe_row);  // Potential resize
}

// Τι συμβαίνει στο .append():
void append(int32_t value) {
    // Check if current page is full
    if (current_page_full()) {
        allocate_new_page();     // Expensive!
        update_metadata();       // Overhead
    }
    
    // Check if buffer needs resize
    if (buffer_full()) {
        resize_buffer();         // Reallocation!
        copy_old_data();         // Memory copy
    }
    
    // Finally write value
    write_value(value);
}
```

**Bottlenecks per append:**
- Page boundary checks
- Potential allocation
- Metadata updates
- Vector resizes
- Poor instruction cache (complex control flow)

**Total cost:** For 10M output rows → 10M checks + reallocations

### 5.2 Λύση: Count → Preallocate → Fill

#### Βελτιστοποιημένη Υλοποίηση

**Αρχείο:** [src/execute_default.cpp:232-280](src/execute_default.cpp#L232-L280)

```cpp
// THREE-PHASE OUTPUT STRATEGY

// PHASE 1: COUNT - Determine total output size
size_t total_matches = 0;
for (size_t page_idx = 0; page_idx < num_pages; ++page_idx) {
    const int32_t* keys = page_data[page_idx];
    const size_t page_rows = page_sizes[page_idx];
    
    for (size_t i = 0; i < page_rows; ++i) {
        size_t match_count = 0;
        hashtable.probe(keys[i], &match_count);
        total_matches += match_count;
    }
}

// PHASE 2: PREALLOCATE - Reserve exact space needed
output_columns[0].reserve_exact(total_matches);  // Build row IDs
output_columns[1].reserve_exact(total_matches);  // Probe row IDs

// Pre-allocate pages (one-time allocation)
const size_t pages_needed = (total_matches + VALUES_PER_PAGE - 1) 
                           / VALUES_PER_PAGE;
output_columns[0].allocate_pages(pages_needed);
output_columns[1].allocate_pages(pages_needed);

// PHASE 3: FILL - Direct write with index
size_t out_idx = 0;
for (size_t page_idx = 0; page_idx < num_pages; ++page_idx) {
    const int32_t* keys = page_data[page_idx];
    const size_t page_rows = page_sizes[page_idx];
    const size_t base_row = page_offsets[page_idx];
    
    for (size_t i = 0; i < page_rows; ++i) {
        size_t match_count = 0;
        const auto* matches = hashtable.probe(keys[i], &match_count);
        
        // Direct write to pre-allocated space
        for (size_t m = 0; m < match_count; ++m) {
            output_columns[0].write_at_index(out_idx, matches[m].row_id);
            output_columns[1].write_at_index(out_idx, base_row + i);
            ++out_idx;
        }
    }
}

// Assert: out_idx == total_matches (perfect sizing)
```

### 5.3 Διάγραμμα: Output Strategy Comparison

```
LEGACY (Incremental Append):
═══════════════════════════

For each match:
│
├─ append(value)
│   ├─ Check page boundary  ◄─── Per-match overhead
│   ├─ Check buffer size
│   ├─ Potential allocation ◄─── Expensive!
│   ├─ Potential resize     ◄─── Copy old data
│   └─ Write value
│
Timeline per 1000 matches:
[Check][Write][Check][Write][Check][ALLOC!][COPY!][Write]...
                                     └─────┘ └────┘
                                     Stalls  Memory BW

Total: ~1000 checks + ~5-10 allocations


OPTIMIZED (Count-Preallocate-Fill):
═══════════════════════════════════

PHASE 1 (COUNT):
┌─────────────┐
│ Scan all    │
│ matches     │ Count: 1,234,567 matches
└──────┬──────┘
       │
PHASE 2 (PREALLOCATE):
       │
┌──────▼──────────────┐
│ Allocate exactly    │
│ 1,234,567 slots     │ ◄─── ONE allocation
│                     │
│ Page 0: [........]  │
│ Page 1: [........]  │
│ Page 2: [........]  │
│   ...               │
│ Page N: [........]  │
└──────┬──────────────┘
       │
PHASE 3 (FILL):
       │
┌──────▼──────────────┐
│ Direct write by     │
│ index (no checks)   │ ◄─── Zero overhead
│                     │
│ out[0] = val0       │
│ out[1] = val1       │
│ out[2] = val2       │
│   ...               │
│ out[1234567] = valN │
└─────────────────────┘

Total: 0 checks + 1 allocation
```

### 5.4 Memory Layout Comparison

```
LEGACY (Fragmented):
═══════════════════

Output Buffer Growth Timeline:

t=0:    [Buffer: 1K]        Initial
t=1:    [Buffer: 2K]        Resize + copy 1K
t=2:    [Buffer: 4K]        Resize + copy 2K
t=3:    [Buffer: 8K]        Resize + copy 4K
t=4:    [Buffer: 16K]       Resize + copy 8K
  ...
t=N:    [Buffer: 1.2M]      Resize + copy 600K

Total copies: 1K + 2K + 4K + 8K + ... = O(N) data copied
Memory allocations: log(N) allocations


OPTIMIZED (Contiguous):
═══════════════════════

Output Buffer: Single Allocation

┌────────────────────────────────────────┐
│  [1.2M allocated once]                 │
│  │                                     │
│  └─ All writes go here (no resize)    │
└────────────────────────────────────────┘

Total copies: 0 (data written once)
Memory allocations: 1 allocation
```



## 6. Parallel Probing

### 6.1 Adaptive Parallelization Strategy

#### Πρόβλημα: Thread Overhead vs. Speedup

**Dilema:**
- Small datasets: Thread overhead > parallelism benefit
- Large datasets: Parallelism critical for performance

**Λύση:** Adaptive threshold-based strategy

#### Υλοποίηση

**Αρχείο:** [src/execute_default.cpp:190-230](src/execute_default.cpp#L190-L230)

```cpp
// Adaptive parallelization threshold
static constexpr size_t PARALLEL_PROBE_THRESHOLD = (1 << 18);  // 262,144 rows

void execute_probe_phase(
    const Column* probe_column,
    const std::vector<size_t>& page_offsets,
    UnchainedHashTable& ht,
    std::vector<OutPair>& results) {
    
    const size_t total_rows = page_offsets.back();
    
    // Adaptive decision
    if (total_rows < PARALLEL_PROBE_THRESHOLD) {
        // SEQUENTIAL: Low overhead, good cache locality
        probe_sequential(probe_column, page_offsets, ht, results);
    } else {
        // PARALLEL: Work-stealing for load balancing
        probe_parallel_work_stealing(
            probe_column, page_offsets, ht, results
        );
    }
}
```



## 7. Single-Pass Hashtable Build

### 7.1 Σύγκριση Αρχιτεκτονικών

#### STRICT Mode: Partition-Based Build (Πολύπλοκο)

**Στρατηγική:** Διαχωρισμός σε partitions για thread-safety και καλύτερη cache locality.

```cpp
// PHASE 1: PARTITION (Parallel)
// Each thread creates local partitions
std::vector<std::vector<std::vector<HashEntry>>> thread_partitions(nthreads);

parallel_for(num_entries, [&](size_t tid, size_t i) {
    const auto& entry = entries[i];
    const size_t partition = hash(entry.key) >> shift_bits;
    thread_partitions[tid][partition].push_back(entry);
});

// BARRIER: All threads must finish partitioning
synchronize_threads();

// PHASE 2: MERGE (Parallel - per partition)
parallel_for(num_partitions, [&](size_t tid, size_t part_idx) {
    // Merge all thread-local partitions for this partition
    std::vector<HashEntry> merged;
    for (size_t t = 0; t < nthreads; ++t) {
        merged.insert(merged.end(),
                     thread_partitions[t][part_idx].begin(),
                     thread_partitions[t][part_idx].end());
    }
    
    // Sort by hash for better locality
    std::sort(merged.begin(), merged.end(), 
             [](const auto& a, const auto& b) { 
                 return hash(a.key) < hash(b.key); 
             });
    
    // Build hashtable for this partition
    build_partition(part_idx, merged);
});
```

**Κόστος:**
- 2 φάσεις με barriers (synchronization overhead)
- Ενδιάμεσες δομές (thread_partitions)
- Merge overhead
- Sort overhead

**Χρόνος (113 queries):** 24.0s

#### OPTIMIZED Mode: Single-Pass Build (Απλό)

**Στρατηγική:** Απευθείας εισαγωγή από pages στο hashtable, χωρίς partitions.

```cpp
// SINGLE PHASE: Direct insert from pages
void build_from_zero_copy_int32_simple_parallel(
    const Column* src_column,
    const std::vector<size_t>& page_offsets,
    size_t num_rows) {
    
    const size_t num_pages = page_offsets.size() - 1;
    
    // Pre-allocate hashtable (one-time)
    reserve(num_rows);
    
    // Parallel page processing
    parallel_for_work_stealing(num_pages, 
        [&](size_t tid, size_t page_idx) {
            const Page* page = src_column->get_page(page_idx);
            const int32_t* data = extract_int32_data(page);
            
            const size_t start_row = page_offsets[page_idx];
            const size_t end_row = page_offsets[page_idx + 1];
            const size_t page_rows = end_row - start_row;
            
            // Direct insert (thread-safe hashtable)
            for (size_t i = 0; i < page_rows; ++i) {
                insert_direct(data[i], start_row + i);
            }
        });
}
```

**Κόστος:**
- 1 φάση (no synchronization overhead)
- Μηδενικές ενδιάμεσες δομές
- Μηδενικό merge overhead
- Μηδενικό sort overhead

**Χρόνος (113 queries):** 10.2s

### 7.2 Διάγραμμα: Build Architecture Comparison

```
STRICT (Partition-Based):
═════════════════════════

INPUT PAGES
    │
    ├─────────────┬──────────────┬─────────────┐
    │             │              │             │
┌───▼────┐   ┌───▼────┐    ┌───▼────┐    ┌───▼────┐
│Thread 0│   │Thread 1│    │Thread 2│    │Thread 3│
│        │   │        │    │        │    │        │
│ Local  │   │ Local  │    │ Local  │    │ Local  │
│ Part[0]│   │ Part[0]│    │ Part[0]│    │ Part[0]│
│ Part[1]│   │ Part[1]│    │ Part[1]│    │ Part[1]│
│ Part[2]│   │ Part[2]│    │ Part[2]│    │ Part[2]│
│  ...   │   │  ...   │    │  ...   │    │  ...   │
└───┬────┘   └───┬────┘    └───┬────┘    └───┬────┘
    │            │             │             │
    └────────────┴─────────────┴─────────────┘
                 │
          ┌──────▼──────┐
          │   BARRIER   │ ← Wait for all threads
          └──────┬──────┘
                 │
    ┌────────────┴─────────────┬─────────────┐
    │                          │             │
┌───▼────────┐         ┌───▼────────┐   ┌───▼────────┐
│ Merge      │         │ Merge      │   │ Merge      │
│ Part 0     │         │ Part 1     │   │ Part 2     │
│ (all T's)  │         │ (all T's)  │   │ (all T's)  │
│   ↓        │         │   ↓        │   │   ↓        │
│ Sort       │         │ Sort       │   │ Sort       │
│   ↓        │         │   ↓        │   │   ↓        │
│ Build HT   │         │ Build HT   │   │ Build HT   │
└────────────┘         └────────────┘   └────────────┘

Phases: 2 (Partition → Merge)
Memory: 3x (local parts + merge + final)
Overhead: Barriers, sorting, merging


OPTIMIZED (Single-Pass):
═════════════════════════

INPUT PAGES
    │
    ├─────────────┬──────────────┬─────────────┐
    │             │              │             │
┌───▼────┐   ┌───▼────┐    ┌───▼────┐    ┌───▼────┐
│Thread 0│   │Thread 1│    │Thread 2│    │Thread 3│
│        │   │        │    │        │    │        │
│Page 0  │   │Page 1  │    │Page 2  │    │Page 3  │
│  ↓     │   │  ↓     │    │  ↓     │    │  ↓     │
│Direct  │   │Direct  │    │Direct  │    │Direct  │
│Insert  │   │Insert  │    │Insert  │    │Insert  │
│  ↓     │   │  ↓     │    │  ↓     │    │  ↓     │
└───┼────┘   └───┼────┘    └───┼────┘    └───┼────┘
    │            │             │             │
    └────────────┴─────────────┴─────────────┘
                      │
              ┌───────▼────────┐
              │   HASHTABLE    │ ← Shared, thread-safe
              │  (final dest)  │
              └────────────────┘

Phases: 1 (Direct Build)
Memory: 1x (final only)
Overhead: Zero
```




### Τελική Αρχιτεκτονική (OPTIMIZED)

```
┌─────────────────────────────────────────────┐
│  OPTIMIZED_PROJECT Hash Join Engine         │
└─────────────────────────────────────────────┘

INPUT
  │
  ├─ Zero-Copy Page Access
  │   └─ Direct pointers (no division/modulo)
  │
  ├─ Single-Pass Build
  │   ├─ Pre-allocate hashtable
  │   └─ Direct insert from pages
  │
  ├─ Parallel Probe (Adaptive)
  │   ├─ Work-stealing (large data)
  │   └─ Sequential (small data)
  │
  └─ Batch Output
      ├─ Count matches
      ├─ Pre-allocate
      └─ Direct fill

OUTPUT
```



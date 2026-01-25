# 📖 Εργασία: Ανάπτυξη Λογισμικού για Πληροφοριακά Συστήματα (3ο Μέρος)

[![Review Assignment Due Date](https://classroom.github.com/assets/deadline-readme-button-22041afd0340ce965d47ae6ef1cefeee28c7c493a6346c4f15d667ab976d596c.svg)](https://classroom.github.com/a/gjaw_qSU)

[![Build Status](https://github.com/uoa-k23a/k23a-2025-d1-runtimeerror/actions/workflows/software_tester.yml/badge.svg)](https://github.com/uoa-k23a/k23a-2025-d1-runtimeerror/actions/workflows/software_tester.yml)

## 👥 Μέλη Ομάδας

* **Ξενοφών Λογοθέτης** - sdi2100087@di.uoa.gr - `1115202100087`
* **Σακκέτος Γεώργιος** - sdi2000177@di.uoa.gr - `1115202000177`
* **Φωτιάδης Ευάγγελος** - sdi1900301@di.uoa.gr - `1115201900301`

---

## Build

```bash
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release -Wno-dev
cmake --build build -- -j $(nproc) fast
```

## Run 

### OPTIMIZED Mode (Ταχύτητα)
```bash
./build/fast plans.json
```

### STRICT Mode (Απαιτήσεις Διαγωνισμού)
```bash
STRICT_PROJECT=1 ./build/fast plans.json
```

### Με Telemetry
```bash
JOIN_TELEMETRY=1 ./build/fast plans.json
```

## Unit Tests

```bash
cmake --build build --target software_tester -- -j && ./build/software_tester --reporter compact
```



## Υλοποιήσεις

### ΠΑΡΑΔΟΤΕΟ 1: Hash Table Optimizations

Αντικατάσταση της std::unordered_map με τρεις optimized hash table υλοποιήσεις:

**Robin Hood Hashing**
- **Τι κάνει**: Balanced Probe Sequence Length (PSL) — διατηρεί ισορροπία μεταξύ των probe sequences
- Όταν ένα νέο entry έχει μεγαλύτερο PSL από υπάρχον, τα σωματά ανταλλάσσονται θέσης
- Linear probing με O(1) average lookup, O(log n) worst case
- Καλύτερη worst-case performance από chained hashtables (κανένα linked list)
- **Αποτέλεσμα**: 4.0% improvement (242.85s → 233.25s)
- **Σχέση με STRICT/OPTIMIZED**: Το OPTIMIZED mode χρησιμοποιεί unchained αντί για RobinHood
- **Αρχεία**: `include/robinhood_hashtable.h`, `src/robinhood.cpp`

**Cuckoo Hashing**
- **Τι κάνει**: Χρησιμοποιεί δύο ανεξάρτητες hash functions h1() και h2()
- Όταν collision: το νέο entry τοποθετείται στο h2(key), και το παλιό "κλωτσάται" στο h1(key)
- Guaranteed O(1) lookup — οποιοδήποτε key είναι μόνο 2 θέσεις μακριά
- **Πρόβλημα**: Μπορεί να δημιουργηθούν infinite cycles — χρειάζεται rehashing
- **Αποτέλεσμα**: 2.6% improvement
- **Αρχεία**: `include/cuckoo_hashtable.h`, `src/cuckoo.cpp`

**Hopscotch Hashing**
- **Τι κάνει**: Hybrid ανάμεσα σε open addressing και chaining με controlled overflow
- Κάθε bucket διατηρεί μία bitmap που δείχνει που είναι τα items του κοντά (max 32 άλτη)
- Προσφέρει καλή cache locality επειδή όλα τα items ενός bucket είναι contiguous
- **Αποτέλεσμα**: 2.0% improvement
- **Αρχεία**: `include/hopscotch_hashtable.h`, `src/hopscotch.cpp`

**Unchained Hashtable (Χρησιμοποιείται στο Project)**
- **Τι κάνει**: Flat storage χωρίς chains — όλα τα tuples σε έναν μεγάλο contiguous array
- Directory structure: prefix-based partitioning (κάθε hash prefix δείχνει ένα range tuples)
- 16-bit bloom filters ανά partition για γρήγορη rejection
- **Σχέση με STRICT**: STRICT mode χρησιμοποιεί 64 partitions + thread-safe parallel build
- **Σχέση με OPTIMIZED**: OPTIMIZED mode χρησιμοποιεί single-pass unchained χωρίς partitions
- **Αποτέλεσμα**: 28.3% improvement (από 46.12s → 27.24s)
- **Αρχεία**: `include/unchained_hashtable.h`, `include/parallel_unchained_hashtable.h`

---

### ΠΑΡΑΔΟΤΕΟ 2: Column-Store & Late Materialization

Μετάβαση από row-oriented σε column-oriented storage με partial materialization:

**Δομή Αποθήκευσης (ColumnBuffer)**
- **INT32 στήλες**: Contiguous arrays σε σελίδες (1024 values/page)
  - Direct indexing: `column[i]` = O(1) — απλό array access
  - Καμία indirection, καμία pointer chasing
  - Cache-friendly: sequential access patterns
- **VARCHAR στήλες**: Κατάσταση (indirect references, όχι πλήρες strings)
  - `PackedStringRef`: 64-bit value = {table_id, column_id, page_id, slot}
  - Αποφεύγουμε αντιγραφή ολόκληρων strings μέχρι το τελικό output
  - Σημαντική εξοικονόμηση memory allocations
- **Διαχωρισμός δεδομένων**:
  - Build-phase data: μόνο τα tuples που χρειάζονται για join (κλειδιά + row_ids)
  - Output data: πλήρεις σειρές με όλα τα στοιχεία που ζητήθηκαν
- **Αποτέλεσμα**: 43.5% improvement (132.53s → 64.33s)

**Late Materialization σε Βάθος**
- **Build Phase**: Μόνο hash table χτίζεται (με κλειδιά και row_ids)
  - VARCHAR στήλες δεν μετακινούνται
  - Tuples όχι έτοιμα ακόμα στο output format
- **Probe Phase**: Δημιουργούμε OutPair (left_idx, right_idx) χωρίς υλικοποίηση
  - Κάθε probe thread συγκεντρώνει ζευγάρια σειρών
  - 0 allocations για strings κατά αυτή τη φάση
- **Output Materialization**: 
  - Τελευταία φάση: γράφουμε ακριβώς τα στοιχεία που χρειάζεται το output schema
  - Δεν υλικοποιούμε όλα τα attributes από τις πηγές
  - Προ-δέσμευση ακριβώς του σωστού αριθμού σελίδων
- **Cumulative Impact**: 51.4% improvement από baseline (242.85s → 119.6s)

**Αρχεία Υλοποίησης**
- `include/column_store.h` - Column storage interface
- `include/columnar.h` - ColumnBuffer definition (pages, offsets, caches)
- `src/columnar.cpp` - Column data management
- `src/execute_default.cpp` - Materialization logic (lines 220-320)
  - `OutPair` structure για ζευγάρια σειρών
  - Batch output preallocation
  - Per-thread local buffers για cache efficiency

---

### ΠΑΡΑΔΟΤΕΟ 3: Parallel Execution & Zero-Copy

Παραλληλοποίηση με zero-copy access patterns και advanced optimizations:

**Zero-Copy INT32 Indexing **
- **Σκοπός**: Αποφυγή αντιγραφής δεδομένων κατά τη φάση build
- **Μηχανισμός**:
  - Κάθε column αποθηκεύει σελίδες (pages) με offsets
  - Αντί να αντιγράψουμε: `entries[i] = {value, row_id}`
  - Χρησιμοποιούμε: `table->build_from_zero_copy_int32(src_column, page_offsets)`
  - Το hash table αναφέρει απευθείας σε σελίδες εισόδου
- **Προϋποθέσεις**:
  - Ισχύει μόνο για INT32 χωρίς NULL values
  - Το `is_zero_copy` flag ελέγχει αν είναι δυνατό
- **Εξοικονόμηση**: ~40% memory για build phase (δεν χρειάζονται ενδιάμεσα entries vectors)
- **Αποτέλεσμα**: 40.9% improvement (46.12s → 27.24s)

**Partition-Based Build (STRICT Mode)**
- **Σκοπός**: Thread-safe parallel build χωρίς locks
- **Αρχιτεκτονική**:
  - Χωρίζουμε το hash table σε 64 partitions
  - Κάθε partition ανήκει σε ένα thread (one-writer)
  - Phase 1: Κάθε thread χτίζει τοπικές λίστες (chunk lists) ανα partition
  - Phase 2: One-writer-per-partition: κάθε partition γράφεται από ένα thread
  - Phase 3: Blooms και offsets υπολογίζονται σε parallel per-partition
- **3-Level Slab Allocator**:
  - Level 1: Global allocator (operator new)
  - Level 2: Thread-local SlabAllocator (per thread, μεγάλα blocks)
  - Level 3: Chunk lists (μικρά blocks από thread-local allocator)
  - Αποφυγή contention και false sharing


**Work-Stealing Load Balancing (Probe Phase)**
- **Σκοπός**: Δυναμική κατανομή δουλειάς όταν κάποιες queries είναι βαρύτερες
- **Μηχανισμός**:
  - `WorkStealingCoordinator`: Διατηρεί κοινή λίστα work blocks
  - Κάθε thread παίρνει ένα block (minimum 256 rows, υπολογίζεται δυναμικά)
  - Όταν ένας thread τελειώνει: κλέβει το επόμενο block από τη λίστα
  - Adaptive parallelization: nthreads = (probe_n >= 2^18) ? hw : 1
- **Αποφυγή**:
  - Load imbalance: κανένα thread δεν περιμένει άλλα
  - False sharing: blocks είναι ανεξάρτητα
- **Αρχεία**: `include/work_stealing.h`, `src/work_stealing.cpp`


**Αρχεία Υλοποίησης**
- `include/unchained_hashtable.h` 
- `include/parallel_unchained_hashtable.h` 
- `src/execute_default.cpp` 
- `partition_hash_builder.h` - Parallel build με partitions
- `src/work_stealing.cpp` - Load balancing coordinator
 - `include/slab_allocator.h` - 3-level slab allocator (thread + per-partition)

---

### ADDITIONAL_IMPLEMENTATIONS: STRICT vs OPTIMIZED Modes

Δύο ολοκληρωμένες εκδόσεις με διαφορετικά trade-offs:

**STRICT_PROJECT Mode (Απαιτήσεις Διαγωνισμού)**
- **Στόχος**: Υλοποίηση όλων των requirements από την εκφώνηση
- **Hash Table**: Partition-based unchained (64 partitions - optimal)
- **Build Phase**:
  - Phase 1: Parallel partitioning με local chunk lists
  - Phase 2: One-writer-per-partition — κάθε partition γράφεται από έναν thread
  - Phase 3: Bloom filter συγχώνευση (per-partition)
- **Compliance**:
  - REQ-1: Unchained hashtable με flat storage
  - REQ-2: Column-oriented με late materialization
  - REQ-3: Parallelization (64 partitions, work-stealing)
  - REQ-4: Zero-copy INT32 indexing
  - REQ-6: Partition-based parallel build με 3-level slab allocator
  - REQ-8.2: Directory-based lookup με END pointers
  - REQ-8.3: Directory[-1] support για special cases
- **Memory**: 3.89 GB peak (includes 3.6 GB CSV data, 64 partitions)
- **Query Runtime**: 32.4s (optimal configuration)
- **Wall Time**: 80.3s (includes I/O)
- **Αρχεία**: `src/execute_default.cpp` (κύρια υλοποίηση)
- **Εκτέλεση**: `STRICT_PROJECT=1 ./build/fast plans.json`

**OPTIMIZED_PROJECT Mode (Μέγιστη Ταχύτητα)**
- **Στόχος**: Ταχύτερη εκτέλεση από STRICT (trade-off ακρίβειας)
- **Hash Table**: Single-pass unchained (χωρίς partitions)
  - Direct page pointers → κανένας overhead division
  - Continuous tuple array → κανένα indirection
  - Batch preallocation → minimal allocations
- **Build Phase**:
  - Απλή 5-phase algorithm (count, prefix sum, allocate, copy, bloom)
  - Χωρίς partition synchronization overhead
  - Zero thread coordination
- **Probe Phase**:
  - Adaptive parallelization (περισσότεροι threads μόνο για μεγάλα inputs)
  - Work-stealing όπου χρειάζεται
- **Output Phase**:
  - Single-threaded (ως προαιρετικό βήμα παραλληλοποίησης)
  - Batch preallocation χωρίς per-row overhead
- **Memory**: 4.34 GB peak (includes 3.6 GB CSV data)
- **Query Runtime**: 12.1s (optimal 4 threads)
- **Wall Time**: 59.4s (includes I/O)
- **Ταχύτητα σχετικά με STRICT**: 3.19x faster (38.6s → 12.1s)
- **Αρχεία**: `src/execute_default.cpp` (env-based mode selection)
- **Εκτέλεση**: `./build/fast plans.json` (προεπιλογή)

**Σύγκριση Απευθείας (Πραγματικές Μετρήσεις)**
| Μέτρηση | STRICT | OPTIMIZED | Λόγος |
|---------|--------|-----------|-------|
| Query Runtime | 32.4s | 12.1s | 2.68x |
| Wall Time (Total) | 80.3s | 59.4s | 1.35x |
| CPU Time (User+Sys) | 98.4s | 62.6s | 1.57x |
| Peak Memory | 3.89 GB | 4.34 GB | 1.12x |
| Correctness | 100% | 100% | ✓ |

---

## 🖥️ Περιβάλλον Πειραμάτων

**Hardware Specifications**
- **CPU**: AMD/Intel Multi-core (hardware_concurrency detected)
- **RAM**: 8+ GB (tested with IMDB ~3.6 GB dataset)
- **Storage**: SSD recommended (CSV loading is I/O intensive)
- **OS**: Linux (tested on Ubuntu/Debian)
- **Compiler**: GCC/Clang with -O3 optimization

**Dataset**
- **Source**: IMDB Job Benchmark
- **Size**: ~3.6 GB (CSV files loaded into memory)
- **Queries**: 33 join queries (plans.json)
- **Complexity**: Multi-table joins with selective predicates

---

### MEASUREMENTS: Performance Analysis & Optimization Path

**Optimization History: 7 Iterations Προς OPTIMIZED**
Το project ακολούθησε ένα συστηματικό δρόμο βελτιστοποίησης (τελικό runtime: 12.1s):

| Iteration | Τεχνική | Runtime | Improvement | Cumulative |
|-----------|---------|---------|-------------|-----------|
| 0 | std::unordered_map (Baseline) | 242.85s | — | — |
| 1A | Robin Hood Hashing | 233.25s | 4.0% | 4.0% |
| 2 | Column-Store | 132.53s | 43.5% | 45.4% |
| 3 | Late Materialization | 64.33s | 51.4% | 73.5% |
| 4 | Unchained Hashtable | 46.12s | 28.3% | 81.0% |
| 5 | Zero-Copy INT32 | 27.24s | 40.9% | 88.8% |
| 6 | STRICT Mode Final | 38.6s | +78% (vs #6) | 84.1% |
| 7 | OPTIMIZED Mode | 12.1s | 68.7% (vs #7) | 95.0% |


### 📊 Πειραματική Ανάλυση Ανά Παράμετρο

**Experiment 1: Hash Table Structures (Impact on Build Phase)**

Σύγκριση διαφορετικών hash table implementations για το ίδιο workload (μόνο συνολικός χρόνος και μνήμη):

| Hash Table | Total Time | Memory |
|-----------|-----------|--------|
| std::unordered_map | 242.8s | ~900 MB |
| Robin Hood | 233.2s | ~850 MB |
| Cuckoo | 237.2s | ~870 MB |
| Hopscotch | 238.0s | ~880 MB |
| Unchained HT + Column + Late | 46.12s | ~410 MB |
| Robin Hood (OPTIMIZED) | 37.914s | 
| Cuckoo (OPTIMIZED) | 36.163s | 
| Hopscotch (OPTIMIZED)| 38.670s | 
| Unchained (OPTIMIZED) | 13.2s | 234 MB |


**Παρατηρήσεις:**
- Το unchained hashtable με zero-copy είναι **18x ταχύτερο** από std::unordered_map
- Robin Hood δίνει μόνο 4% βελτίωση (limited by chaining overhead)
- Το STRICT mode trade-off: +161% χρόνος για 100% compliance

---

**Experiment 2: Column-Store vs Row-Store (Storage Layout)**

Επίδραση του column-oriented storage στην απόδοση (σύνολο χρόνου μόνο):

| Storage Layout | Total |
|---------------|-------|
| Row-oriented (baseline) | 292.6s |
| Column-oriented | 132.5s |
| + Late Materialization | 64.3s |
| + Zero-Copy INT32 | 27.2s |

**Κέρδος column-store:**
- **54.7%** μείωση χρόνου (292.6s → 132.5s)
- Late materialization: επιπλέον **51.4%** βελτίωση
- Zero-copy: τελικό **90.7%** βελτίωση συνολικά

---

**Experiment 3: Parallelization (Thread Scaling)**

Επίδραση του αριθμού threads στην απόδοση (OPTIMIZED mode):

| Threads | Total Runtime | Speedup vs 1T | Efficiency | Wall Time |
|---------|--------------|---------------|-----------|-----------|
| 1 | 18.3s | 1.00x | 100% | 68.7s |
| 2 | 14.7s | 1.24x | 62.0% | 61.4s |
| 4 | 12.1s | 1.51x | 37.8% | 59.4s |
| 8 | 12.2s | 1.50x | 18.8% | 61.3s |
| 12 | 12.4s | 1.48x | 12.3% | 61.9s |
| 20 | 12.8s | 1.43x | 7.2% | 62.0s |

**Παρατηρήσεις:**
- Optimal: **4 threads** (12.1s runtime)
- Diminishing returns μετά τα 4 threads (I/O bound workload)
- Το wall time παραμένει ~60s λόγω CSV parsing και I/O overhead
- Η διαφορά στο query execution runtime είναι το parallelization κέρδος

---

**Experiment 4: Partition Count (STRICT mode)**

Πειράματα με διαφορετικό αριθμό partitions:

| Partitions | Query Runtime | Wall Time | Memory | Trade-off |
|-----------|--------------|-----------|--------|----------|
| 16 | 34.6s | 84.6s | 4.29 GB | High contention |
| **64** | **32.4s** | **80.3s** | **3.89 GB** | **✓ Optimal** |
| 128 | 35.3s | 84.0s | 4.06 GB | Overhead |
| 256 | 35.8s | 85.9s | 4.22 GB | High overhead |

**Παρατηρήσεις:**
- **Optimal: 64 partitions** - best balance of contention vs overhead
- 16 partitions: High contention on shared chunk lists
- 128+ partitions: Diminishing returns, memory overhead increases
- **3.3s faster** than 256 partitions (10% performance gain)
- **9% memory savings** vs 256 partitions

---

**Experiment 5: Memory Footprint Breakdown**

Πραγματικές μετρήσεις μνήμης (peak RSS) για IMDB workload:

**OPTIMIZED Mode:**
- **Peak Memory**: 4.34 GB (4,442 MB)
- **Query Runtime**: 12.1s (4 threads)
- **Wall Time**: 59.4s (including I/O)
- **CPU Time**: 62.6s total

**STRICT Mode:**
- **Peak Memory**: 3.89 GB (3,990 MB)  
- **Query Runtime**: 32.4s
- **Wall Time**: 80.3s
- **CPU Time**: 98.4s total

**Διαφορά:** 
- Memory: OPTIMIZED χρησιμοποιεί +11.6% περισσότερη μνήμη (4.34 vs 3.89 GB)
- Runtime: STRICT είναι +168% πιο αργό (32.4s vs 12.1s)
- Wall Time: STRICT είναι +35% πιο αργό (80.3s vs 59.4s)

**Σημείωση:** Η peak μνήμη περιλαμβάνει τα loaded CSV data (~3.6 GB) που είναι κοινά και στα δύο modes. Η διαφορά στην join execution structure είναι μικρή σε μνήμη αλλά μεγάλη σε χρόνο.

---



## Βασικά Αρχεία Υλοποίησης

### Header Files (`include/`)

**Core Execution & Configuration**
- `project_config.h` - Ρύθμιση modes (STRICT/OPTIMIZED/JOIN_TELEMETRY)
- `hashtable_interface.h` - Interface για όλες τις hash table υλοποιήσεις
- `hash_functions.h` - Hash functions 
- `hash_common.h` - Κοινές δομές και constants

**Column Storage & Data Management**
- `columnar.h` - ColumnBuffer definition (pages, offsets, caches)
- `inner_column.h` - Εσωτερική αναπαράσταση στηλών
- `table.h` - Table structure και metadata
- `table_entity.h` - Table entity definitions
- `attribute.h` - Attribute metadata
- `late_materialization.h` - Late materialization helpers

**Hash Table Implementations**
- `unchained_hashtable.h` - Βασική unchained HT (single-pass)
- `parallel_unchained_hashtable.h` - Partition-based unchained (STRICT mode)
- `partition_hash_builder.h` - Parallel build με partitions
- `robinhood.h` + `robinhood_wrapper.h` - Robin Hood hashing
- `cuckoo.h` + `cuckoo_wrapper.h` - Cuckoo hashing
- `hopscotch.h` + `hopscotch_wrapper.h` - Hopscotch hashing
- `unchained_hashtable_wrapper.h` - Wrapper για unchained (production)
- `cuckoo_map.h` - Alternative cuckoo implementation

**Optimization Techniques**
- `bloom_filter.h` - Bloom filter για pre-filtering
- `work_stealing.h` - Work-stealing load balancing
- `slab_allocator.h` - 3-level slab allocator (STRICT mode)
- `join_telemetry.h` - Performance telemetry

**Infrastructure**
- `plan.h` - Query plan structures
- `statement.h` - SQL statement parsing
- `csv_parser.h` - CSV data loading
- `hardware.h` - Hardware detection
- `common.h` - Common utilities

### Source Files (`src/`)
- `execute_default.cpp` - Κύρια join execution (και τα δύο modes)
- `columnar.cpp` - Column data management
- `work_stealing.cpp` - Load balancing coordinator
- `slab_allocator.cpp` - Slab allocator implementation
- `late_materialization.cpp` - Materialization logic
- `join_telemetry.cpp` - Telemetry tracking & reporting
- `build_table.cpp` - Table construction
- `statement.cpp` - Statement processing
- `csv_parser.cpp` - CSV parsing

### Documentation (`docs/`)
- `PARADOTEO_1.md` - Hash table analysis & Robin Hood details
- `PARADOTEO_2.md` - Column-store & late materialization
- `PARADOTEO_3.md` - Parallel execution & zero-copy
- `ADDITIONAL_IMPLEMENTATIONS.md` - OPTIMIZED optimizations

Δείτε τα σχετικά `.md` αρχεία για πλήρες τεχνικό background.
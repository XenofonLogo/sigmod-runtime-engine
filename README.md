# 📖 Εργασία: Ανάπτυξη Λογισμικού για Πληροφοριακά Συστήματα (2ο Μέρος)

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

### STRICT Mode (Απαιτήσεις Διαγωνισμού) - Προεπιλογή
```bash
./build/fast plans.json
```

### OPTIMIZED Mode (Ταχύτητα)
```bash
OPTIMIZED_PROJECT=1 ./build/fast plans.json
```

### Με Telemetry
```bash
JOIN_TELEMETRY=1 ./build/fast plans.json
```

## Unit Tests

```bash
cmake --build build --target software_tester -- -j && ./build/software_tester --reporter compact
```

---

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
- **Σχέση με STRICT**: STRICT mode χρησιμοποιεί 256 partitions + thread-safe parallel build
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

**Zero-Copy INT32 Indexing (REQ-4)**
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

**Partition-Based Build (STRICT Mode, REQ-6)**
- **Σκοπός**: Thread-safe parallel build χωρίς locks
- **Αρχιτεκτονική**:
  - Χωρίζουμε το hash table σε 256 partitions
  - Κάθε partition ανήκει σε ένα thread (one-writer)
  - Phase 1: Κάθε thread χτίζει τοπικές λίστες (chunk lists) ανα partition
  - Phase 2: One-writer-per-partition: κάθε partition γράφεται από ένα thread
  - Phase 3: Blooms και offsets υπολογίζονται σε parallel per-partition
- **3-Level Slab Allocator**:
  - Level 1: Global allocator (operator new)
  - Level 2: Thread-local TempAlloc (per thread, μεγάλα blocks)
  - Level 3: Chunk lists (μικρά blocks από thread-local allocator)
  - Αποφυγή contention και false sharing
- **Αποτέλεσμα**: 20.4% improvement (27.24s → 21.68s)

**Work-Stealing Load Balancing (Probe Phase)**
- **Σκοπός**: Δυναμική κατανομή δουλειάς όταν κάποιες queries είναι βαρύτερες
- **Μηχανισμός**:
  - `WorkStealingCoordinator`: Διατηρεί κοινή λίστα work blocks
  - Κάθε thread παίρνει ένα block (π.χ., 256 rows)
  - Όταν ένας thread τελειώνει: κλέβει το επόμενο block από τη λίστα
  - Adaptive parallelization: nthreads = (probe_n >= 2^18) ? hw : 1
- **Αποφυγή**:
  - Load imbalance: κανένα thread δεν περιμένει άλλα
  - False sharing: blocks είναι ανεξάρτητα
- **Αρχεία**: `include/work_stealing.h`, `src/work_stealing.cpp`

**Bloom Filters (16-bit per partition)**
- **Τι κάνει**: Πρώιμη απόρριψη (early rejection) κατά το probe
- **Δομή**: 
  - Μέρος του directory (όχι ξεχωριστή allocation)
  - `make_tag_from_hash()`: 4 bits από 4 διαφορετικές θέσεις του hash
  - `maybe_contains()`: AND έλεγχος — αν δεν υπάρχουν, σίγουρα missing
- **Lợi ích**:
  - STRICT: ~14% improvement (34.5s vs 39.4s χωρίς bloom)
  - Μια απλή AND πράξη αποφεύγει ακριβή hash table probes
- **Αρχεία**: `include/bloom_filter.h` — ενσωματωμένα στο `parallel_unchained_hashtable.h`

**Αρχεία Υλοποίησης**
- `include/unchained_hashtable.h` - Unchained HT interface
- `include/parallel_unchained_hashtable.h` - STRICT mode implementation (partitioned + bloom)
- `src/execute_default.cpp` - Zero-copy logic (lines 47-100) + work-stealing (lines 115-230)
- `src/hashtable_builder.cpp` - Partition-based building
- `src/work_stealing.cpp` - Load balancing coordinator
- `include/bloom_filter.h` - Bloom filter helpers
- `include/three_level_slab.h` - Thread-safe memory allocator

---

### ADDITIONAL_IMPLEMENTATIONS: STRICT vs OPTIMIZED Modes

Δύο ολοκληρωμένες εκδόσεις με διαφορετικά trade-offs:

**STRICT_PROJECT Mode (Απαιτήσεις Διαγωνισμού)**
- **Στόχος**: Πληρέστε όλες τις 7 requirements από την εκφώνηση
- **Hash Table**: Partition-based unchained (256 partitions)
- **Build Phase**:
  - Phase 1: Parallel partitioning με local chunk lists
  - Phase 2: One-writer-per-partition — κάθε partition γράφεται από έναν thread
  - Phase 3: Bloom filter συγχώνευση (per-partition)
- **Compliance**:
  - REQ-1: Unchained hashtable με flat storage
  - REQ-2: Column-oriented με late materialization
  - REQ-3: Parallelization (256 partitions, work-stealing)
  - REQ-4: Zero-copy INT32 indexing
  - REQ-6: Partition-based parallel build με 3-level slab allocator
  - REQ-8.2: Directory-based lookup με END pointers
  - REQ-8.3: Directory[-1] support για special cases
- **Memory**: 577 MB για IMDB workload
- **Runtime**: 34.5s total (24s build + 8s probe + 2.5s output)
- **Improvement**: 85.3% από baseline (242.85s → 34.5s)
- **Αρχεία**: `src/execute_default.cpp` (κύρια υλοποίηση)
- **Εκτέλεση**: `./build/fast plans.json` (προεπιλογή)

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
- **Memory**: 234 MB για IMDB workload (59% λιγότερα από STRICT)
- **Runtime**: 13.2s total (10.2s build + 1.8s probe + 1.2s output)
- **Improvement**: 95.5% από baseline (242.85s → 13.2s)
- **Ταχύτητα σχετικά με STRICT**: 3.23x faster (34.5s → 13.2s)
- **Αρχεία**: `src/execute_default.cpp` (mode selection στη γραμμή 139-140)
- **Εκτέλεση**: `OPTIMIZED_PROJECT=1 ./build/fast plans.json`

**Σύγκριση Απευθείας**
| Μέτρηση | STRICT | OPTIMIZED | Λόγος |
|---------|--------|-----------|-------|
| Σύνολο Runtime | 34.5s | 13.2s | 3.23x |
| Build Phase | 24.0s | 10.2s | 2.35x |
| Probe Phase | 8.0s | 1.8s | 4.44x |
| Output Phase | 2.5s | 1.2s | 2.08x |
| Memory Usage | 577 MB | 234 MB | 2.47x |
| Correctness | 100% | 100% | ✓ |

---

### MEASUREMENTS: Performance Analysis & Optimization Path

**Optimization History: 8 Iterations Προς OPTIMIZED**
Το project ακολούθησε ένα συστηματικό δρόμο βελτιστοποίησης:

| Iteration | Τεχνική | Runtime | Improvement | Cumulative |
|-----------|---------|---------|-------------|-----------|
| 0 | std::unordered_map (Baseline) | 242.85s | — | — |
| 1A | Robin Hood Hashing | 233.25s | 4.0% | 4.0% |
| 2 | Column-Store | 132.53s | 43.5% | 45.4% |
| 3 | Late Materialization | 64.33s | 51.4% | 73.5% |
| 4 | Unchained Hashtable | 46.12s | 28.3% | 81.0% |
| 5 | Zero-Copy INT32 | 27.24s | 40.9% | 88.8% |
| 6 | Partition-Based Build | 21.68s | 20.4% | 91.1% |
| 7 | STRICT Mode Final | 34.5s | +59% (vs #6) | 85.3% |
| 8 | OPTIMIZED Mode | 11.04s | 49.1% (vs #7) | 95.5% |

*Σημείωση: Η iteration #7 (STRICT) είναι "χειρότερη" επειδή προσθέτει partition overhead για compliance με requirements*

**STRICT vs OPTIMIZED Detailed Comparison**

**Build Phase Analysis (24.0s vs 10.2s)**
- **STRICT**: 
  - Phase 1: Parallel partitioning — 3.2s
  - Phase 2: One-writer-per-partition gather — 18.0s
  - Phase 3: Bloom merging — 2.8s
- **OPTIMIZED**:
  - Direct count (parallel) — 1.5s
  - Prefix sum + allocate — 0.8s
  - Copy with bloom update — 7.9s
- **Λόγος 2.35x**: OPTIMIZED αποφεύγει partition synchronization

**Probe Phase Analysis (8.0s vs 1.8s)**
- **STRICT**: 
  - 256 partitions = μεγαλύτερες range scans
  - Περισσότερες συγκρούσεις → περισσότερες comparisons
- **OPTIMIZED**:
  - Single-pass = μικρότερες range scans
  - Work-stealing = καλή load balancing
  - Bloom filters εξαιρετικά αποτελεσματικές σε single-pass
- **Λόγος 4.44x**: Σημαντικό κέρδος από απλούστερη δομή

**Output Phase Analysis (2.5s vs 1.2s)**
- **STRICT**: Materialization από 577 MB memory footprint
- **OPTIMIZED**: Materialization από 234 MB (60% μείωση)
- **Λόγος 2.08x**: Λιγότερα δεδομένα = γρήγορη materialization

**Root Causes of Optimization (Ανάλυση Συμβολής)**
Μια ανάλυση της συνεισφοράς κάθε βελτιστοποίησης:

| Βελτιστοποίηση | Συνεισφορά |
|---|---|
| Partitioning efficiency (STRICT) | 39% |
| Zero-Copy INT32 access | 18% |
| Data structure optimization (unchained) | 7% |
| Parallelization (work-stealing) | 4% |
| Output optimization (late materialization) | 4% |

**Memory Footprint Comparison**
- **Baseline** (std::unordered_map): ~900 MB
- **STRICT** (partition-based): 577 MB (36% reduction)
- **OPTIMIZED** (single-pass): 234 MB (74% reduction)

**Per-Query Performance**
Οι μικρές queries σε OPTIMIZED τρέχουν sub-millisecond:
- Query 1c: 4ms (STRICT) → 3ms (OPTIMIZED)
- Query 5b: 1ms (STRICT) → 1ms (OPTIMIZED)
- Query 7a: 503ms (STRICT) → 397ms (OPTIMIZED)
- Query 8c: 904ms (STRICT) → 766ms (OPTIMIZED)

**Αρχεία Μετρήσεων**
- `MEASUREMENTS.md` - Detailed performance analysis με per-query metrics
- `PARADOTEO_1.md` - Hash table analysis και Robin Hood vs alternatives
- `PARADOTEO_2.md` - Column-store architecture και late materialization overhead
- `PARADOTEO_3.md` - Parallelization strategy με work-stealing details

---



## Βασικά Αρχεία Υλοποίησης

### Header Files (`include/`)
- `column_store.h` - Column storage interface
- `robinhood_hashtable.h` - Robin Hood hash table
- `cuckoo_hashtable.h` - Cuckoo hashing
- `hopscotch_hashtable.h` - Hopscotch hashing
- `unchained_hashtable.h` - Unchained HT with partitions
- `bloom_filter.h` - Bloom filter για pre-filtering

### Source Files (`src/`)
- `execute_default.cpp` - STRICT mode (partition-based)
- `execute_optimized.cpp` - OPTIMIZED mode (single-pass)
- `column_manager.cpp` - Column data management
- `hashtable_builder.cpp` - Build phase implementation
- `work_stealing.cpp` - Load balancing
- `robinhood.cpp`, `cuckoo.cpp`, `hopscotch.cpp` - Hash table implementations

### Documentation
- `PARADOTEO_1.md` - Hash table analysis & Robin Hood details
- `PARADOTEO_2.md` - Column-store & late materialization
- `PARADOTEO_3.md` - Parallel execution & zero-copy
- `ADDITIONAL_IMPLEMENTATIONS.md` - OPTIMIZED optimizations
- `MEASUREMENTS.md` - Performance measurements
- `ODIGIES_EKTELESHS.md` - Quick execution guide

Δείτε τα σχετικά `.md` αρχεία για πλήρες τεχνικό background.
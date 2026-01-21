## 🌟 ΕΠΙΠΛΕΟΝ ΥΛΟΠΟΙΗΣΕΙΣ (Πέρα από Requirements)

### Κρίσιμες αλλαγές που έριξαν τον χρόνο (μετρημένες)
- **Direct page access αντί για `column.get()`**: Αφαιρεί division/modulo και indirection σε κάθε row· σειριακή ανάγνωση page buffers ρίχνει ~10-11s συνολικά (22.8s → ~12s).
- **Zero-copy build από pages**: `build_from_zero_copy_int32` αποφεύγει materialization/vector copies στο build key· κερδίζει ~1-2s.
- **Zero-copy probe με page cursor**: Per-thread page cursor + άμεση ανάγνωση probe keys χωρίς binary search ανά row· κερδίζει ~1-2s.
- **Batch output (preallocation + direct writes)**: Αντί για per-row `append`, γράφει με indexing σε προδεσμευμένες σελίδες· κερδίζει ~0.5-1s.
- **Global Bloom Filter**: Early reject στο probe μειώνει άχρηστα lookups· ~15-16% επιπλέον (11.04s → 9.54s).

**Συνολικό αποτέλεσμα**: 22.8s (legacy path) → 9.5s (τρέχον default) = ~-58% βελτίωση, επιβεβαιωμένο μετρήσεις.

### Legacy vs Current Path: τι άλλαξε και γιατί είναι ταχύτερο
- **Build phase**
    - Legacy: Materialization σε ενδιάμεσο `vector<HashEntry>` με ανάγνωση μέσω `get(i)` (division/modulo ανά row).
    - Current: Άμεσο χτίσιμο από σελίδες με `build_from_zero_copy_int32` χωρίς αντιγραφή keys και χωρίς ενδιάμεσο vector. Βλ. [src/execute_default.cpp#L320-L377](src/execute_default.cpp#L320-L377).
    - Γιατί κερδίζει: Μηδενίζει μεγάλα copies/allocations και αφαιρεί το per-row κόστος της `get()`.
- **Probe phase**
    - Legacy: Για κάθε `j`, `get(j)` από `ColumnBuffer` (division/modulo + indirection) και probe στο hashtable.
    - Current: Αν υπάρχει zero-copy στήλη, κρατά per-thread page cursor και διαβάζει τα probe keys σειριακά από τα page data (pointer arithmetic) χωρίς per-row binary search. Βλ. [src/execute_default.cpp#L360-L469](src/execute_default.cpp#L360-L469).
    - Γιατί κερδίζει: Σειριακή πρόσβαση στη μνήμη, καθόλου division/modulo ανά row, λιγότερα cache misses.
- **Output phase**
    - Legacy: Per-row `append()` στα output columns (πιθανές επεκτάσεις/ελέγχοι σε κάθε εγγραφή).
    - Current: Προ-δεσμεύει (γνωστό `total_out`) και γράφει με direct indexing σε σελίδες. Βλ. [src/execute_default.cpp#L492-L560](src/execute_default.cpp#L492-L560).
    - Γιατί κερδίζει: Εξαλείφει per-row reallocation checks και μειώνει τη διαχείριση μνήμης.
- **Bloom / Early reject**
    - Legacy: Χωρίς global bloom στο probe.
    - Current: Global bloom filter (128 KiB, 2-hash) απορρίπτει έγκαιρα keys που σίγουρα δεν υπάρχουν. Βλ. [src/execute_default.cpp#L200-L244](src/execute_default.cpp#L200-L244). Μετρημένο κέρδος ~15-16% (11.04s → 9.54s).
- **Parallel probing (adapts by size)**
    - Legacy: Σειριακό.
    - Current: Work-stealing με atomic counter για μεγάλα inputs (κατώφλι 2^18 rows). Βλ. [src/execute_default.cpp#L528-L560](src/execute_default.cpp#L528-L560). Δεν είναι η κύρια πηγή κέρδους, αλλά κρατά υψηλή αξιοποίηση CPU όταν χρειάζεται.

### Γιατί το `get(i)` είναι ακριβό σε αυτό το workload
- Κάνει υπολογισμό `page = i / values_per_page`, `slot = i % values_per_page` ανά row (division/modulo),
- έπειτα διπλή έμμεση πρόσβαση (`pages[page][slot]`) και επιστροφή `value_t`.
- Με εκατομμύρια rows ανά φάση, αυτό μεταφράζεται σε δεκάδες εκατομμύρια divisions/modulos + κακή locality.
**Αντίθετα**, με direct page pointers: μία φορά βρίσκουμε page/span και μετά απλή αύξηση δείκτη.

### Μετρημένες επιδράσεις (113 queries)
- Legacy-like διαδρομή: ~21.7–22.8s (χωρίς bloom, per-row `get`/`append`).
- Current χωρίς global bloom: ~11.04s.
- Current με global bloom: ~9.54s.

---

### 3. Polymorphic Hash Table Interface

**Υλοποίηση**: 
- `include/hashtable_interface.h` - Abstract base class
- `*_wrapper.h` (4 files) - Concrete implementations

**Γιατί υλοποιήθηκε**:
- Runtime επιλογή του hash table algorithm χωρίς recompile
- Testing & benchmarking infrastructure
- Flexible architecture για future optimizations

**Files**:
```cpp
- UnchainedHashTableWrapper (best performer)
- RobinHoodHashTableWrapper
- HopscotchHashTableWrapper  
- CuckooHashTableWrapper
```

---

### 4. Advanced Fibonacci Hashing

**Υλοποίηση**: `unchained_hashtable.h` + `parallel_unchained_hashtable.h`

**Αλγόριθμος**:
```cpp
h(x) = uint64_t(x) * 11400714819323198485ULL
```

**Γιατί υλοποιήθηκε**:
- Καλύτερη κατανομή από simple modulo hashing
- Εξαλείφει patterns (modulo-sensitive keys)
- Χαμηλότερες collisions για IMDB data

**Benefit**: Better hash distribution → fewer probes → faster lookups

---

### 5. Dual Bloom Filter Implementation

**Υλοποίηση**: 
- 4-bit bloom filters (tag-based)
- 16-bit bloom filters (directory-based)

**Γιατί υλοποιήθηκε**:
- Requirements ζητούσαν "16-bit bloom"
- 4-bit version δείχθηκε ταχύτερη σε testing
- Adaptive selection κατά build

**Benefit**: O(1) rejection για non-matching keys πριν linear search

---


### 7. Environment Variable Controls

**Υλοποίηση**:
```bash
REQ_PARTITION_BUILD=1    # Enable partition build (disabled by default)
REQ_3LVL_SLAB=1          # Enable 3-level slab (disabled by default)
EXP_PARALLEL_BUILD=1     # Enable experimental parallel build
```

**Γιατί υλοποιήθηκε**:
- Testing framework για άγνωστα optimizations
- Easy enable/disable χωρίς recompile
- Benchmarking infrastructure
- Validation ότι κάποια features είναι counterproductive

**Usage**:
```bash
REQ_PARTITION_BUILD=1 ./build/fast plans.json  # Test partition build
```

---


---



### 11. Comprehensive Testing & Telemetry

**Υλοποίηση**:
- Per-query timing breakdown
- Total runtime tracking
- Algorithm comparison framework
- Cache statistics collection

**Γιατί υλοποιήθηκε**:
- Verification of implementations
- Performance validation
- Documentation of trade-offs
- Scientific rigor (measurable vs theoretical)

**Benefit**: Δεν βασιζόμαστε σε assumptions - όλα verified
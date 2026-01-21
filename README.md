
* # 📖 Εργασία: Ανάπτυξη Λογισμικού για Πληροφοριακά Συστήματα (2ο Μέρος)

[![Review Assignment Due Date](https://classroom.github.com/assets/deadline-readme-button-22041afd0340ce965d47ae6ef1cefeee28c7c493a6346c4f15d667ab976d596c.svg)](https://classroom.github.com/a/gjaw_qSU)

[![Build Status](https://github.com/uoa-k23a/k23a-2025-d1-runtimeerror/actions/workflows/software_tester.yml/badge.svg)](https://github.com/uoa-k23a/k23a-2025-d1-runtimeerror/actions/workflows/software_tester.yml)

## 👥 Μέλη Ομάδας

* **Ξενοφών Λογοθέτης** - sdi2100087@di.uoa.gr - `1115202100087`
* **Σακκέτος Γεώργιος** - sdi2000177@di.uoa.gr - `1115202000177`
* **Φωτιάδης Ευάγγελος** - sdi1900301@di.uoa.gr - `1115201900301`

---

## Εκτέλεση

##### Οι αρχική υλοποίηση που τρέχει σαν default

```bash
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release -DEXECUTE_IMPL=default -Wno-dev
cmake --build build -- -j $(nproc) fast
```

ή

```bash
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release -Wno-dev
cmake --build build -- -j $(nproc) fast
```

##### Οδηγίες εκτέλεσης unit_tests

```bash
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release -DEXECUTE_IMPL=default -Wno-dev && cmake --build build --target software_tester -- -j && ./build/software_tester --reporter compact
```

##### Οι υλοποιήσεις

```bash
Αλλαγή του header στο αρχείο "execute_default.cpp"
```

> ***Σημείωση:*** Το υπόλοιπο της εκτέλεσης είναι ίδιο

---

## Runtime Toggles (env)

- `REQ_PARTITION_BUILD` (default off): enable required partitioned hash build
- `REQ_PARTITION_BUILD_MIN_ROWS` (default 0): minimum rows to use partitioned build
- `REQ_BUILD_FROM_PAGES` (default on): allow zero-copy INT32 build from input pages without NULLs
- `REQ_SLAB_GLOBAL_BLOCK_BYTES` (bytes): override slab global block size (default 4 MiB)
- `JOIN_GLOBAL_BLOOM` (default on): global bloom filter for probe-side early rejection
- `JOIN_GLOBAL_BLOOM_BITS` (default 20): bloom filter size in bits (2^20 = 128 KiB)
- `JOIN_TELEMETRY` (default on): set to 0 to silence join telemetry output

## Δομή Αρχείων

- **include/** — Public headers και API

  - include/unchained_hashtable.h — API του Unchained Hashtable
  - include/bloom_filter.h — Bloom helpers (tag & masks)
  - include/columnar.h — Columnar API (views, column buffers)
  - include/late_materialization.h — LM helpers (`pack_string_ref`, `resolve_string_ref`)
- **src/** — Υλοποιήσεις

  - src/execute_default.cpp — Integration του JoinAlgorithm και χρήση hashtable
  - src/unchained_hashtable.cpp — Υλοποίηση unchained hashtable
  - src/late_materialization.cpp — LM helpers, scan/resolve functions
  - src/columnar.cpp — Columnar loaders/iterators και paging

Η παραπάνω λίστα συνοψίζει τα πιο σημαντικά αρχεία/φακέλους — δείτε τα αντίστοιχα headers στο `include/` και τις υλοποιήσεις στο `src/` για λεπτομέρειες.

## 1. Late Materialization

* **Υλοποιήθηκε από:** **Φωτιάδης Ευάγγελος**

Το Late Materialization (LM) περιορίζει την άμεση υλοποίηση (materialization) μεγάλων ή μεταβλητού μήκους πεδίων (π.χ. `VARCHAR`) κατά τη διάρκεια των scans και των joins. Αντί να αντιγράφονται οι συμβολοσειρές σε προσωρινές δομές, το σύστημα χρησιμοποιεί compact 64-bit αναφορές (`PackedStringRef`) που περιέχουν `table_id`, `column_id`, `page_id`, `offset` και flags (π.χ. null). Η πραγματική συμβολοσειρά ανακτάται μόνο όταν απαιτείται (π.χ. για έξοδο ή σύγκριση με πλήρες string), μειώνοντας αντιγραφές και memory bandwidth.

Κύριες αλλαγές / οφέλη:

- Zero-copy string handling με `PackedStringRef`, σημαντική μείωση σε αντιγραφές και memory bandwidth.
- Προσθήκη module `late_materialization` (π.χ. `src/late_materialization.cpp`, `include/late_materialization.h`) με helpers για packing/ resolving των string refs.
- Προσαρμογή του `join_columnbuffer_hash` ώστε να αποδέχεται γενικό `value_t` που μπορεί να περιέχει είτε υλοποιημένες τιμές είτε `PackedStringRef`.
- Σελιδοποίηση (`pages`) για αποδοτική διαχείριση ενδιάμεσων αποτελεσμάτων και καλύτερη τοπικότητα στην πρόσβαση.

Σημεία υλοποίησης:

- Files/APIs: `pack_string_ref(...)`, `resolve_string_ref(...)`, ειδικοί comparators/hashes για `PackedStringRef`.
- Adapter functions για συμβατότητα με υπάρχοντα modules (π.χ. `columnar` -> `row` materialization όταν χρειάζεται).

LM Δομές & Scanning:

- `LM_Table`: αναπαράσταση πίνακα σε column-store μορφή με πολλές στήλες (`LM_Column`) και σελίδες (`pages`).
- `LM_Column`: ξεχωρίζει `is_int` για `int_pages` και `str_pages` για varchar.
- `LM_IntPage` / `LM_VarcharPage`: `std::vector<int32_t>` / `std::vector<std::string>` αντίστοιχα.
- Scanning helper: `scan_to_rowstore(Catalog&, table_id, col_ids)` για περιστασιακή υλοποίηση/επιστροφή rowstore views.

Columnar processing updates (σύντομο):

- Μετακίνηση των `scan_columnar_to_columnbuffer` και `finalize_columnbuffer_to_columnar` στο `late_materialization` module για κεντρική λογική.
- `join_columnbuffer_hash` προσαρμόστηκε ώστε να χειρίζεται το γενικό `value_t` και να υποστηρίζει numeric/varchar handlers χωρίς άσκοπες υλοποιήσεις strings.

## 2. Row Store σε Column Store

* **Υλοποιήθηκε από: Σακκέτος Γεώργιος**

Το conversion από row-store σε column-store στοχεύει στη βελτίωση της απόδοσης των αναλυτικών queries με τη βελτιστοποίηση της χωρικής και χρονικής τοπικότητας των στηλών.

Κύρια χαρακτηριστικά της υλοποίησης:

- Column-major αποθήκευση για κάθε στήλη με συνεχή buffers και σελιδοποίηση (`pages`) για αποδοτική ανάγνωση υπο-τμημάτων.
- Υποστήριξη fixed- και variable-length τύπων: τα fixed-size πεδία αποθηκεύονται απευθείας, ενώ τα strings διαχειρίζονται μέσω αναφορών (δείκτες/offsets) για zero-copy πρόσβαση.
- Σύνδεση με Late Materialization: οι στήλες μπορούν να επιστρέφουν `PackedStringRef`/δείκτες ώστε η πλήρης υλοποίηση των πεδίων να γίνεται όποτε απαιτείται.
- APIs/αρχεία:
  - Κύριος header: `include/columnar.h`
  - Ροή φόρτωσης/σελιδοποίησης: `src/columnar.cpp` (ή αντίστοιχο module στο `src/`)

Σημεία σχεδίασης και επιπτώσεις:

- Μειώνει I/O και memory bandwidth για επιλεγμένες στήλες, ειδικά όταν τα queries αφορούν λίγα πεδία ανά εγγραφή.
- Απαιτεί μετατροπή της διεπαφής ανάγνωσης/συγκέντρωσης δεδομένων (scans) ώστε να επιστρέφουν columnar views αντί για πλήρη tuples.
- Συμβατότητα με υπάρχοντα join/hash modules μέσω μικρού adapter layer (`columnar->row` views όταν χρειάζεται).

## 3. Unchained Hashing

* **Υλοποιήθηκε από:** **Ξενοφών Λογοθέτης**
  Η υλοποίηση του unchained hashing ακολουθεί την προσέγγιση της ανεξάρτητης αλυσίδας (separate chaining) αλλά με βελτιστοποιήσεις για cache και resizing.

Κύρια χαρακτηριστικά της υλοποίησης:

- Γρήγορος Fibonacci hashing για INT32 keys (`h(x) = uint64_t(x) * 11400714819323198485ULL`).
- Προϋπολογισμένος πίνακας 16-bit popcount (65536 entries) για O(1) popcount lookups.
- Συμπαγής unchained hashtable με prefix directory (prefix από τα υψηλά bits του hash).
- 4-bit/16-bit Bloom filters ανά directory bucket για γρήγορο prefiltering (bitmask/tag check).
- Contiguous buffer of tuples ανά prefix — όχι dynamic allocations ανά bucket.
- Build-phase σε 3 περάσματα (counts → offsets → fill) και γρήγορος probe-phase με bloom reject.
- Μικρές αλλαγές στο `execute.cpp` για ενσωμάτωση του νέου πίνακα.

Σημεία σχεδίασης (σύντομο):

- Directory/prefix: `prefix = (h >> 16) & dir_mask` — κάθε bucket έχει `begin_idx`, `end_idx`, `bloom (uint16_t)`.
- Probe-phase: compute hash → locate prefix → bloom filter reject → return pointer+length για candidate range.
- Exact-match comparisons γίνονται στον `JoinOperator`, το hashtable επιστρέφει το candidate range.

Tests / verification:

- Unit tests καλύπτουν bloom/tag correctness, fibonacci hashing distribution, build+probe, heavy collisions και large-scale tests (π.χ. 100k tuples).
- Οι δοκιμές εκτελούνται από το `software_tester` και περνούν στο repo.

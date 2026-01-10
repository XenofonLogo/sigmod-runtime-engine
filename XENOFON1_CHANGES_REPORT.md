# xenofon1 vs baseline (9c46df7) — Πλήρης διαφορά + τι σημαίνει για απόδοση

## Στόχος
Να τρέχει το `./build/fast plans.json` όσο πιο γρήγορα γίνεται, μειώνοντας:
- materialization τιμών (allocations + κίνηση `value_t` στη μνήμη)
- τυχαία πρόσβαση μνήμης / pointer chasing στα joins
- περιττή δουλειά στο probe και στο output materialization

## Μετρήσεις (σε αυτό το μηχάνημα)
- Baseline commit `9c46df7`: `elapsed=98.48s` (user=62.76s sys=31.61s) RSS≈3.6GB
- Branch `xenofon1` (rebuilt `fast`): `elapsed=59.51s` (user=45.17s sys=23.66s) RSS≈4.5GB

Ερμηνεία:
- ~1.65× πιο γρήγορο end-to-end.
- Το μεγαλύτερο RSS είναι αναμενόμενο: πιο επιθετικό pre-allocation στα outputs και μεγαλύτερες auxiliary δομές για hash tables.

## Αρχεία που άλλαξαν (name-status)
Προστέθηκαν:
- `include/column_zero_copy.h`
- `include/parallel_unchained_hashtable.h`
- `src/column_zero_copy.cpp`
- `tests/indexing_optimization_tests.cpp`

Τροποποιήθηκαν:
- `include/columnar.h`
- `include/cuckoo_wrapper.h`
- `include/hash_common.h`
- `include/hashtable_interface.h`
- `include/hopscotch_wrapper.h`
- `include/late_materialization.h`
- `include/robinhood.h`
- `include/robinhood_wrapper.h`
- `include/unchained_hashtable.h`
- `include/unchained_hashtable_wrapper.h`
- `src/columnar.cpp`
- `src/execute_default.cpp`
- `src/late_materialization.cpp`
- `tests/software_tester.cpp`

## Κύριες αλλαγές απόδοσης (τι άλλαξε + γιατί έχει σημασία)

### 1) Zero-copy scan path για INT32 χωρίς NULLs
Αρχεία:
- `src/late_materialization.cpp`
- `include/columnar.h`
- `include/column_zero_copy.h`, `src/column_zero_copy.cpp`

Τι άλλαξε:
- Το scan->`ColumnBuffer` ανιχνεύει INT32 στήλες που **δεν έχουν NULLs**.
- Για αυτές τις στήλες δεν κάνει materialize ένα `value_t` ανά row. Αντί γι’ αυτό κρατά:
  - pointer στο αρχικό `Column`
  - ένα index `page_offsets` για γρήγορο lookup σελίδας/offset
- Το `column_t::get()` αποκτά fast zero-copy path που διαβάζει κατευθείαν από τα page data.
- Προστίθεται μικρό cache (`cached_page_idx`) ώστε σε σχεδόν σειριακή πρόσβαση να αποφεύγεται binary search.
- Προστίθεται thread-safe accessor (`get_cached`) για περιπτώσεις parallel materialization.

Τι σημαίνει:
- Κόβει τεράστιο κόστος όταν πολλά INT32 columns περνάνε μέσα από joins.
- Το eager materialization πολλαπλασιάζει memory traffic· με zero-copy μετατοπίζουμε χρόνο από “alloc + copy” σε “διάβασε μόνο ό,τι χρειάζεσαι”.

Tradeoffs:
- Το zero-copy ενεργοποιείται μόνο όταν είναι **σωστό** (χωρίς NULLs). Αν υπάρχουν NULLs, γίνεται fallback στο κλασικό materialization.

### 2) Ανακατασκευή hash join στον default executor: parallel probe + preallocated output
Αρχείο:
- `src/execute_default.cpp`

Τι άλλαξε:
- Το join γίνεται εξειδίκευση για INT32 και ξαναγράφεται πάνω σε interface hash-table που επιστρέφει buckets.
- Το probe παραλληλοποιείται για μεγάλα probe sides (`probe_n >= 2^19`) με `std::thread`.
- Κάθε thread μαζεύει τοπικά `(lidx,ridx)` pairs σε vector, αποφεύγοντας locks.
- Τα output columns γίνονται **pre-sized** ακριβώς σε `total_out` rows και μετά γεμίζουν με index writes (χωρίς συνεχές `append()` growth).
- Για πολύ μεγάλα outputs (`total_out >= 2^22`) παραλληλοποιείται και το ίδιο το output materialization.
- Ένα heuristic (“optimizer-ish”) μπορεί να αγνοήσει το `build_left` του plan και να χτίσει hash table στη μικρότερη πλευρά.

Τι σημαίνει:
- Χτυπάει τα 2 μεγαλύτερα bottlenecks στα hash joins:
  1) random probes + bucket scans
  2) κόστος output materialization
- Το preallocation κάνει τις εγγραφές σειριακές και κόβει reallocs.
- Το parallel probe αυξάνει CPU utilization σε μεγάλα joins.

Ρυθμίσεις (knobs):
- `AUTO_BUILD_SIDE=0` απενεργοποιεί το heuristic επιλογής build side.

### 3) Προαιρετικό global bloom filter πριν το probe
Αρχείο:
- `src/execute_default.cpp`

Τι άλλαξε:
- Όταν ενεργοποιηθεί, χτίζεται ένα global bloom filter από τα build-side keys.
- Probe keys που σίγουρα δεν υπάρχουν κόβονται πριν γίνει hash-table probe.

Τι σημαίνει:
- Μειώνει ακριβές probes όταν το join selectivity είναι χαμηλό.

Ρυθμίσεις:
- Enable: `JOIN_GLOBAL_BLOOM=1`
- Μέγεθος: `JOIN_GLOBAL_BLOOM_BITS=16..24` (default 20)

### 4) Cache-friendly “unchained/flat” hash table + καλύτερο directory sizing
Αρχεία:
- `include/parallel_unchained_hashtable.h` (νέο)
- `include/unchained_hashtable.h`
- `include/hash_common.h`

Τι άλλαξε:
- Εισάγεται unchained table που χρησιμοποιεί prefix sums και contiguous tuple storage.
- Το `HashEntry.row_id` γίνεται `uint32_t` (μικρότερο footprint ανά entry).
- Το directory sizing γίνεται performance-oriented: power-of-two, capped, με στόχο bucket length ~8.

Τι σημαίνει:
- Λιγότερο pointer chasing, λιγότερα cache misses, μικρότερο memory bandwidth ανά probe.
- Μικρότερο entry footprint → καλύτερη cache locality.

### 5) Καθάρισμα hash table interface + ενημέρωση wrappers
Αρχεία:
- `include/hashtable_interface.h`
- `include/unchained_hashtable_wrapper.h`
- `include/cuckoo_wrapper.h`, `include/hopscotch_wrapper.h`, `include/robinhood_wrapper.h`

Τι άλλαξε:
- Το interface πλέον δουλεύει με `std::vector<HashEntry<Key>>` (αντί για `std::pair<Key,size_t>`), ταιριάζοντας με το βελτιστοποιημένο layout.
- Τα wrappers προσαρμόζουν backends που ακόμη περιμένουν `pair` (με μετατροπή).

Τι σημαίνει:
- Η hot-path του executor αποφεύγει περιττές μετατροπές και έχει σταθερό, μικρότερο layout.

### 6) Εναλλακτική υλοποίηση join στο columnar.cpp
Αρχείο:
- `src/columnar.cpp`

Τι άλλαξε:
- Αντικατάσταση `std::unordered_map<Key, vector<rowid>>` με `UnchainedHashTable<Key>` για INT32 και για packed string refs.
- Στα VARCHAR joins γίνεται hash στο packed 64-bit ref αντί να materialize/κατασκευάζει strings.

Τι σημαίνει:
- Πολύ καλύτερη locality και πολύ λιγότερη πίεση από allocations.

### 7) Προσθήκη/επέκταση tests
Αρχεία:
- `tests/indexing_optimization_tests.cpp`
- `tests/software_tester.cpp`

Τι άλλαξε:
- Tests που επιβεβαιώνουν ότι INT32 χωρίς NULLs χρησιμοποιεί zero-copy και ότι τα NULLs το απενεργοποιούν.
- Επέκταση harness για hash quality και functional join tests.

Τι σημαίνει:
- Προστατεύει τη σωστότητα του “fast path” που δίνει τη μεγάλη διαφορά στον χρόνο.

## Γρήγορο validation
- Build: `cmake --build build --target fast -j`
- Run: `./build/fast plans.json`

Προαιρετικά diagnostics:
- `JOIN_TELEMETRY=1 ./build/fast plans.json` τυπώνει bandwidth-style lower bounds.
- `JOIN_GLOBAL_BLOOM=1 ./build/fast plans.json` ενεργοποιεί bloom prefilter (για πειραματισμό).

## Γιατί αυτό επιταχύνει το plans.json
Τα IMDB-like analytic plans συνήθως κυριαρχούνται από:
- INT32-heavy joins (IDs) σε μεγάλα inputs
- τεράστιο output materialization
- memory bandwidth + cache misses

Το `xenofon1` στοχεύει ακριβώς αυτά με:
- αποφυγή materialization όταν είναι ασφαλές (zero-copy)
- cache-friendly hash tables και μικρότερο entry format
- parallelization στα ακριβά σημεία (με thresholds για να μη πληρώνουμε overhead)
- προ-δέσμευση outputs και σειριακές εγγραφές στη μνήμη

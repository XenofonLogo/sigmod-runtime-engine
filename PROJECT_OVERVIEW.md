# 📊 Περιγραφή του Έργου

## 🎯 Γενική Επισκόπηση

Αυτό είναι ένα **σύστημα βάσης δεδομένων υψηλής απόδοσης** που αναπτύχθηκε για την πανεπιστημιακή εργασία "Ανάπτυξη Λογισμικού για Πληροφοριακά Συστήματα (2ο Μέρος)" και συμμετέχει στο **SIGMOD Contest 2025**.

**Team Members:**
- Ξενοφών Λογοθέτης (sdi2100087@di.uoa.gr)
- Σακκέτος Γεώργιος (sdi2000177@di.uoa.gr)  
- Φωτιάδης Ευάγγελος (sdi1900301@di.uoa.gr)

---

## 🏗️ Αρχιτεκτονική

Το έργο περιλαμβάνει **3 κύρια modules βελτιστοποίησης**:

### 1️⃣ **Late Materialization (Αργή Υλοποίηση)**
**Υλοποιητής:** Φωτιάδης Ευάγγελος

**Τι κάνει:**
- **Καθυστερημένη μετατροπή strings** σε πλήρεις τιμές
- Χρησιμοποιεί compact **64-bit PackedStringRef** αντί να αντιγράφει ολόκληρα strings
- Τα strings ανακτώνται **μόνο όταν απαιτούνται** (έξοδος, σύγκριση)
- Μειώνει αντιγραφές και κατανάλωση memory bandwidth

**Αρχεία:**
- `src/late_materialization.cpp`
- `include/late_materialization.h`

**Πλεονεκτήματα:**
- Zero-copy string handling
- Μικρότερη κατανάλωση μνήμης
- Ταχύτερη επεξεργασία

---

### 2️⃣ **Row-Store σε Column-Store Conversion**
**Υλοποιητής:** Σακκέτος Γεώργιος

**Τι κάνει:**
- Μετατροπή δεδομένων από **행-προσανατολισμό (row-major)** σε **στήλη-προσανατολισμό (column-major)**
- **Column-major αποθήκευση** με συνεχή buffers
- Σελιδοποίηση (pages) για αποδοτική ανάγνωση υποσυνόλων
- Υποστήριξη fixed-length (INT, FLOAT) και variable-length (VARCHAR) τύπων

**Αρχεία:**
- `src/columnar.cpp`
- `include/columnar.h`

**Πλεονεκτήματα:**
- Μειώνει I/O για queries που προσπελαύνουν λίγες στήλες
- Καλύτερη χρονική τοπικότητα
- Αποδοτικότερη cache utilization

---

### 3️⃣ **Unchained Hashing (Κατακερματισμός χωρίς Αλυσίδες)**
**Υλοποιητής:** Ξενοφών Λογοθέτης

**Τι κάνει:**
- **Βελτιστοποιημένος hashtable** για rapid join operations
- Fibonacci hashing για INT32 keys
- 4-bit/16-bit Bloom filters ανά bucket για γρήγορο filtering
- Prefix directory με flat structure για cache efficiency
- 3-phase build: Count → Offsets → Fill

**Αρχεία:**
- `src/unchained_hashtable.cpp`
- `include/unchained_hashtable.h`

**Πλεονεκτήματα:**
- Γρήγορη probe-phase με Bloom filter prefiltering
- Cache-friendly contiguous tuples buffer
- Μειωμένη directory μνήμη (6 bytes αντί 18)

---

## 📁 Δομή Αρχείων

```
k23a-2025-d1-runtimeerror/
├── include/               # Public headers/API
│   ├── unchained_hashtable.h      # Hashtable API
│   ├── bloom_filter.h              # Bloom filter helpers
│   ├── columnar.h                  # Column-store API
│   ├── late_materialization.h     # LM helpers
│   └── ...
├── src/                   # Υλοποιήσεις
│   ├── execute_default.cpp         # Join orchestration
│   ├── unchained_hashtable.cpp     # Hashtable implementation
│   ├── late_materialization.cpp    # LM functions
│   ├── columnar.cpp                # Column-store loaders
│   ├── csv_parser.cpp              # CSV parsing
│   ├── build_table.cpp             # Table construction
│   └── ...
├── tests/                 # Unit tests (Catch2)
├── cache/                 # Pre-computed lookup tables (.tbl files)
├── imdb/                  # IMDB dataset (CSV)
├── build/                 # Build artifacts (CMake)
│   ├── fast              # Main executable
│   ├── software_tester   # Unit test executable
│   └── unit_tests        # Test runner
├── CMakeLists.txt        # Build configuration
└── README.md             # Original documentation
```

---

## 🔄 Ροή Εκτέλεσης

### Εξαγωγή δεδομένων (Loading):
1. **CSV Parser** → Διαβάζει IMDB CSV files
2. **Build Table** → Δημιουργεί in-memory table structures
3. **Columnar Conversion** → Μετατρέπει σε column-major format
4. **Late Materialization** → Αποθηκεύει strings ως PackedStringRef

### Επεξεργασία Queries (JOIN):
1. **Table Scan** → Ανάγνωση στηλών (columnar)
2. **Hashtable Build** → Κατασκευή unchained hashtable
3. **Probe & Join** → Γρήγορη κατακερματισμένη σύνδεση
4. **Bloom Filter** → Prefiltering για απόρριψη candidates
5. **Output** → Υλοποίηση strings και εγγραφή αποτελεσμάτων

---

## ⚙️ Τεχνολογίες

- **Γλώσσα:** C++17
- **Build System:** CMake
- **Testing:** Catch2
- **External Libraries:**
  - Abseil (Google utilities)
  - RE2 (Regular expressions)
  - nlohmann/json
  - range-v3
  - SQL Parser

---

## 📊 Απόδοση

- **Baseline:** 98.48s
- **Current:** 59.51s (1.65× faster)
- **Tests:** 16,529 assertions, 51 test cases ✅

---

## 🚀 Εκτέλεση

### Κανονική εκτέλεση:
```bash
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release -Wno-dev
cmake --build build -- -j $(nproc) fast
./build/fast
```

### Unit Tests:
```bash
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release -Wno-dev
cmake --build build --target software_tester -- -j
./build/software_tester --reporter compact
```

---

## 📝 Σύνοψη

Αυτό το έργο είναι μια **high-performance database query engine** που συνδυάζει:
- ✅ Late materialization για αποδοτικότερη χρήση μνήμης
- ✅ Column-store storage για γρήγορα scans
- ✅ Unchained hashing με Bloom filters για γρήγορα joins
- ✅ Comprehensive testing με 16k+ assertions

Ο συνδυασμός αυτών των τεχνικών επιτυγχάνει **65% απόδοσης βελτίωση** σε σχέση με το baseline.

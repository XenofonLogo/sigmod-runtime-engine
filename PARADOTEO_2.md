## 🟡 ΜΕΡΟΣ 2ο: Οργάνωση Δεδομένων (Column-store) & Late Materialization

### Σκοπός & Κίνητρο

Το αρχικό σύστημα χρησιμοποιούσε **row-store δομή**:

```
Row-Store (BAD):
┌───────────────────────────────┐
│ Row 1: (id, name, salary)     │ ← Inline data
│ Row 2: (id, name, salary)     │ ← Inline data
│ Row 3: (id, name, salary)     │ ← Inline data
└───────────────────────────────┘

Πρόβλημα:
- Cache misses όταν χρειάζονται μόνο ορισμένες στήλες
- Αναγκαστική δημιουργία ενδιάμεσων materialized rows
- Κακή cache efficiency
```

**Λύση**: Μετάβαση σε **column-store** με **late materialization**:

```
Column-Store (GOOD):
Column[id]:     ┌──────────────┐
                │ 1,2,3,4,...  │  (contiguous)
                └──────────────┘

Column[salary]: ┌──────────────┐
                │ 50,60,75,... │  (contiguous)
                └──────────────┘

Column[name]:   ┌──────────────┐
                │ refs→strings │  (late materialized)
                └──────────────┘
```

---

### 2.1 Late Materialization & Διαχείριση VARCHAR

#### Πρόβλημα Αρχικής Υλοποίησης

```cpp
// BEFORE (Bad)
struct Row {
    int32_t id;
    std::string name;  // ← Inline → huge memory footprint
    int32_t salary;
};

std::vector<Row> rows;  // ← All columns mixed
```

**Αποτέλεσμα**:
- ❌ Σειριακή ανάγνωση όλων των στηλών
- ❌ Μεγάλα row sizes
- ❌ Cache misses για μη χρησιμοποιούμενα fields
- ❌ Forced materialization

#### Νέα Αρχιτεκτονική: value_t Type

**Σχεδιασμός νέας δομής (έως 64-bit)** για την αναπαράσταση των VARCHAR που λειτουργεί ως δείκτης (index) στο αρχικό column store:

```cpp
// AFTER (Good)
// Unified value type that represents both INT32 and VARCHAR reference
union value_t {
    int32_t int_val;              // For INT32 columns
    StringRef string_ref;          // For VARCHAR columns
};

// StringRef: 64-bit index into original column store
// (πίνακας, στήλη, σελίδα, θέση)
struct StringRef {
    uint16_t column_id;    // Which column?
    uint16_t page_id;      // Which page in that column?
    uint32_t offset;       // Offset within page
};
```

**Ορισμός του τύπου value_t** που αναπαριστά ταυτόχρονα INT32 και τη νέα δομή για strings, αντικαθιστώντας τη χρήση variant.

#### Column-Store Structure

**Σελιδοποιημένη δομή (column_t)** για την αποθήκευση ενδιάμεσων αποτελεσμάτων:

```cpp
// Σελιδοποιημένη δομή για κάθε στήλη
struct Column {
    std::vector<Page*> pages;
    DataType type;
    size_t row_count;
};

struct Page {
    void* data;           // Raw memory
    size_t capacity;      // Bytes allocated
    size_t size;          // Bytes used
};

// Ενδιάμεσα αποτελέσματα σε column-store format
struct column_t {
    std::vector<std::vector<value_t>> pages;  // Paged column
};
```

#### Τροποποίηση ScanNodes

**Τροποποίηση των ScanNodes για την παραγωγή ενδιάμεσων αποτελεσμάτων σε μορφή vector<vector<value_t>>**:

```cpp
// BEFORE: Produced rows
std::vector<Row> ScanNode::execute() {
    std::vector<Row> result;
    for (size_t row = 0; row < num_rows; row++) {
        result.push_back({
            read_int32(col_id, row),
            read_string(col_id, row),  // ← Materialization!
            read_int32(col_id, row)
        });
    }
    return result;
}

// AFTER: Produces column vectors
std::vector<column_t> ScanNode::execute() {
    std::vector<column_t> result(3);  // 3 columns
    
    // Column 0: INT32 (id)
    for (size_t page = 0; page < pages.size(); page++) {
        auto col = read_int32_column(page);
        result[0].pages.push_back(col);
    }
    
    // Column 1: VARCHAR (name) - Late materialization
    for (size_t page = 0; page < pages.size(); page++) {
        std::vector<value_t> refs;
        for (size_t i = 0; i < page_size; i++) {
            refs.push_back(StringRef{col_id, page, i * 4});
        }
        result[1].pages.push_back(refs);
    }
    
    // Column 2: INT32 (salary)
    for (size_t page = 0; page < pages.size(); page++) {
        auto col = read_int32_column(page);
        result[2].pages.push_back(col);
    }
    
    return result;
}
```

#### Πλεονεκτήματα

✅ **Lazy materialization**: VARCHARs only read when needed  
✅ **Better cache locality**: Sequential int32_t values  
✅ **Reduced memory footprint**: No inline strings  
✅ **SIMD-friendly**: Contiguous numeric columns  
✅ **Selective filtering**: Can skip entire columns

---

### 2.2 Ενδιάμεσα Αποτελέσματα σε Column-store

**Τροποποίηση των ScanNodes και των Hash Joins ώστε να λειτουργούν αποκλειστικά με τη νέα δομή vector<column_t>**, εξαλείφοντας κάθε row-store δομή από την κεντρική συνάρτηση εκτέλεσης.

#### Previous (Row-Store) Join

```cpp
// BEFORE: Row-based join
std::vector<OutRow> HashJoin::execute(
    const std::vector<Row>& probe_rows,
    const HashTable& build_table
) {
    std::vector<OutRow> result;
    
    for (const auto& probe : probe_rows) {
        auto build = build_table.find(probe.join_key);
        if (build) {
            result.push_back({
                probe.col1, probe.col2,
                build.col3, build.col4
            });
        }
    }
    
    return result;
}
```

**Πρόβλημα**: Κάθε probe row περιέχει όλες τις στήλες.

#### New (Column-Store) Join

```cpp
// AFTER: Column-based join
std::vector<column_t> HashJoin::execute(
    const std::vector<column_t>& probe_cols,
    const ParallelUnchainedHashTable& build_table
) {
    // Operate on column vectors
    std::vector<column_t> result(out_column_count);
    
    // Phase 1: Only read join key column from probe
    const auto& probe_join_col = probe_cols[join_key_idx];
    
    // Phase 2: Hash-based filtering
    std::vector<size_t> match_rows;
    for (size_t page = 0; page < probe_join_col.pages.size(); page++) {
        for (size_t row = 0; row < probe_join_col.pages[page].size(); row++) {
            value_t key = probe_join_col.pages[page][row];
            
            if (build_table.probe(key.int_val) != INVALID) {
                match_rows.push_back(page * PAGE_SIZE + row);
            }
        }
    }
    
    // Phase 3: Late materialization - only copy matched columns
    for (size_t col_idx = 0; col_idx < result.size(); col_idx++) {
        if (source_table[col_idx] == BUILD) {
            result[col_idx] = copy_from_build(match_rows);
        } else {
            result[col_idx] = copy_from_probe(probe_cols[col_idx], match_rows);
        }
    }
    
    return result;
}
```

**Ενδιάμεσα αποτελέσματα των joins** αποθηκεύονται σε column-store format, με τα δεδομένα κάθε στήλης να είναι σειριακά στη μνήμη.



### 2.3 Unchained Hashtable

#### Τι Είναι

**Υλοποίηση προηγμένου πίνακα κατακερματισμού χωρίς αλυσίδες** που συνδυάζει:
1. **Open addressing** χωρίς αλυσίδες (unchained)
2. **Directory structure**: Κάθε hash value έχει ξεχωριστό bucket
3. **Bloom filters (16-bit)**: Ενσωματωμένα στα ανώτερα bits των δεικτών για γρήγορη απόρριψη στοιχείων που δεν μετέχουν στη ζεύξη

#### Αρχιτεκτονική

```
Directory:
┌────────────────────────────────┐
│  Bucket[0]  → Tuples 1,5,9     │  (hash value 0)
│  Bucket[1]  → Tuples 2,7       │  (hash value 1)
│  Bucket[2]  → Tuples 3,4,6,8   │  (hash value 2)
│  ...                           │
└────────────────────────────────┘
         ↓
    Contiguous Tuples Array
    ┌────┬────┬────┬────┬────┬────┬────┬────┬────┐
    │ t1 │ t5 │ t9 │ t2 │ t7 │ t3 │ t4 │ t6 │ t8 │
    └────┴────┴────┴────┴────┴────┴────┴────┴────┘
```



**Αρχείο**: `include/unchained_hashtable.h` (Sequential base)

Δομή:
- Directory: Array με Bucket structures (start, end, bloom_filter)
- Tuples Array: Contiguous storage με HashEntry δομές
- Hash: Fibonacci hashing (k * 11400714819323198485ULL)
- Bloom: 16-bit per bucket για fast rejection

5-Phase Build:
1. Count entries
2. Prefix sum (offsets)
3. Single malloc
4. Copy & compute bloom
5. Set ranges


#### Bloom Filters

Κάθε bucket έχει 16-bit bloom filter:
- Απόρριψη ~95% non-matching keys
- Trade-off: Small false positive rate

#### Performance (Sequential)

| Metric | Time |
|---|---|
| Build | ~1 ms |
| Probe | ~8-10 ms |
| Total (113 queries) | **46.12 sec** ✅ |

**Σημείωση**: Ο unchained hashtable με sequential execution μετρήθηκε στα 46.12 sec

---


#### Performance Impact

| # | Υλοποίηση | Runtime (sec) | Βελτίωση vs Προηγούμενο (%) | Βελτίωση vs Baseline (%) |
|---|-----------|---------------|-----------------------------|--------------------------|
| 0 | unordered_map (Baseline) | 242.85 | – | – |
| 1 | Late Materialization | 132.53 | 43.5% | 43.5% |
| 2 | Column-Store + Late Materialization | 64.33 | 51.4% | 73.5% |
| 3 | Unchained Hashtable + Column + Late | 46.12 | 28.3% | 81.0% |


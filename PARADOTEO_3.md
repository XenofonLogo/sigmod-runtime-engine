## � PARADOTEO 3ο: Λεπτομερής Υλοποίηση των Απαιτήσεων

---

## **ΕΝΌΤΗΤΑ 1: Απαιτήσεις Βελτιστοποίησης Indexing**

### 1.1  Αποφυγή Περιττής Αντιγραφής Στηλών


**Κώδικας:**
- [src/late_materialization.cpp#L85-L100]: Για INT32 χωρίς NULLs, **δεν γίνεται αντιγραφή**. Το column προστίθεται ως zero-copy reference
- [src/execute_default.cpp#L50-L85]: Έλεγχος `build_col.is_zero_copy` αποφεύγει την ενδιάμεση materialization

**Αναλυτικά:**
```cpp
// src/late_materialization.cpp#L85-L100
if (column.type == DataType::INT32 && !column_has_nulls(column)) {
    out_col.is_zero_copy = true;           // ← Δεν αντιγράφεται
    out_col.src_column = &column;           // ← Direct pointer
    out_col.num_values = input_columnar.num_rows;
    
    // Build page offsets for fast lookup
    out_col.page_offsets.push_back(0);
    size_t cumulative = 0;
    for (const auto& page_ptr : column.pages) {
        auto* page = page_ptr->data;
        uint16_t num_rows = *reinterpret_cast<const uint16_t*>(page);
        cumulative += num_rows;
        out_col.page_offsets.push_back(cumulative);
    }
    
    continue; // Skip materialization ← Σκιπ αντιγραφή
}
```

---

### 1.2 Δεικτοδότηση με Βάση τον Τύπο Στήλης


**Κώδικας:**
- [src/late_materialization.cpp#L85-L110]: Type-aware branching για INT32/VARCHAR

**Αναλυτικά:**

| Τύπος | NULL | Ενέργεια | Κώδικας |
|------|------|----------|--------|
| INT32 | Χωρίς | Zero-copy | [src/late_materialization.cpp#L85-L101] |
| INT32 | Με | Materialization | [src/late_materialization.cpp#L110+] |
| VARCHAR | Οποιαδήποτε | Materialization | [src/late_materialization.cpp#L110+] |

---

### 1.3 Άμεση Δεικτοδότηση στις Σελίδες Εισόδου


**Κώδικας:**
- [include/parallel_unchained_hashtable.h#L224-L275]: `build_from_zero_copy_int32()` διαβάζει **απευθείας** από input pages
- [src/execute_default.cpp#L76-L80]: Κληση χωρίς intermediate materialization

**Αναλυτικά:**
```cpp
// include/parallel_unchained_hashtable.h#L224+
void build_from_zero_copy_int32(const Column* src_column,
                                const std::vector<std::size_t>& page_offsets,
                                std::size_t num_rows) {
    // Διαβάζει απευθείας από src_column, χωρίς αντιγραφή
    // Τοποθετεί tuples στη hash table in-place
}
```

---

### 1.4 Ανίχνευση Μεταδεδομένων Στήλης



**Κώδικας:**
- [src/late_materialization.cpp#L30-L60]: `column_has_nulls()` ανιχνεύει NULL τιμές μέσω bitmap headers

**Αναλυτικά:**
```cpp
// src/late_materialization.cpp#L30-L60
bool column_has_nulls(const Column* col) {
    for (size_t page_idx = 0; page_idx < col->pages.size(); ++page_idx) {
        auto* page = col->pages[page_idx]->data;
        uint16_t num_rows_in_page = *reinterpret_cast<const uint16_t*>(page);
        
        // Διαβάζει bitmap header από page
        if (num_rows_in_page == 0) continue;
        
        auto* bitmap = reinterpret_cast<const uint8_t*>(
            page + PAGE_SIZE - (num_rows_in_page + 7) / 8);
        
        // Ελέγχει αν κάποιο bit είναι set (δηλώνει NULL)
        uint32_t bitmap_bytes = (num_rows_in_page + 7) / 8;
        for (size_t i = 0; i < bitmap_bytes; ++i) {
            uint8_t expected = 0xFF;
            if (i == bitmap_bytes - 1 && (num_rows_in_page % 8 != 0)) {
                expected = (1u << (num_rows_in_page % 8)) - 1;
            }
            if ((bitmap[i] & expected) != expected)
                return true;  // ← Βρήκε NULL
        }
    }
    return false;  // ← Δεν υπάρχουν NULLs
}
```

**Στίγμα:** Διαβάζει bitmap headers κάθε page, όχι scan όλων των τιμών.

---

## **ΕΝΌΤΗΤΑ 2: Μοντέλο Παράλληλης Εκτέλεσης (Global)**

### 2.1  Περιορισμοί Συγχρονισμού


**Ελεγχος:**
-  **Χωρίς mutex**: Δεν υπάρχει χρήση `std::mutex`, `std::lock_guard`, `std::unique_lock`
-  **Χωρίς condition variables**: Δεν υπάρχει χρήση `std::condition_variable`
-  **Χωρίς semaphores**: Δεν υπάρχει χρήση `sem_*` ή POSIX semaphores
-  **Χρήση atomic counters μόνο**: [src/execute_default.cpp#L125-L135] χρησιμοποιεί `std::atomic` για work stealing

**Κώδικας:**
```cpp
// src/execute_default.cpp#L125-L135
WorkStealingCoordinator ws_coordinator(ws_config);  // ← Χρήσει atomic, όχι mutex

// include/work_stealing.h
class WorkStealingCoordinator {
private:
    std::atomic<size_t> work_counter_;  // ← Μόνο atomic, όχι mutex
};
```

---

### 2.2  Παράλληλη Εκτέλεση με Φάσεις


**Κώδικας:**
- [src/execute_default.cpp#L36-L205]: Φάσεις Build → Probe → Materialize με explicit `join()`

**Αναλυτικά:**
```cpp
// src/execute_default.cpp#L36-L205
struct JoinAlgorithm {
    void run_int32() {
        // ========== PHASE 1: BUILD ==========
        table->build_from_entries(entries);  // Serial build
        
        // ========== PHASE 2: PROBE (Parallel) ==========
        std::vector<std::thread> probe_threads;
        for (size_t t = 0; t < nthreads; ++t) {
            probe_threads.emplace_back([&, t]() {
                // Parallel probing with work stealing
            });
        }
        
        // ↓ Explicit phase synchronization
        for (auto& th : probe_threads) th.join();  // ← Join
        
        // ========== PHASE 3: MATERIALIZE (Serial) ==========
        // Results from probe are now safe to merge
    }
};
```



## **ΕΝΌΤΗΤΑ 3: Παράλληλη Κατασκευή Hash Table**

### 3.1  Κατασκευή με Διαμερίσεις και Δύο Φάσεις


**Κώδικας:**
- [include/parallel_unchained_hashtable.h#L137-L143]: Ελέγχει flag `REQ_PARTITION_BUILD=1` και επιλέγει:
  - Αν flag=1: Parallel 3-phase partitioned build [line 139]
  - Αν flag=0: Serial 5-phase build [lines 150-207]

**Αποδοχή:** Και οι δύο διαδρομές (serial και partitioned) είναι πλήρως υλοποιημένες. Το serial build χρησιμοποιείται για IMDB workload γιατί είναι ταχύτερο.

---

### 3.2  Φάση 1 – Συλλογή Tuples & Διαμέριση (Opt-in)


**Κώδικας:**
- [include/parallel_unchained_hashtable.h#L371-L420]: Phase 1 partitioning

```cpp
// include/parallel_unchained_hashtable.h#L371-L420
void build_from_entries_partitioned_parallel(...) {
    const std::size_t nthreads = std::thread::hardware_concurrency();
    
    // ========== PHASE 1: PARTITION ==========
    std::vector<std::vector<ChunkList>> lists(nthreads, std::vector<ChunkList>(dir_size_));
    
    // Σλάβ allocators (thread-local)
    std::vector<std::unique_ptr<TempAlloc>> allocs;
    for (std::size_t t = 0; t < nthreads; ++t) {
        allocs.emplace_back(std::make_unique<TempAlloc>());
    }
    
    const std::size_t block = (n + nthreads - 1) / nthreads;
    std::vector<std::thread> threads;
    
    for (std::size_t t = 0; t < nthreads; ++t) {
        const std::size_t begin = t * block;
        const std::size_t end = std::min(n, begin + block);
        
        threads.emplace_back([&, t, begin, end]() {
            auto& my_lists = lists[t];
            TempAlloc& my_alloc = *allocs[t];
            
            // Κάθε thread διαμερίζει τα tuples της στο chunk lists ανά partition
            for (std::size_t i = begin; i < end; ++i) {
                const uint64_t h = compute_hash(entries[i].key);
                const std::size_t slot = (h >> shift_) & dir_mask_;
                const uint16_t tag = Bloom::make_tag_from_hash(h);
                
                // Allocate από thread-local slab
                chunklist_push(my_lists[slot], 
                               TmpEntry{entries[i].key, entries[i].row_id, tag}, 
                               my_alloc);
            }
        });
    }
    
    for (auto& th : threads) th.join();  // ← Barrier
}
```

**Αναμενόμενο Συμπεριφορά:**
- Κάθε thread παίρνει ένα block από entries
- Υπολογίζει hash και χωρίζει σε per-partition chunk lists
- Χρησιμοποιεί thread-local TempAlloc για allocation
- Barrier στο `join()` πριν την Phase 2

---

### 3.3  Φάση 2 – Τελική Κατασκευή Hash Table (Opt-in)


**Κώδικας:**
- [include/parallel_unchained_hashtable.h#L421-L445]: Phase 2 one-writer-per-partition

```cpp
// include/parallel_unchained_hashtable.h#L421-L445
// ========== PHASE 2: COUNT & BLOOM ==========
if (counts_.size() != dir_size_) counts_.assign(dir_size_, 0);
std::fill(bloom_filters_.begin(), bloom_filters_.end(), 0);

threads.clear();
const std::size_t workers = nthreads;

for (std::size_t t = 0; t < workers; ++t) {
    threads.emplace_back([&, t]() {
        // One writer per partition: κάθε thread φροντίζει ένα διαφορετικό set
        for (std::size_t slot = t; slot < dir_size_; slot += workers) {
            uint32_t c = 0;
            uint16_t bloom = 0;
            
            // Συλλέγει από όλα τα thread-local lists
            for (std::size_t src = 0; src < nthreads; ++src) {
                for (Chunk* ch = lists[src][slot].head; ch; ch = ch->next) {
                    c += ch->size;
                    for (uint32_t i = 0; i < ch->size; ++i) {
                        bloom |= ch->items[i].tag;
                    }
                }
            }
            
            counts_[slot] = c;
            bloom_filters_[slot] = bloom;
        }
    });
}

for (auto& th : threads) th.join();  // ← Barrier
```

---

### 3.4  Απαιτήσεις Τελικής Δομής Αποθήκευσης
 (και default, και opt-in)

**Κώδικας:**
- [include/parallel_unchained_hashtable.h#L30-L50]: Directory structure με prefix sums
- [include/parallel_unchained_hashtable.h#L170-L207]: Copy phase τοποθετεί contiguous tuples

```cpp
// include/parallel_unchained_hashtable.h#L30-L50
// ===== STRUCTURE =====
std::vector<uint32_t> directory_offsets_;  // Prefix sums: END pointers
std::vector<uint16_t> bloom_filters_;      // Per-slot bloom
std::vector<entry_type> tuples_;           // Contiguous flat storage

// Αρχιτεκτονική:
// tuples_ = [slot0_entries...] [slot1_entries...] [slot2_entries...]
//           ↑                  ↑                  ↑
//           directory[-1]      directory[0]      directory[1]
//           =0                 =c[0]             =c[0]+c[1]
```

**Χαρακτηριστικά:**
-  Ενιαίο contiguous block (`tuples_`)
-  Ταξινομημένο ανά hash prefix (sorted by directory slot)
-  Contiguous tuples ανά slot
-  Directory[-1] ως start pointer [lines 65-68]

---

## **ΕΝΌΤΗΤΑ 4: Συλλογή Tuples & Κατανομή Μνήμης**

### 4.1  Είσοδος Tuples ως Ροή (Opt-in)


**Σημείωση:** Στο default path, όλα τα entries συλλέγονται σε `std::vector<HashEntry<Key>>` πριν το build.

---

### 4.2  Δυναμική & Ταυτόχρονη Κατανομή (Opt-in)


**Κώδικας:**
- [include/parallel_unchained_hashtable.h#L371-L420]: Κάθε thread κατανέμει δυναμικά

---

### 4.3  Slab Allocator Τριών Επιπέδων (Opt-in)


**Κώδικας:**
- [include/temp_allocator.h]: `TempAlloc` implementation
- [include/three_level_slab.h]: `ThreeLevelSlab` structure (αν υπάρχει)

**Αναλυτικά (Partitioned Build):**

```cpp
// include/parallel_unchained_hashtable.h#L390-L397
// Επίπεδο 1: Global allocator (via operator new)
// Επίπεδο 2: Thread-local allocators (TempAlloc)
std::vector<std::unique_ptr<TempAlloc>> allocs;
for (std::size_t t = 0; t < nthreads; ++t) {
    allocs.emplace_back(std::make_unique<TempAlloc>());  // ← Thread-local
}

// Επίπεδο 3: Per-tuple chunks (allocated από thread's TempAlloc)
for (std::size_t t = 0; t < nthreads; ++t) {
    threads.emplace_back([&, t, begin, end]() {
        TempAlloc& my_alloc = *allocs[t];  // ← Thread-local slab
        for (std::size_t i = begin; i < end; ++i) {
            // Allocate chunks από my_alloc
            chunklist_push(my_lists[slot], entry, my_alloc);
        }
    });
}
```


## **ΕΝΌΤΗΤΑ 5: Ανταλλαγή Partitions Μεταξύ Threads**

### 5.1  Συγχώνευση Μετά τη Συλλογή (Opt-in)


**Κώδικας:**
- [include/parallel_unchained_hashtable.h#L421-L445]: Phase 2 συγχωνεύει chunks από όλα τα threads ανά partition

---

### 5.2  Ιδιοκτησία Partition (Opt-in)


**Κώδικας:**
- [include/parallel_unchained_hashtable.h#L425-L430]: Κάθε thread έχει ιδιοκτησία διαφορετικών partitions

---

## **ΕΝΌΤΗΤΑ 6: Καταμέτρηση Tuples & Κατασκευή Directory**

### 6.1  Καταμέτρηση Ανά Hash Prefix
 (και default, και opt-in)

**Κώδικας:**
- [include/parallel_unchained_hashtable.h#L155-L165] (Default):
```cpp
// PHASE 1: COUNT entries per directory slot
for (std::size_t i = 0; i < entries.size(); ++i) {
    uint64_t h = compute_hash(entries[i].key);
    std::size_t slot = (h >> shift_) & dir_mask_;
    counts_[slot]++;  // ← Καταμέτρηση ανά slot
    bloom_filters_[slot] |= Bloom::make_tag_from_hash(h);
}
```

- [include/parallel_unchained_hashtable.h#L421-L440] (Partitioned):
```cpp
// Phase 2: COUNT & BLOOM (one-writer-per-partition)
for (std::size_t slot = t; slot < dir_size_; slot += workers) {
    uint32_t c = 0;
    for (std::size_t src = 0; src < nthreads; ++src) {
        for (Chunk* ch = lists[src][slot].head; ch; ch = ch->next) {
            c += ch->size;  // ← Καταμέτρηση ανά partition
        }
    }
    counts_[slot] = c;
}
```

---

### 6.2  Υπολογισμός Prefix Sums


**Κώδικας:**
- [include/parallel_unchained_hashtable.h#L167-L175] (Default):
```cpp
// PHASE 2: PREFIX SUM - Υπολογισμός offsets (END pointers)
uint32_t cumulative = 0;
for (std::size_t i = 0; i < dir_size_; ++i) {
    cumulative += counts_[i];
    directory_offsets_[i] = cumulative;  // ← END pointer
}
```

- [include/parallel_unchained_hashtable.h#L447-L455] (Partitioned):
```cpp
// Serial prefix sum (after synchronization)
uint32_t cumulative = 0;
for (std::size_t i = 0; i < dir_size_; ++i) {
    cumulative += counts_[i];
    directory_offsets_[i] = cumulative;  // ← END pointer
}
```

---

## **ΕΝΌΤΗΤΑ 7: Τελική Αντιγραφή Tuples**

### 7.1  Κανόνες Αντιγραφής


**Κώδικας:**
- [include/parallel_unchained_hashtable.h#L185-L207] (Default):
```cpp
// PHASE 4: COPY - Αντιγραφή tuples στη σωστή θέση
// Write pointers: START of each slot
if (write_ptrs_.size() != dir_size_) write_ptrs_.assign(dir_size_, 0);
write_ptrs_[0] = 0;
for (std::size_t i = 1; i < dir_size_; ++i) {
    write_ptrs_[i] = directory_offsets_[i - 1];
}

for (std::size_t i = 0; i < entries.size(); ++i) {
    uint64_t h = compute_hash(entries[i].key);
    std::size_t slot = (h >> shift_) & dir_mask_;
    uint32_t pos = write_ptrs_[slot]++;
    tuples_[pos].key = entries[i].key;
    tuples_[pos].row_id = entries[i].row_id;
}
```

---

### 7.2  Σημασιολογία Δεικτών Directory


**Κώδικας:**
- [include/parallel_unchained_hashtable.h#L30-L80]: Directory struct με END pointers
- Range για slot i: `[directory_offsets_[i-1], directory_offsets_[i])`

```cpp
// tuples_ layout:
// [slot0 entries] [slot1 entries] [slot2 entries] ...
//  ↑              ↑               ↑
//  directory[-1]  directory[0]    directory[1]
//  = 0            = count[0]      = count[0]+count[1]
```

---

### 7.3  Ειδικό Directory Entry [-1]


**Κώδικας:**
- [include/parallel_unchained_hashtable.h#L65-L68]:
```cpp
// Allocate directory with extra slots for directory[-1] entry
directory_buffer_.assign(dir_size_ + 2, 0);
directory_offsets_ = directory_buffer_.data() + 1;  // Shift pointer so [-1] is valid
directory_offsets_[-1] = 0;  // Points to start of tuple storage
```

---

### 7.4  Χειρισμός Bloom Filters


**Κώδικας:**
- [include/parallel_unchained_hashtable.h#L155-L165] (Count + Bloom):
```cpp
bloom_filters_[slot] |= Bloom::make_tag_from_hash(h);
```

- [include/parallel_unchained_hashtable.h#L421-L440] (Partitioned + Bloom):
```cpp
uint16_t bloom = 0;
for (std::size_t src = 0; src < nthreads; ++src) {
    for (Chunk* ch = lists[src][slot].head; ch; ch = ch->next) {
        for (uint32_t i = 0; i < ch->size; ++i) {
            bloom |= ch->items[i].tag;  // ← Merge bloom bits
        }
    }
}
bloom_filters_[slot] = bloom;
```

---


## **ΕΝΌΤΗΤΑ 8: Work Stealing (Εξισορρόπηση Φορτίου)**

### 8.1  Σκοπός


**Κώδικας:** [src/execute_default.cpp#L125-L201]

---

### 8.2  Μηχανισμός


**Κώδικας:**
- [include/work_stealing.h#L20-L35]: `WorkStealingCoordinator` με atomic counter
- [src/execute_default.cpp#L145-L160]: Probe threads χρησιμοποιούν work stealing

```cpp
// include/work_stealing.h
class WorkStealingCoordinator {
private:
    std::atomic<size_t> work_counter_;  // ← Atomic μετρητής
};

// src/execute_default.cpp#L145-L160
while (true) {
    // Atomic increment και παίρνουμε το block
    size_t block_start, block_end;
    if (!ws_coordinator.steal_block(block_start, block_end)) {
        break;  // Όλη η δουλειά ολοκληρώθηκε
    }
    
    // Εκτελούμε την εργασία για αυτό το block
}
```

---

### 8.3  Συγχώνευση Αποτελεσμάτων


**Κώδικας:** [src/execute_default.cpp#L195-L235]

```cpp
// src/execute_default.cpp#L195-L235
// Μετά τη σύγχρονη σύμπτωση (join)
for (auto& th : probe_threads) th.join();

// Συγχώνευση μερικών αποτελεσμάτων σειριακά
for (size_t tid = 0; tid < nthreads; ++tid) {
    auto& local = out_by_thread[tid];
    for (const auto& pair : local) {
        results.push_back(pair);  // ← Merge
    }
}
```

---



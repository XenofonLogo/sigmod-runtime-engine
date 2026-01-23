## 🔴 ΜΕΡΟΣ 1ο: Βελτιστοποίηση Join Pipeline & Αλγόριθμοι Κατακερματισμού

### Σκοπός & Κίνητρο

Το αρχικό σύστημα χρησιμοποιούσε `std::unordered_map` για τις hash join λειτουργίες. Αυτή η δομή έχει:
- ❌ Σημαντική overhead από node allocations (κάθε entry είναι ξεχωριστό allocation)
- ❌ Κακή cache locality (chaining structure)
- ❌ Μη βέλτιστη utilization της CPU

**Λύση**: Υλοποίηση τριών υψηλής απόδοσης hash table implementations με βάση τις προδιαγραφές (Robin Hood, Hopscotch, Cuckoo).

---

### 1.1 Robin Hood Hashing

#### Τι Είναι

Variant της open addressing με βάση την ανοιχτή διευθυνσιοδότηση. Σε περίπτωση σύγκρουσης, γίνεται ανταλλαγή θέσεων με βάση το **Probe Sequence Length (PSL)** - την απόσταση κάθε entry από την ιδανική του θέση.

#### Αλγόριθμος

```cpp
// Εισαγωγή κλειδιού K με τιμή V
pos = hash(K) % size
distance = 0

while table[pos] is occupied:
    if PSL(table[pos]) < distance:
        // Ο νέος έχει πιο μεγάλη απόσταση
        // Άρπαξε τη θέση (Robin Hood!)
        swap K,V with table[pos]
        K = table[pos].key
        V = table[pos].value
    pos = (pos + 1) % size
    distance++

table[pos] = {K, V, distance}
```

#### Πλεονεκτήματα

✅ **Balanced PSL**: Εξισορροπεί τις αποστάσεις → καλύτερη worst-case performance  
✅ **Απλή υλοποίηση**: Δεν χρειάζεται δυναμική resizing  
✅ **Cache-friendly**: Linear probing → sequential memory access  
✅ **Predictable**: O(1) average case

#### Υλοποίηση

**Αρχείο**: `include/robinhood_hashtable.h`

```cpp
template <typename Key>
class RobinHoodHashTable {
    struct Entry {
        Key key;
        uint32_t row_id;
        uint16_t psl;  // Probe Sequence Length
    };
    
    std::vector<Entry> table_;
    std::vector<bool> occupied_;
    
    void insert(const Key& key, uint32_t row_id) {
        size_t pos = hash(key) % table_.size();
        uint16_t distance = 0;
        
        Key k = key;
        uint32_t rid = row_id;
        
        while (occupied_[pos]) {
            if (table_[pos].psl < distance) {
                std::swap(k, table_[pos].key);
                std::swap(rid, table_[pos].row_id);
                std::swap(distance, table_[pos].psl);
            }
            pos = (pos + 1) % table_.size();
            distance++;
        }
        
        table_[pos] = {k, rid, distance};
        occupied_[pos] = true;
    }
};
```


### 1.2 Hopscotch Hashing

#### Τι Είναι

Hash table που χρησιμοποιεί **neighborhood-based approach**. Κάθε θέση έχει ένα **bitmap (hop-information)** που δείχνει ποιες θέσεις στην ίδια cache line περιέχουν entries που belong σε αυτήν τη θέση.

#### Neighborhood Concept

```
Initial Position (h):    Neighborhood (size H, typically 32):
┌─────────────────┐      ┌───┬───┬───┬───┬───┬───┬───┬───┐
│ Index h         │ ───> │ 0 │ 1 │ 2 │ 3 │ 4 │ 5 │ 6 │ 7 │  ...
└─────────────────┘      └───┴───┴───┴───┴───┴───┴───┴───┘
                            ↑  (Hop bitmap shows which ones)
                            belong to h
```

#### Αλγόριθμος

```cpp
insert(key K):
    h = hash(K) % size
    
    // Βρες κενή θέση
    for pos in h to h+MAX_HOPS:
        if table[pos] is empty:
            // Βάλε το K εκεί
            table[pos] = K
            hop_info[h] |= (1 << (pos - h))  // Update bitmap
            return
    
    // Αν δεν βρέθηκε κενή θέση, κάνε resizing
    resize()
```

#### Πλεονεκτήματα

✅ **Γρήγορη αναζήτηση**: Bitmap tells exactly where to look  
✅ **Cache efficiency**: Everything in one cache line (64 bytes → ~8 entries)  
✅ **Deterministic bounds**: Can't exceed H hops

#### Μειονεκτήματα

❌ **Insertion complexity**: May need to shift many entries  
❌ **Resizing overhead**: Frequent resizing if neighborhood full  
❌ **Limited load factor**: ~85% max capacity

#### Υλοποίηση

**Αρχείο**: `include/hopscotch_hashtable.h`

```cpp
template <typename Key>
class HopscotchHashTable {
    static constexpr size_t NEIGHBORHOOD_SIZE = 32;
    
    struct Entry {
        Key key;
        uint32_t row_id;
        uint32_t hop_info;  // Bitmap of neighborhood
    };
    
    std::vector<Entry> table_;
    
    void insert(const Key& key, uint32_t row_id) {
        size_t h = hash(key) % table_.size();
        
        // Βρες κενή θέση εντός neighborhood
        for (size_t i = 0; i < NEIGHBORHOOD_SIZE; i++) {
            size_t pos = h + i;
            if (table_[pos].key == EMPTY) {
                table_[pos] = {key, row_id, 0};
                table_[h].hop_info |= (1 << i);
                return;
            }
        }
        
        // Neighborhood full → resize
        resize();
    }
};
```


### 1.3 Cuckoo Hashing

#### Τι Είναι

Χρησιμοποιεί **δύο πίνακες** και **δύο συναρτήσεις κατακερματισμού** (h₁, h₂). Κάθε κλειδί έχει ακριβώς δύο πιθανές θέσεις:

```
Table 1:  ┌─────┬─────┬─────┬─────┐
          │  A  │  B  │  C  │  D  │
          └─────┴─────┴─────┴─────┘
           h₁(x) positions

Table 2:  ┌─────┬─────┬─────┬─────┐
          │  E  │  A  │  F  │  G  │
          └─────┴─────┴─────┴─────┘
           h₂(x) positions
           
A is in table[1][h₁(A)] AND table[2][h₂(A)]
```

#### Αλγόριθμος

```cpp
insert(key K, value V):
    pos1 = h1(K) % size
    
    if table1[pos1] is empty:
        table1[pos1] = V
        return
    
    // Displace occupant
    swap K,V with table1[pos1]
    
    // Try table 2
    pos2 = h2(K) % size
    if table2[pos2] is empty:
        table2[pos2] = V
        return
    
    swap K,V with table2[pos2]
    
    // Repeat for up to MAX_ITERATIONS
    for i in 1 to MAX_ITERATIONS:
        pos1 = h1(K) % size
        if table1[pos1] is empty:
            table1[pos1] = V
            return
        
        swap K,V with table1[pos1]
        pos2 = h2(K) % size
        
        if table2[pos2] is empty:
            table2[pos2] = V
            return
        
        swap K,V with table2[pos2]
    
    // Failed → resize
    resize()
```

#### Πλεονεκτήματα

✅ **O(1) worst-case guarantee**: Always finds entry in ≤ 2 lookups  
✅ **Small table size**: Both tables < 2n total capacity  
✅ **Predictable**: No collision chain traversal

#### Μειονεκτήματα

❌ **Eviction chains**: Can trigger cascade of displacements  
❌ **Load factor limit**: ~50% max before resizing  
❌ **Insertion cost**: Average O(1) but bad worst case with chains  
❌ **Two hash functions**: More complex implementation

#### Υλοποίηση

**Αρχείο**: `include/cuckoo_hashtable.h`

```cpp
template <typename Key>
class CuckooHashTable {
    struct Entry {
        Key key;
        uint32_t row_id;
    };
    
    std::vector<Entry> table1_, table2_;
    static constexpr size_t MAX_ITERATIONS = 100;
    
    void insert(const Key& key, uint32_t row_id) {
        Key k = key;
        uint32_t rid = row_id;
        
        for (size_t iter = 0; iter < 2 * MAX_ITERATIONS; iter++) {
            size_t pos1 = hash1(k) % table1_.size();
            if (table1_[pos1].key == EMPTY) {
                table1_[pos1] = {k, rid};
                return;
            }
            
            std::swap(k, table1_[pos1].key);
            std::swap(rid, table1_[pos1].row_id);
            
            size_t pos2 = hash2(k) % table2_.size();
            if (table2_[pos2].key == EMPTY) {
                table2_[pos2] = {k, rid};
                return;
            }
            
            std::swap(k, table2_[pos2].key);
            std::swap(rid, table2_[pos2].row_id);
        }
        
        resize_and_rehash();
    }
};
```


### Σύγκριση Αλγορίθμων Μέρους 1 - ΜΕΤΡΗΜΕΝΑ ΑΠΟΤΕΛΕΣΜΑΤΑ

| # | Υλοποίηση | Runtime (sec) | Βελτίωση vs Baseline (%) |
|---|-----------|---------------|-----------------------------|--------------------------|
| 0 | unordered_map (Baseline) | 242.85 | – |
| 1A | Robin Hood Hashing | 233.25 | 4.0% |
| 1B | Cuckoo Hashing | 236.54 | 2.6% |
| 1C | Hopscotch Hashing | 238.05 | 2.0% |
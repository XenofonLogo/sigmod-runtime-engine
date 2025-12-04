οδηγιες εκτελεσης unit_tests
g++ -std=c++20 -O2 tests/test_unchained_hashtable.cpp -Iinclude -o test_unchained_hashtable
./test_unchained_hashtable

Εισαγωγή
Η παρούσα υλοποίηση επεκτείνει τον query engine και
 ενσωματώνει έναν πλήρως βελτιστοποιημένο Unchained Hash Table
  για επιτάχυνση των hash joins. 
  Ο στόχος είναι η ελαχιστοποίηση των cache misses
   και η δημιουργία ενός layout που προσεγγίζει το paper της εργασίας.

Υλοποιήθηκαν:

Γρήγορος Fibonacci hashing για INT32 keys

Προϋπολογισμένος πίνακας 16-bit popcount (65536 entries)

Συμπαγής Unchained Hashtable με prefix directory

4-bit Bloom filters ανά directory bucket

Αποδοτικό build-phase και probe-phase

Μικρές αλλαγές στο execute.cpp για να χρησιμοποιηθεί ο νέος πίνακας

Το σύστημα έχει πλήρη unit tests και επαληθεύτηκε με large-scale και heavy-collision δοκιμές.

Δομή Αρχείων

include/
unchained_hashtable.h
bloom_filter.h
hash_functions.h

src/
execute.cpp

tests/
test.cpp

Αναλυτική Περιγραφή Σχεδιαστικών Επιλογών

3.1 hash_functions.h

Στόχος:
Παροχή εξαιρετικά γρήγορης hash function για INT32 join keys.

Υλοποίηση:

Επιλέχθηκε ο Fibonacci hashing:
h(x) = uint64_t(x) * 11400714819323198485ULL

Είναι ταχύτερος από CRC32 και έχει πολύ καλή κατανομή.

Ο CRC32 παραμένει διαθέσιμος, αλλά δεν είναι default.

Υποστηρίζει εύκολη αλλαγή του hasher μέσω template παραμέτρων στο UnchainedHashTable.

Γιατί αυτή η επιλογή:

Οι join στήλες είναι INT32, άρα δεν χρειάζεται γενική hash function.

Ο Fibonacci hashing είναι branchless και πολύ γρήγορος.

Είναι η ίδια αρχή που χρησιμοποιείται σε μεγάλα συστήματα (Java, Linux kernel).

3.2 bloom_filter.h

Στόχος:
Απόρριψη μη σχετικών tuples ήδη στο επίπεδο directory bucket χωρίς κόστος.

Υλοποίηση:

Precomputed popcount 16-bit (64KB table, O(1) lookups)

Tag με 4 bits ανά tuple (παράγονται από hash στα bits 4, 12, 20, 28)

Τα Bloom filters είναι 16-bit μάσκες ανά bucket

Ο έλεγχος γίνεται με:
(bloom & tag) == tag

Γιατί αυτή η επιλογή:

Το directory έχει ελάχιστο overhead.

Το bloom είναι branchless και πάρα πολύ γρήγορο.

Μειώνει σημαντικά το probe cost σε μη ταιριαστά keys.

Ακριβώς όπως περιγράφει το paper της εργασίας.

3.3 unchained_hashtable.h

Στόχος:
Υλοποίηση hash table χωρίς λίστες (unchained), χωρίς pointer chasing, με layout που μεγιστοποιεί locality.

Υλοποίηση:

3.3.1 Directory (prefix table)

Προκύπτει από τα υψηλά bits του hash:
prefix = (h >> 16) & dir_mask

Κάθε bucket έχει:
begin_idx
end_idx
bloom (uint16_t)

3.3.2 Contiguous buffer of tuples

Ολόκληρη η δομή των tuples βρίσκεται σε ένα μεγάλο vector.

Δεν υπάρχουν malloc ανά bucket.

Η πρόσβαση σε όλα τα tuples που έχουν το ίδιο prefix είναι συνεχόμενη.

3.3.3 Build-phase (3 περάσματα)

Υπολογισμός prefix counts

Prefix offsets

Γέμισμα των tuples στο σωστό prefix range + ενημέρωση bloom

3.3.4 Probe-phase

Υπολογίζουμε hash

Βρίσκουμε bucket μέσω prefix

Bloom filter reject (πολύ συχνό)

Επιστρέφουμε pointer + length στη σωστή περιοχή tuples

3.3.5 Exact match
Το hashtable δεν συγκρίνει ίδια τα keys. Αυτό γίνεται στον JoinOperator. Το paper περιγράφει ότι η σωστή μοντελοποίηση είναι:

Το directory επιτρέπει τον γρήγορο εντοπισμό του candidate range

Η ήδη μικρή λίστα συγκρίνεται από τον caller

Γιατί αυτή η επιλογή:

Καμία pointer-based δομή (όπως std::unordered_map)

Καλύτερη locality

Πολύ καλύτερη συμπεριφορά όταν υπάρχουν πολλά duplicate keys

Αμελητέο memory overhead

Πλήρως ευθυγραμμισμένο με τις αρχές του paper

3.4 execute.cpp (JoinAlgorithm integration)

Στόχος:
Αντικατάσταση της unordered_map από custom UnchainedHashTable.

Υλοποίηση:

Η πλευρά build μετατρέπει τις εγγραφές σε:
vector<pair<key,row_id>>

Το hashtable χτίζεται με build_from_entries

Η πλευρά probe καλεί:
probe(key,len)

Για κάθε candidate entry επιβεβαιώνεται το key match και δημιουργείται το join output row

Γιατί αυτή η επιλογή:

Το join operator παραμένει συμβατός με το αρχικό API.

Δεν αλλάζει ο execution engine.

Όλη η επιτάχυνση γίνεται στο hashing layer.

Το join πλέον δεν κάνει dynamic allocations ούτε hash lookups στο unordered_map.

3.5 test.cpp (unit tests)

Στόχος:
Δημιουργία απλών, compiler-only tests χωρίς frameworks.

Δοκιμές:

Bloom tag correctness

Bloom maybe_contains correctness

False positive rate σε 16-bit bloom

Fibonacci hashing correctness & distribution uniformity

Build + probe basic

Heavy collision buckets (όλα τα keys ίδιο prefix)

Large scale test (100k tuples)

Αποτέλεσμα:
Όλες οι δοκιμές περνούν.



/* PART 1 */
# Late Materialization (LM) – Αναλυτική Τεκμηρίωση

Το Late Materialization (LM) είναι τεχνική εκτέλεσης ερωτημάτων που επιτρέπει στο σύστημα να *μην υλοποιεί* (materialize) ακριβές τιμές τύπου `VARCHAR` μέχρι να χρειαστεί πραγματικά.  
Αντί να αντιγράφονται strings κατά τη διάρκεια των scans και joins, το σύστημα χρησιμοποιεί compact references (`PackedStringRef` / `StringRef`).
Αυτό μειώνει την κατανάλωση μνήμης και βελτιώνει σημαντικά την απόδοση.

# Βασική Ιδέα

# INT32 τιμές → Υλοποιούνται άμεσα  
# VARCHAR τιμές → *ΔΕΝ υλοποιούνται*

Αντί για string, αποθηκεύεται μία 64-bit packed αναφορά που περιλαμβάνει:
- table_id  
- column_id  
- page_id  
- offset  
- flags (null, long-string)

Η πραγματική συμβολοσειρά ανασύρεται μόνο στην τελική φάση.

#  Δομές LM
## LM_Table
Αναπαριστά έναν πίνακα σε column-store μορφή.  
Έχει πολλές στήλες (`LM_Column`), κάθε μία με πολλές σελίδες (pages).

## LM_Column
- `is_int = true` → `int_pages`  
- `is_int = false` → `str_pages`

## LM_IntPage / LM_VarcharPage
Αποθηκεύουν πραγματικές τιμές:
- `std::vector<int32_t>`
- `std::vector<std::string>`

# Scanning: `scan_to_rowstore()`
Η συνάρτηση:
scan_to_rowstore(Catalog&, table_id, col_ids)


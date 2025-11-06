* # 📖 Εργασία: Ανάπτυξη Λογισμικού για Πληροφοριακά Συστήματα (1ο Μέρος)
[![Review Assignment Due Date](https://classroom.github.com/assets/deadline-readme-button-22041afd0340ce965d47ae6ef1cefeee28c7c493a6346c4f15d667ab976d596c.svg)](https://classroom.github.com/a/gjaw_qSU)
<div align="right">
  [![Software Tester](https://github.com/uoa-k23a/k23a-2025-d1-runtimeerror/actions/workflows/software_tester.yml/badge.svg)](https://github.com/uoa-k23a/k23a-2025-d1-runtimeerror/actions/workflows/software_tester.yml)
</div>

  ## 👥 Μέλη Ομάδας

  * **Ξενοφών Λογοθέτης** - (Email) - `1115202100087`
  * **Σακκέτος Γεώργιος** - sdi2000177@di.uoa.gr - `1115202000177`
  * **(Ονωματεπώνυμο Μέλους 3)** - (Email) - `ΑΜ`

---

## Εκτέλεση

##### Οι αρχική υλοποίηση που τρέχει σαν default

```bash
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release -DEXECUTE_IMPL=default -Wno-dev
cmake --build build -- -j $(nproc)
```

ή

```bash
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release -Wno-dev
cmake --build build -- -j $(nproc)
```

##### Οι τρείς υλοποιήσεις

```bash
# Για την 'robinhood'
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release -DEXECUTE_IMPL=robinhood -Wno-dev
cmake --build build -- -j $(nproc)

# Για την 'hopscotch'
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release -DEXECUTE_IMPL=hopscotch -Wno-dev
cmake --build build -- -j $(nproc)

# Για την 'cuckoo'
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release -DEXECUTE_IMPL=cuckoo -Wno-dev
cmake --build build -- -j $(nproc)
```

> ***Σημείωση:*** Το υπόλοιπο της εκτέλεσης είναι ίδιο

---

## 1. Αλγόριθμος Κατακερματισμού Robin Hood

* **Υλοποιήθηκε από:** ...

---

## 2. Αλγόριθμος Κατακερματισμού Hopscotch

* **Υλοποιήθηκε από:** **Ξενοφών Λογοθέτης**

### 2.1. Αντικατάσταση `std::unordered_map` με `tsl::hopscotch_map`

Η βασική αλλαγή αφορά τη δομή του hash table που χρησιμοποιείται στον αλγόριθμο join. Αντί για `std::unordered_map`, χρησιμοποιείται πλέον η `tsl::hopscotch_map`.

### 2.2. Προσθήκη Βοηθητικής Συνάρτησης `try_normalize`

Προστέθηκε η `inline` συνάρτηση `try_normalize`, η οποία επιχειρεί να "ομαλοποιήσει" το κλειδί (join key) σε ενιαίο τύπο `T`.

**Λειτουργία:**

* Επιστρέφει `std::optional<T>`, το οποίο είναι `nullopt` αν το key δεν μπορεί να μετατραπεί με ασφάλεια.
* Υποστηρίζει:
  * Μετατροπή αριθμητικών τύπων (int, double).
  * Μετατροπή αριθμών σε string (αν ο join key είναι VARCHAR).
  * Αγνόηση `std::monostate` (NULL τιμές).

**Στόχος:**

* Αποφυγή exceptions κατά τη σύγκριση διαφορετικών τύπων.
* Καλύτερη ανθεκτικότητα σε heterogenous data sets.
* Επέκταση συμβατότητας join πεδίων διαφορετικού τύπου.

### 2.3. Αφαίρεση Ρίψης Exceptions για Type Mismatch

Στην αρχική έκδοση, κάθε ασυμβατότητα τύπου προκαλούσε `throw std::runtime_error("wrong type of field")`.

**Πλέον:**

* Οι ασύμβατες ή null τιμές αγνοούνται απλά.
* Το join συνεχίζει κανονικά χωρίς διακοπή.
* Αυτό βελτιώνει τη σταθερότητα σε mixed ή ελλιπή δεδομένα.

### 2.4. Βελτιώσεις στη Φάση Build / Probe

Η βασική λογική παρέμεινε ίδια, αλλά:

* Οι φάσεις build και probe χρησιμοποιούν τη συνάρτηση `try_normalize`.
* Τα κλειδιά αποθηκεύονται μόνο αν έχουν έγκυρη τιμή (όχι `monostate`).
* Ελαχιστοποιήθηκε ο επαναλαμβανόμενος κώδικας και ο έλεγχος τύπων.

> **Σημείωση:** Καμία αλλαγή δεν έγινε στη λογική του Join ή στα δεδομένα εξόδου.

### 2.5. Απόδοση

> **Συνολικός χρόνος εκτέλεσης των queries:** `737201 ms`
> (Προηγούμενος χρόνος: `493646 ms`)

---

## 3. Αλγόριθμος Κατακερματισμού Cuckoo

* **Υλοποιήθηκε από: Σακκέτος Γεώργιος**

### 3.1. Αντικατάσταση `std::unordered_map` με `libcuckoo::cuckoohash_map`

  Η βασική δομή `std::unordered_map` της αρχικής υλοποίησης αντικαταστάθηκε με τη `libcuckoo::cuckoohash_map` για την κατασκευή του hash table.

### 3.2. Προσθήκη Βοηθητικής Συνάρτησης `try_normalize`

  Προστέθηκε μια βοηθητική συνάρτηση `try_normalize`, η οποία επιχειρεί να "ομαλοποιήσει" (normalize) το κλειδί (join key) στον απαιτούμενο τύπο `T` του hash table.

  **Λειτουργία:**

* Επιστρέφει `std::optional<T>`, η οποία τιμή είναι `nullopt` εάν η μετατροπή του κλειδιού (join key) αποτύχει.
* Υποστηρίζει:

  * Μετατροπή μεταξύ αριθμητικών τύπων (π.χ. `int` σε `double`).
  * Μετατροπή αριθμητικών τιμών σε `string` εάν ο τύπος του κλειδού (join key) είναι `VARCHAR`.
  * Αγνόηση `std::monostate` (NULL values), επιστρέφοντας `nullopt`.

  **Στόχος:**
* Αποφυγή exceptions κατά τη σύγκριση διαφορετικών τύπων.
* Καλύτερη ανθεκτικότητα σε δεδομένα με διαφορετικούς τύπους (heterogeneous data).
* Βελτιωμένη συμβατότητα μεταξύ πεδίων join διαφορετικού τύπου.

### 3.3. Αφαίρεση Ρίψης Exceptions για Type Mismatch

  Στην αρχική έκδοση, οποιαδήποτε ασυμβατότητα τύπου (type mismatch) που δεν ήταν `std::monostate` προκαλούσε `throw std::runtime_error("wrong type of field")`.

  **Πλέον:**

* Η `try_normalize` διαχειρίζεται τις ασυμβατότητες επιστρέφοντας `nullopt`.
* Εάν ένα κλειδί είναι `std::monostate` ή δεν μπορεί να μετατραπεί, η εγγραφή αυτή απλώς αγνοείται, επιτρέποντας στο join να συνεχίσει χωρίς διακοπή.

### 3.4. Προσαρμογές στη Φάση Build / Probe

  Η λογική παραμένει ίδια, αλλά με τις εξείς αλλαγές:

* **Build Phase:** Η εισαγωγή στο hash table δεν είναι πλέον μια απλή ενέργεια. Απαιτεί τα εξής βήματα:
  1. Το κλειδί ομαλοποιείται με την `try_normalize`.
  2. Γίνεται έλεγχος με `hash_table.find()` για να δούμε αν το κλειδί υπάρχει ήδη.
  3. **Αν βρεθεί:** Ο υπάρχων `vector` ανακτάται, ενημερώνεται με το νέο index, και καλείται η `hash_table.update()` για να αντικαταστήσει την παλιά τιμή.
  4. **Αν δεν βρεθεί:** Ένας νέος `vector` δημιουργείται και καλείται η `hash_table.insert()` για τη νέα εγγραφή.
* **Probe Phase:** Χρησιμοποιεί επίσης την `try_normalize` πριν κάνει `find` στο hash table.

### 3.5. Αποτελέσματα Απόδοσης

> **Συνολικός χρόνος εκτέλεσης των queries:** `768494 ms`
> (Προηγούμενος χρόνος: `433537 ms`)

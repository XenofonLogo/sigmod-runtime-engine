#ifndef PARTITION_HASH_BUILDER_H
#define PARTITION_HASH_BUILDER_H

#include <cstdint>
#include <vector>
#include <thread>
#include <memory>
#include "slab_allocator.h"

namespace Contest {

// Προσωρινή εγγραφή για partitioned χτίσιμο hash table (Επίπεδο 3: Per-partition data)
template<typename Key>
struct TmpEntry {
    Key key;         // Κλειδί
    uint32_t row_id; // Γραμμή προέλευσης
    uint16_t tag;    // Bloom tag για γρήγορη απόρριψη
};

// Chunk σταθερού μεγέθους για αποδοτική δέσμευση (Επίπεδο 3: Per-partition allocation unit)
template<typename Key>
struct Chunk {
    static constexpr uint32_t kChunkCap = 256;
    
    Chunk* next;                     // Επόμενο chunk στη λίστα
    uint32_t size;                   // Πλήθος στοιχείων στο chunk
    TmpEntry<Key> items[kChunkCap];  // Αποθηκευμένες εγγραφές
};

// Συνδεδεμένη λίστα chunks ανά partition (Επίπεδο 3: Per-partition tuple collection)
template<typename Key>
struct ChunkList {
    Chunk<Key>* head = nullptr;
    Chunk<Key>* tail = nullptr;
};

// Δέσμευση νέου chunk από την partition arena (Επίπεδο 3: Partition-specific allocation)
// Παίρνει μνήμη από το PartitionArena αντί απευθείας από thread allocator
template<typename Key>
inline Chunk<Key>* alloc_chunk_from_partition(PartitionArena& partition_arena) {
    void* mem = partition_arena.alloc(sizeof(Chunk<Key>), alignof(Chunk<Key>));
    auto* c = new (mem) Chunk<Key>();
    c->next = nullptr;
    c->size = 0;
    return c;
}

// Δέσμευση νέου chunk από τον thread allocator (fallback, για backward compatibility)
template<typename Key>
inline Chunk<Key>* alloc_chunk(SlabAllocator& alloc) {
    void* mem = alloc.alloc(sizeof(Chunk<Key>), alignof(Chunk<Key>));
    auto* c = new (mem) Chunk<Key>();
    c->next = nullptr;
    c->size = 0;
    return c;
}

// Προσθήκη εγγραφής σε λίστα chunks με partition arena (Επίπεδο 3: Chunk collection)
// Δημιουργεί νέο chunk αν γεμίσει το τρέχον, παίρνοντας μνήμη από partition arena
template<typename Key>
inline void chunklist_push_from_partition(ChunkList<Key>& list, const TmpEntry<Key>& e, 
                                          PartitionArena& partition_arena) {
    constexpr uint32_t kChunkCap = Chunk<Key>::kChunkCap;
    
    if (!list.tail || list.tail->size == kChunkCap) {
        Chunk<Key>* c = alloc_chunk_from_partition<Key>(partition_arena);
        if (!list.head) list.head = c;
        else list.tail->next = c;
        list.tail = c;
    }
    list.tail->items[list.tail->size++] = e;
}

// Προσθήκη εγγραφής σε λίστα chunks με thread allocator (fallback)
template<typename Key>
inline void chunklist_push(ChunkList<Key>& list, const TmpEntry<Key>& e, SlabAllocator& alloc) {
    constexpr uint32_t kChunkCap = Chunk<Key>::kChunkCap;
    
    if (!list.tail || list.tail->size == kChunkCap) {
        Chunk<Key>* c = alloc_chunk<Key>(alloc);
        if (!list.head) list.head = c;
        else list.tail->next = c;
        list.tail = c;
    }
    list.tail->items[list.tail->size++] = e;
}

} // namespace Contest

#endif // PARTITION_HASH_BUILDER_H

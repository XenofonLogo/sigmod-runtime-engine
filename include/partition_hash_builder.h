#ifndef PARTITION_HASH_BUILDER_H
#define PARTITION_HASH_BUILDER_H

#include <cstdint>
#include <vector>
#include <thread>
#include <memory>
#include "temp_allocator.h"

namespace Contest {

// Temporary entry for partitioned hash building
template<typename Key>
struct TmpEntry {
    Key key;
    uint32_t row_id;
    uint16_t tag;  // Bloom filter tag
};

// Fixed-size chunk for efficient allocation
template<typename Key>
struct Chunk {
    static constexpr uint32_t kChunkCap = 256;
    
    Chunk* next;
    uint32_t size;
    TmpEntry<Key> items[kChunkCap];
};

// Linked list of chunks for each partition
template<typename Key>
struct ChunkList {
    Chunk<Key>* head = nullptr;
    Chunk<Key>* tail = nullptr;
};

// Allocate a new chunk from the temp allocator
template<typename Key>
inline Chunk<Key>* alloc_chunk(TempAlloc& alloc) {
    void* mem = alloc.alloc(sizeof(Chunk<Key>), alignof(Chunk<Key>));
    auto* c = new (mem) Chunk<Key>();
    c->next = nullptr;
    c->size = 0;
    return c;
}

// Push an entry to the chunk list
template<typename Key>
inline void chunklist_push(ChunkList<Key>& list, const TmpEntry<Key>& e, TempAlloc& alloc) {
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

#pragma once

#include <vector>
#include <map>
#include <algorithm>
#include <cstdint>
#include <functional>
#include <cmath>
#include <stdexcept>
#include <typeinfo>
#include <iostream>

#include "hash_common.h"

namespace Contest {

static constexpr size_t NEIGHBORHOOD_SIZE = 8;

template<typename Key>
struct HopscotchSlot {
    uint64_t ckey;        // <-- composite key
    size_t start_index = 0;
    size_t count = 0;
    bool is_valid = false;
};

template<typename Key>
class HopscotchBackend {
private:
    using Entry = HashEntry<Key>;
    using Slot  = HopscotchSlot<Key>;

    std::vector<Entry> _storage;
    std::vector<Slot>  _table;
    std::vector<uint32_t> _hop_map;

    size_t _capacity = 0;
    size_t _initial_capacity = 0;

   
    // Composite key generator
    
    inline uint64_t make_ckey(const Key& k) const {
        // ID for type namespace (ensures different tables don't mix)
        static uint64_t tid = (std::hash<std::string>()(typeid(Key).name()) & 0xFFFFULL);

        return (tid << 48) | (uint64_t)k;
    }

    inline size_t hash_ckey(uint64_t ck) const {
        return ck % _capacity;
    }

    size_t distance(size_t i, size_t j) const {
        if (j >= i) return j - i;
        return j + (_capacity - i);
    }

    
    // Move empty slot closer
    
    size_t move_slot_closer(size_t home_slot, size_t empty_slot) {
        for (size_t offset = 1; offset < NEIGHBORHOOD_SIZE; ++offset) {

            size_t candidate_slot = (empty_slot + _capacity - offset) % _capacity;
            const Slot& cand = _table[candidate_slot];
            if (!cand.is_valid) continue;

            size_t cand_home = hash_ckey(cand.ckey);
            size_t hop_off   = distance(cand_home, candidate_slot);
            if (hop_off >= NEIGHBORHOOD_SIZE) continue;

            uint32_t mask = (1u << hop_off);
            if (!(_hop_map[cand_home] & mask)) continue;

            size_t new_hop_offset = distance(cand_home, empty_slot);
            if (new_hop_offset < NEIGHBORHOOD_SIZE) {

                _hop_map[cand_home] &= ~mask;
                _table[empty_slot] = _table[candidate_slot];
                _table[empty_slot].is_valid = true;
                _hop_map[cand_home] |= (1u << new_hop_offset);

                _table[candidate_slot] = Slot{};
                _table[candidate_slot].is_valid = false;

                return candidate_slot;
            }
        }
        return (size_t)-1;
    }

    
    // Insert
    
    bool insert_slot(const Slot& info) {
        size_t home = hash_ckey(info.ckey);

        size_t empty_slot = home;
        size_t count = 0;

        while (_table[empty_slot].is_valid && count < _capacity) {
            empty_slot = (empty_slot + 1) % _capacity;
            count++;
        }
        if (count == _capacity) return false;

        while (distance(home, empty_slot) >= NEIGHBORHOOD_SIZE) {
            size_t new_empty = move_slot_closer(home, empty_slot);
            if (new_empty == (size_t)-1) return false;
            empty_slot = new_empty;
        }

        _table[empty_slot] = info;
        _table[empty_slot].is_valid = true;

        size_t off = distance(home, empty_slot);
        _hop_map[home] |= (1u << off);

        return true;
    }

public:
    HopscotchBackend() = default;

    
    // Build
    
    void build_from_entries(const std::vector<std::pair<Key, size_t>>& entries) {
        if (entries.empty()) {
            _capacity = 0; _table.clear(); _hop_map.clear(); _storage.clear();
            return;
        }

        std::map<Key,std::vector<size_t>> groups;
        for (auto& p : entries) groups[p.first].push_back(p.second);

        _storage.clear(); _storage.reserve(entries.size());

        std::vector<Slot> slots; slots.reserve(groups.size());
        size_t idx = 0;

        for (auto& g : groups) {
            Slot s;
            s.ckey = make_ckey(g.first);
            s.start_index = idx;
            s.count = g.second.size();
            s.is_valid = true;

            slots.push_back(s);

            for (auto rid : g.second)
                _storage.push_back({g.first, rid});

            idx += g.second.size();
        }

        double load_factor = 0.45;
        _initial_capacity = std::ceil(slots.size() / load_factor);
        if (_initial_capacity < 16) _initial_capacity = 16;

        _capacity = _initial_capacity;

        const size_t HARD_CAP = slots.size() * 128 + 100000;

        while (true) {
            _table.assign(_capacity, Slot{});
            _hop_map.assign(_capacity, 0);

            bool ok = true;
            for (auto& s : slots)
                if (!insert_slot(s)) { ok = false; break; }

            if (ok) return;

            
            _capacity *= 2;
        }
    }

   
    // Probe
  
    std::pair<const Entry*, size_t> probe(const Key& k) const {
        if (_capacity == 0) return {nullptr, 0};

        uint64_t ck = make_ckey(k);
        size_t home = hash_ckey(ck);

        uint32_t bitmap = _hop_map[home];

        for (size_t off = 0; off < NEIGHBORHOOD_SIZE; ++off) {
            if (bitmap & (1u << off)) {
                size_t slot = (home + off) % _capacity;
                const Slot& s = _table[slot];

                if (s.is_valid && s.ckey == ck)
                    return { &_storage[s.start_index], s.count };
            }
        }
        return {nullptr, 0};
    }
};

} // namespace Contest
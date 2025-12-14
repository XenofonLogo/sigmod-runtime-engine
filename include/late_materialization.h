#pragma once
#include <cstdint>
#include <vector>
#include <string>
#include <cstring>
#include <functional> 
#include <tuple>

// Forward declaration
struct Plan;

namespace Contest {

// ----------------------------------------------------------------------------
// 1. PackedStringRef (64-bit reference to string data)
// ----------------------------------------------------------------------------
struct PackedStringRef {
    union {
        uint64_t raw;
        struct {
            uint64_t slot_idx : 16;
            uint64_t page_idx : 32;
            uint64_t col_id   : 8;
            uint64_t table_id : 8;
        } parts;
    };

    PackedStringRef() : raw(0) {}
    PackedStringRef(uint64_t r) : raw(r) {}
    PackedStringRef(uint8_t tid, uint8_t cid, uint32_t pid, uint16_t sid) {
        raw = 0;
        parts.table_id = tid;
        parts.col_id = cid;
        parts.page_idx = pid;
        parts.slot_idx = sid;
    }

    static PackedStringRef unpack(uint64_t val) { return PackedStringRef(val); }
    
    bool operator==(const PackedStringRef& other) const { return raw == other.raw; }
};

// 2. StringRef alias (για συμβατότητα με υπάρχοντα κώδικα)
typedef PackedStringRef StringRef; 

// 3. Hash & Equality functors
// forward-declare resolver used by hash/eq
struct StringRefResolver;

struct StringRefHash {
    const Plan* plan;
    StringRefHash(const Plan* p) : plan(p) {} 
    size_t operator()(const PackedStringRef& k) const;
};

struct StringRefEq {
    const Plan* plan;
    StringRefEq(const Plan* p) : plan(p) {}
    bool operator()(const PackedStringRef& a, const PackedStringRef& b) const;
};

// 4. value_t (compact 64-bit payload)
//
// We store a single 64-bit word per value. Column-level `DataType` identifies
// how to interpret the payload. A reserved payload of `UINT64_MAX` encodes NULL.
struct value_t {
    uint64_t raw;

    static value_t make_i32(int32_t v) { value_t x; x.raw = static_cast<uint64_t>(static_cast<int64_t>(v)); return x; }
    static value_t make_i64(int64_t v) { value_t x; x.raw = static_cast<uint64_t>(v); return x; }
    static value_t make_f64(double v) { value_t x; uint64_t bits; std::memcpy(&bits, &v, sizeof(bits)); x.raw = bits; return x; }

    // Explicit Late Materialization factory (pack a full 64-bit string reference)
    static value_t make_str_ref(uint8_t t, uint8_t c, uint32_t p, uint16_t s) {
        value_t x; x.raw = PackedStringRef(t,c,p,s).raw; return x;
    }

    static value_t make_str(PackedStringRef sr) { value_t x; x.raw = sr.raw; return x; }

    static value_t make_null() { value_t x; x.raw = UINT64_MAX; return x; }

    bool is_null() const { return raw == UINT64_MAX; }

    int32_t as_i32() const { return static_cast<int32_t>(static_cast<int64_t>(raw)); }
    int64_t as_i64() const { return static_cast<int64_t>(raw); }
    double  as_f64() const { double v; std::memcpy(&v, &raw, sizeof(v)); return v; }
    uint64_t as_ref() const { return raw; }
};

// 5. StringRefResolver
struct StringRefResolver {
    const Plan* plan;
    StringRefResolver(const Plan* p) : plan(p) {}
    std::pair<const char*, size_t> resolve(uint64_t raw_ref, std::string& buffer);
};

} // namespace Contest
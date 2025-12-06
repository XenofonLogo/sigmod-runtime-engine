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
struct StringRefHash {
    const Plan* plan;
    StringRefHash(const Plan* p) : plan(p) {} 
    size_t operator()(const PackedStringRef& k) const { return std::hash<uint64_t>{}(k.raw); }
};

struct StringRefEq {
    const Plan* plan;
    StringRefEq(const Plan* p) : plan(p) {}
    bool operator()(const PackedStringRef& a, const PackedStringRef& b) const { return a.raw == b.raw; }
};

// 4. value_t (Unified)
struct value_t {
    enum class Type : uint8_t { I32, I64, FP64, STR, STR_REF, NULL_VAL } type;

    union {
        int32_t i32;
        int64_t i64;
        double  f64;
        uint64_t ref; // Stores PackedStringRef.raw
    } u;

    // Factories
    static value_t make_i32(int32_t v) { value_t x; x.type = Type::I32; x.u.i32 = v; return x; }
    static value_t make_i64(int64_t v) { value_t x; x.type = Type::I64; x.u.i64 = v; return x; }
    static value_t make_f64(double v) { value_t x; x.type = Type::FP64; x.u.f64 = v; return x; }
    
    // Explicit Late Materialization factory
    static value_t make_str_ref(uint8_t t, uint8_t c, uint32_t p, uint16_t s) {
        value_t x; x.type = Type::STR_REF; x.u.ref = PackedStringRef(t,c,p,s).raw; return x;
    }
    
    // Factory συμβατότητας για το columnar.cpp
    static value_t make_str(PackedStringRef sr) {
         value_t x; x.type = Type::STR_REF; x.u.ref = sr.raw; return x;
    }

    // Factory για NULL
    static value_t make_null() {
        value_t x; x.type = Type::NULL_VAL; x.u.i64 = 0; return x; 
    }
};

// 5. StringRefResolver
struct StringRefResolver {
    const Plan* plan;
    StringRefResolver(const Plan* p) : plan(p) {}
    std::pair<const char*, size_t> resolve(uint64_t raw_ref, std::string& buffer);
};

} // namespace Contest
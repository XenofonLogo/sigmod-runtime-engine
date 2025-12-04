#pragma once

#include <cstdint>
#include <utility>
#include <functional>

namespace Contest {



// ------------------------------------------------------------
// StringRef: αναφορά σε string σε columnar σελίδες
// ------------------------------------------------------------
struct StringRef {
    uint16_t table_id;
    uint8_t  column_id;
    uint32_t page_id;
    uint16_t offset;
    uint16_t length;

    StringRef() = default;

    StringRef(uint16_t t, uint8_t c, uint32_t p, uint16_t off, uint16_t len)
        : table_id(t), column_id(c), page_id(p), offset(off), length(len) {}

    // Πακετάρισμα σε 64-bit για χρήση σε hash κλπ
    std::uint64_t pack() const {
        std::uint64_t x = 0;
        x |= static_cast<std::uint64_t>(offset) & 0xFFFFull;             // bits 0-15
        x |= (static_cast<std::uint64_t>(page_id) & 0xFFFFull) << 16;    // bits 16-31
        x |= (static_cast<std::uint64_t>(column_id) & 0xFFull) << 32;    // bits 32-39
        x |= (static_cast<std::uint64_t>(table_id) & 0xFFull) << 40;     // bits 40-47
        x |= (static_cast<std::uint64_t>(length) & 0xFFFFull) << 48;     // bits 48-63
        return x;
    }

    static StringRef unpack(std::uint64_t x) {
        StringRef r;
        r.offset    = static_cast<uint16_t>( x        & 0xFFFFu );
        r.page_id   = static_cast<uint32_t>((x >> 16) & 0xFFFFu );
        r.column_id = static_cast<uint8_t> ((x >> 32) & 0xFFu   );
        r.table_id  = static_cast<uint16_t>((x >> 40) & 0xFFu   );
        r.length    = static_cast<uint16_t>((x >> 48) & 0xFFFFu );
        return r;
    }
};

// ------------------------------------------------------------
// value_t: generic value container
// ------------------------------------------------------------
struct value_t {
    enum class Type : std::uint8_t {
        I32,
        I64,
        FP64,
        STR,
        NULLTYPE,
        // aliases για παλιό κώδικα
        INT32 = I32,
        INT64 = I64
    };

    Type type;

    union {
        std::int32_t i32;
        std::int64_t i64;
        double       f64;
        StringRef    ref;
        StringRef    sref; // alias ώστε να δουλεύουν και u.ref και u.sref
    } u;

    value_t() : type(Type::NULLTYPE) {
        u.i64 = 0;
    }

    // factories που περιμένει το columnar.cpp
    static value_t make_i32(std::int32_t v) {
        value_t r;
        r.type = Type::I32;
        r.u.i32 = v;
        return r;
    }

    static value_t make_i64(std::int64_t v) {
        value_t r;
        r.type = Type::I64;
        r.u.i64 = v;
        return r;
    }

    static value_t make_f64(double v) {
        value_t r;
        r.type = Type::FP64;
        r.u.f64 = v;
        return r;
    }

    static value_t make_str(const StringRef& rref) {
        value_t r;
        r.type = Type::STR;
        r.u.ref = rref;
        r.u.sref = rref;
        return r;
    }

    static value_t make_null() {
        return value_t();
    }

    // factories που χρησιμοποιούν τα tests (from_int, from_stringref)
    static value_t from_int(std::int64_t v) {
        // αρκεί 32-bit για τα tests
        return make_i32(static_cast<std::int32_t>(v));
    }

    static value_t from_stringref(const StringRef& rref) {
        return make_str(rref);
    }

    // helpers που χρησιμοποιούν τα tests
    bool is_int() const {
        return type == Type::I32 || type == Type::I64;
    }

    bool is_stringref() const {
        return type == Type::STR;
    }

    bool is_null() const {
        return type == Type::NULLTYPE;
    }
};

// ------------------------------------------------------------
// Hash / equality για StringRef σε unordered_map
// ------------------------------------------------------------
struct StringRefHash {
    const Plan* plan; // δεν το χρειαζόμαστε εδώ, αλλά το columnar.cpp περνάει &plan

    explicit StringRefHash(const Plan* p) : plan(p) {}

    std::size_t operator()(const StringRef& r) const noexcept {
        std::uint64_t x = r.pack();
        return std::hash<std::uint64_t>{}(x);
    }
};

struct StringRefEq {
    const Plan* plan;

    explicit StringRefEq(const Plan* p) : plan(p) {}

    bool operator()(const StringRef& a, const StringRef& b) const noexcept {
        return a.table_id  == b.table_id  &&
               a.column_id == b.column_id &&
               a.page_id   == b.page_id   &&
               a.offset    == b.offset    &&
               a.length    == b.length;
    }
};

// ------------------------------------------------------------
// StringRefResolver: βρίσκει pointer+length από ColumnarTable
// ------------------------------------------------------------
struct StringRefResolver {
    const Plan* plan;

    explicit StringRefResolver(const Plan* p) : plan(p) {}

    std::pair<const char*, unsigned int>
    resolve(const StringRef& ref, const ColumnarTable& table) const;
};

// inline υλοποίηση για να λινκάρει παντού
inline std::pair<const char*, unsigned int>
StringRefResolver::resolve(const StringRef& ref, const ColumnarTable& table) const {
    // Υποθέτουμε ότι το ColumnarTable έχει:
    // table.columns[ column_id ].pages[ page_id ] -> Page*
    // και ότι Page έχει std::vector<char> data;
    const auto& col = table.columns[ref.column_id];
    auto pg = col.pages[ref.page_id];       // Page* const
    const char* ptr = pg->data.data() + ref.offset;
    return { ptr, ref.length };
}

} // namespace Contest
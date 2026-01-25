// columnar.cpp
#include "columnar.h"                  
#include "late_materialization.h"      
#include "parallel_unchained_hashtable.h" 
#include <cstring>                       
#include <algorithm>                     

namespace Contest {

// Επιστρέφει ColumnBuffer με το αποτέλεσμα του join
// Εκτελεί hash join πάνω σε δύο ColumnBuffer και επιστρέφει νέο ColumnBuffer
ColumnBuffer join_columnbuffer_hash(const Plan& plan,
    const JoinNode& join,
    const std::vector<std::tuple<size_t, DataType>>& output_attrs,
    const ColumnBuffer& left,
    const ColumnBuffer& right) {
    // Δημιουργία buffer εξόδου με το ζητούμενο schema
    ColumnBuffer out(output_attrs.size(), 0);
    out.types.reserve(output_attrs.size());
    for (auto& t : output_attrs) out.types.push_back(std::get<1>(t));

    // Βοηθός: προσθέτει ένα ζεύγος (left row, right row) στην έξοδο
    auto emit_pair = [&](size_t lidx, size_t ridx) {
        size_t left_cols = left.num_cols();                  // Πλήθος στηλών αριστερού
        for (size_t col_idx = 0; col_idx < output_attrs.size(); ++col_idx) {
            size_t src_idx = std::get<0>(output_attrs[col_idx]); // Από ποια στήλη προκύπτει
            if (src_idx < left_cols)                           // Από αριστερό
                out.columns[col_idx].append(left.columns[src_idx].get(lidx));
            else                                               // Από δεξί
                out.columns[col_idx].append(right.columns[src_idx - left_cols].get(ridx));
        }
        ++out.num_rows;                                       // Αύξηση πλήθους γραμμών
    };

    // Join για INT32: χρησιμοποιεί flat UnchainedHashTable (cache-friendly)
    auto join_numeric = [&](bool build_left_side) {
        using T = int32_t;

        const ColumnBuffer& build_buf = build_left_side ? left : right; // Πλευρά build
        size_t build_key_idx = build_left_side ? join.left_attr : join.right_attr; // Στήλη κλειδιού build

        // Συλλογή entries από τη build πλευρά (αγνοεί NULLs)
        std::vector<HashEntry<T>> entries;
        entries.reserve(build_buf.num_rows);
        for (std::size_t i = 0; i < build_buf.num_rows; ++i) {
            const auto& v = build_buf.columns[build_key_idx].get(i);
            if (v.is_null()) continue;
            entries.push_back(HashEntry<T>{v.as_i32(), static_cast<uint32_t>(i)});
        }

        UnchainedHashTable<T> ht;               // Κατασκευή hash table
        ht.build_from_entries(entries);

        const ColumnBuffer& probe_buf = build_left_side ? right : left; // Πλευρά probe
        size_t probe_key_idx = build_left_side ? join.right_attr : join.left_attr; // Στήλη probe

        // Probe όλων των γραμμών και εκπομπή ταιριασμάτων
        for (std::size_t j = 0; j < probe_buf.num_rows; ++j) {
            const auto& v = probe_buf.columns[probe_key_idx].get(j);
            if (v.is_null()) continue;

            T probe_key = v.as_i32();

            std::size_t len = 0;
            const auto* base = ht.probe(probe_key, len); // Εύρεση bucket
            if (!base) continue;

            for (std::size_t k = 0; k < len; ++k) {
                if (base[k].key == probe_key) { // Επιβεβαίωση ίδιου κλειδιού
                    if (build_left_side) emit_pair(static_cast<size_t>(base[k].row_id), j);
                    else emit_pair(j, static_cast<size_t>(base[k].row_id));
                }
            }
        }
    };

    // Join για VARCHAR: hash σε packed 64-bit references, αποφυγή string compares
    auto join_varchar = [&](bool build_left_side) {
        const ColumnBuffer& build_buf = build_left_side ? left : right;
        size_t build_key_idx = build_left_side ? join.left_attr : join.right_attr;

        std::vector<HashEntry<uint64_t>> entries; // Συλλογή references
        entries.reserve(build_buf.num_rows);

        for (std::size_t i = 0; i < build_buf.num_rows; ++i) {
            const auto& v = build_buf.columns[build_key_idx].get(i);
            if (v.is_null()) continue;
            entries.push_back(HashEntry<uint64_t>{v.as_ref(), static_cast<uint32_t>(i)});
        }

        UnchainedHashTable<uint64_t> ht; // Κατασκευή hash table για refs
        ht.build_from_entries(entries);

        const ColumnBuffer& probe_buf = build_left_side ? right : left; // Πλευρά probe
        size_t probe_key_idx = build_left_side ? join.right_attr : join.left_attr;

        for (std::size_t j = 0; j < probe_buf.num_rows; ++j) {
            const auto& v = probe_buf.columns[probe_key_idx].get(j);
            if (v.is_null()) continue;

            uint64_t key = v.as_ref();           // Packed ref
            std::size_t len = 0;
            const auto* base = ht.probe(key, len);
            if (!base) continue;

            for (std::size_t k = 0; k < len; ++k) {
                if (base[k].key == key) {
                    if (build_left_side) emit_pair(static_cast<size_t>(base[k].row_id), j);
                    else emit_pair(j, static_cast<size_t>(base[k].row_id));
                }
            }
        }
    };

    // Επιλογή τύπου κλειδιού (INT32 ή VARCHAR) με βάση την build πλευρά
    DataType key_type = join.build_left ? std::get<1>(plan.nodes[join.left].output_attrs[join.left_attr])
                                        : std::get<1>(plan.nodes[join.right].output_attrs[join.right_attr]);
    switch (key_type) {
        case DataType::INT32:   join_numeric(join.build_left); break;
        case DataType::VARCHAR: join_varchar(join.build_left); break;
        default: break;
    }

    return out;
}

} // namespace Contest
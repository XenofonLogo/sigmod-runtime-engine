
    #include "columnar.h"
    #include "late_materialization.h"
    #include "parallel_unchained_hashtable.h"
    #include <cstring>
    #include <algorithm>

namespace Contest {

// H get_bitmap_local_col αφαιρέθηκε από εδώ επειδή μετακινήθηκε στο columnar.h

// Οι scan_columnar_to_columnbuffer και finalize_columnbuffer_to_columnar
// αφαιρέθηκαν από εδώ επειδή μετακινήθηκαν στο late_materialization.cpp

// Υλοποίηση μόνο της join_columnbuffer_hash (αν χρειάζεται για tests ή legacy code)
// Ενημερωμένη για να συμβαδίζει με το νέο value_t
// Επιστρέφει ColumnBuffer με το αποτέλεσμα του join
ColumnBuffer join_columnbuffer_hash(const Plan& plan,
    const JoinNode& join,
    const std::vector<std::tuple<size_t, DataType>>& output_attrs,
    const ColumnBuffer& left,
    const ColumnBuffer& right) {
    // Δημιουργεί το output buffer με το σωστό schema
    ColumnBuffer out(output_attrs.size(), 0);
    out.types.reserve(output_attrs.size());
    for (auto& t : output_attrs) out.types.push_back(std::get<1>(t));

    // Συνάρτηση που προσθέτει ένα ζεύγος (tuple) στο output
    auto emit_pair = [&](size_t lidx, size_t ridx) {
        size_t left_cols = left.num_cols();
        for (size_t col_idx = 0; col_idx < output_attrs.size(); ++col_idx) {
            size_t src_idx = std::get<0>(output_attrs[col_idx]);
            if (src_idx < left_cols) out.columns[col_idx].append(left.columns[src_idx].get(lidx));
            else out.columns[col_idx].append(right.columns[src_idx - left_cols].get(ridx));
        }
        ++out.num_rows;
    };

    // Numeric join (INT32 only)
    // (GR) Αντικατάσταση std::unordered_map (πολλά μικρά allocations + pointer chasing)
    // με UnchainedHashTable (flat storage + prefix sums), που είναι πολύ πιο cache-friendly.
    auto join_numeric = [&](bool build_left_side) {
        using T = int32_t;

        // Επιλογή πλευράς για build (left ή right)
        const ColumnBuffer& build_buf = build_left_side ? left : right;
        size_t build_key_idx = build_left_side ? join.left_attr : join.right_attr;

        // Δημιουργεί τα entries για το hash table
        std::vector<HashEntry<T>> entries;
        entries.reserve(build_buf.num_rows);
        for (std::size_t i = 0; i < build_buf.num_rows; ++i) {
            const auto& v = build_buf.columns[build_key_idx].get(i);
            if (v.is_null()) continue;
            entries.push_back(HashEntry<T>{v.as_i32(), static_cast<uint32_t>(i)});
        }

        // Χτίζει το hash table
        UnchainedHashTable<T> ht;
        ht.build_from_entries(entries);

        // Επιλογή πλευράς για probe
        const ColumnBuffer& probe_buf = build_left_side ? right : left;
        size_t probe_key_idx = build_left_side ? join.right_attr : join.left_attr;

        // Για κάθε γραμμή του probe, ψάχνει στο hash table
        for (std::size_t j = 0; j < probe_buf.num_rows; ++j) {
            const auto& v = probe_buf.columns[probe_key_idx].get(j);
            if (v.is_null()) continue;

            T probe_key = v.as_i32();

            std::size_t len = 0;
            const auto* base = ht.probe(probe_key, len);
            if (!base) continue;

            // Για κάθε match, προσθέτει το ζεύγος στο output
            for (std::size_t k = 0; k < len; ++k) {
                if (base[k].key == probe_key) {
                    if (build_left_side) emit_pair(static_cast<size_t>(base[k].row_id), j);
                    else emit_pair(j, static_cast<size_t>(base[k].row_id));
                }
            }
        }
    };

    // Join για VARCHAR (packed string refs)
    auto join_varchar = [&](bool build_left_side) {
        // Hashάρουμε πάνω στο packed 64-bit reference (raw). Αυτό αποφεύγει string compares/copies.
        // Το πραγματικό string γίνεται resolve μόνο στο τελικό materialization.
        const ColumnBuffer& build_buf = build_left_side ? left : right;
        size_t build_key_idx = build_left_side ? join.left_attr : join.right_attr;

        std::vector<HashEntry<uint64_t>> entries;
        entries.reserve(build_buf.num_rows);

        for (std::size_t i = 0; i < build_buf.num_rows; ++i) {
            const auto& v = build_buf.columns[build_key_idx].get(i);
            if (v.is_null()) continue;
            entries.push_back(HashEntry<uint64_t>{v.as_ref(), static_cast<uint32_t>(i)});
        }

        UnchainedHashTable<uint64_t> ht;
        ht.build_from_entries(entries);

        // Probe side
        const ColumnBuffer& probe_buf = build_left_side ? right : left;
        size_t probe_key_idx = build_left_side ? join.right_attr : join.left_attr;

        for (std::size_t j = 0; j < probe_buf.num_rows; ++j) {
            const auto& v = probe_buf.columns[probe_key_idx].get(j);
            if (v.is_null()) continue;

            uint64_t key = v.as_ref();
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

    // Επιλογή τύπου κλειδιού (INT32 ή VARCHAR)
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
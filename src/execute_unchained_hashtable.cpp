#include <hardware.h>
#include <plan.h>
#include <table.h>

#include "unchained_hashtable.h"
#include "hash_functions.h"
#include "bloom_filter.h"

namespace Contest {

using ExecuteResult = std::vector<std::vector<Data>>;

ExecuteResult execute_impl(const Plan& plan, size_t node_idx);

// -----------------------------------------------------------------------------
// JoinAlgorithm (cleaned / INT32-only)
// -----------------------------------------------------------------------------
// Σημείωση: η εκφώνηση δηλώνει ρητά ότι οι ζεύξεις γίνονται πάνω σε στήλες INT32.
// Γι' αυτό η παρακάτω υλοποίηση υποστηρίζει μόνο INT32 join keys — απλοποιεί
// τον κώδικα και βελτιώνει την απόδοση (no generic templating).
// -----------------------------------------------------------------------------
struct JoinAlgorithm {
    bool                                             build_left;
    ExecuteResult&                                   left;      // build-side = left αν build_left==true
    ExecuteResult&                                   right;     // probe-side = right αν build_left==true
    ExecuteResult&                                   results;
    size_t                                           left_col, right_col;
    const std::vector<std::tuple<size_t, DataType>>& output_attrs;

    // Helper: εξάγει int32 από Data variant. Επιστρέφει true αν υπάρχει έγκυρη τιμή.
    static bool extract_int32(const Data& d, int32_t& out) {
        bool ok = false;
        std::visit([&](const auto& v) {
            using Vt = std::decay_t<decltype(v)>;
            if constexpr (std::is_same_v<Vt, int32_t>) {
                out = v;
                ok = true;
            } else {
                // std::monostate (NULL) ή άλλος τύπος -> not ok
                ok = false;
            }
        }, d);
        return ok;
    }

    // Μονοδιάστατο run για INT32 joins
    void run_int32() {
        using Key = int32_t;
        // Εντοπίζω ποια πλευρά είναι build/probe σε γενικευμένη μορφή
        ExecuteResult* build_side = build_left ? &left : &right;
        ExecuteResult* probe_side = build_left ? &right : &left;
        size_t build_key_col = build_left ? left_col : right_col;
        size_t probe_key_col = build_left ? right_col : left_col;

        // 1) Συλλογή entries (key,rowid) από build_side
        std::vector<std::pair<Key, std::size_t>> entries;
        entries.reserve(build_side->size());
        for (std::size_t i = 0; i < build_side->size(); ++i) {
            const auto& record = (*build_side)[i];
            Key key{};
            if (extract_int32(record[build_key_col], key)) {
                entries.emplace_back(key, i);
            } else {
                // skip NULLs / non-int32 (per spec joins are INT32-only)
            }
        }

        // Αν δεν έχουμε entries, τίποτα να κάνουμε
        if (entries.empty()) return;

        // 2) Build unchained hashtable
        UnchainedHashTable<Key> table;
        table.reserve(entries.size());           // προ-κρατάμε χώρο (βασική βελτιστοποίηση)
        table.build_from_entries(entries);

        // 3) Probe: για κάθε tuple στην probe_side ψάχνουμε matches
        for (const auto& probe_record : *probe_side) {
            Key probe_key{};
            if (!extract_int32(probe_record[probe_key_col], probe_key)) continue; // skip NULLs / wrong type

            std::size_t len = 0;
            const auto* base = table.probe(probe_key, len);
            if (!base || len == 0) continue;

            // Σαρώνουμε το candidate range και κάνουμε exact compare
            for (std::size_t bi = 0; bi < len; ++bi) {
                if (base[bi].key != probe_key) continue;
                // Αν ταιριάζει, φτιάχνουμε το joined record.
                // build_row_id δείχνει σε index του build_side vector
                const auto& build_record = (*build_side)[base[bi].row_id];
                const auto& probe_rec    = probe_record;

                // Επειδή το output_attrs είναι σε όλο το αποτέλεσμα (left + right),
                // πρέπει να ξέρουμε ποιο είναι το left και ποιο το right στο τελικό output.
                // Αν build_left==true τότε left==build_side, right==probe_side,
                // αλλιώς το αντίστροφο.
                const auto* left_rec_ptr  = build_left ? &build_record : &probe_rec;
                const auto* right_rec_ptr = build_left ? &probe_rec    : &build_record;

                // Δημιουργία result row σύμφωνα με output_attrs
                std::vector<Data> new_record;
                new_record.reserve(output_attrs.size());
                for (auto [col_idx, _] : output_attrs) {
                    // Αν το col_idx αναφέρεται στην πλευρά του left (πριν το μέγεθος του left record)
                    // τότε παίρνουμε από left_rec_ptr, αλλιώς από right_rec_ptr.
                    if (col_idx < left_rec_ptr->size()) {
                        new_record.emplace_back((*left_rec_ptr)[col_idx]);
                    } else {
                        new_record.emplace_back((*right_rec_ptr)[col_idx - left_rec_ptr->size()]);
                    }
                }
                results.emplace_back(std::move(new_record));
            }
        }
    } // run_int32()
}; // struct JoinAlgorithm

// -----------------------------------------------------------------------------
// execute_hash_join / scan / impl / execute
// -----------------------------------------------------------------------------
ExecuteResult execute_hash_join(const Plan&          plan,
    const JoinNode&                                  join,
    const std::vector<std::tuple<size_t, DataType>>& output_attrs) {

    auto left_idx  = join.left;
    auto right_idx = join.right;
    auto& left_node  = plan.nodes[left_idx];
    auto& right_node = plan.nodes[right_idx];
    auto& left_types = left_node.output_attrs;
    auto& right_types= right_node.output_attrs;

    // Εκτέλεση υποδέντρων (scan/join) για να πάρουμε materialized rows
    auto left  = execute_impl(plan, left_idx);
    auto right = execute_impl(plan, right_idx);

    std::vector<std::vector<Data>> results;

    JoinAlgorithm join_algorithm{
        .build_left   = join.build_left,
        .left         = left,
        .right        = right,
        .results      = results,
        .left_col     = join.left_attr,
        .right_col    = join.right_attr,
        .output_attrs = output_attrs
    };

    // Ελέγχουμε ότι ο τύπος join-στήλης είναι INT32 — σύμφωνο με την εκφώνηση.
    if (join.build_left) {
        if (std::get<1>(left_types[join.left_attr]) != DataType::INT32) {
            throw std::runtime_error("Only INT32 join columns are supported in this executor");
        }
    } else {
        if (std::get<1>(right_types[join.right_attr]) != DataType::INT32) {
            throw std::runtime_error("Only INT32 join columns are supported in this executor");
        }
    }

    // Εδώ καλούμε την απλοποιημένη run_int32() — δεν χρειάζεται template dispatch.
    join_algorithm.run_int32();

    return results;
}

ExecuteResult execute_scan(const Plan&               plan,
    const ScanNode&                                  scan,
    const std::vector<std::tuple<size_t, DataType>>& output_attrs) {
    auto table_id = scan.base_table_id;
    auto& input   = plan.inputs[table_id];
    return Table::copy_scan(input, output_attrs);
}

ExecuteResult execute_impl(const Plan& plan, size_t node_idx) {
    auto& node = plan.nodes[node_idx];
    return std::visit(
        [&](const auto& value) -> ExecuteResult {
            using T = std::decay_t<decltype(value)>;
            if constexpr (std::is_same_v<T, JoinNode>) {
                return execute_hash_join(plan, value, node.output_attrs);
            } else {
                return execute_scan(plan, value, node.output_attrs);
            }
        },
        node.data);
}

ColumnarTable execute(const Plan& plan, [[maybe_unused]] void* context) {
    namespace views = ranges::views;
    auto ret       = execute_impl(plan, plan.root);
    auto ret_types = plan.nodes[plan.root].output_attrs
                   | views::transform([](const auto& v) { return std::get<1>(v); })
                   | ranges::to<std::vector<DataType>>();
    Table table{std::move(ret), std::move(ret_types)};
    return table.to_columnar();
}

void* build_context() {
    return nullptr;
}

void destroy_context([[maybe_unused]] void* context) {}

} // namespace Contest

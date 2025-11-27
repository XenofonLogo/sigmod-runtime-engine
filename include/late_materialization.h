#pragma once

#include <vector>
#include <cstdint>
#include <tuple>
#include <plan.h>
#include <table.h>

namespace Contest {

// Placeholder: Late materialization structures and type definitions.
// (e.g., StringRef, value_t, ExecuteResult, StringRefResolver)

struct StringRef;
struct value_t;
using ExecuteResult = std::vector<std::vector<value_t>>;

// Function declarations
ExecuteResult scan_columnar_to_values(const Plan& plan,
    const ScanNode& scan,
    const std::vector<std::tuple<size_t, DataType>>& output_attrs);

ColumnarTable finalize_values_to_columnar(const Plan& plan,
    const ExecuteResult& values,
    const std::vector<std::tuple<size_t, DataType>>& output_attrs);

ExecuteResult join_values_hash(const Plan& plan,
    const JoinNode& join,
    const std::vector<std::tuple<size_t, DataType>>& output_attrs,
    const ExecuteResult& left,
    const ExecuteResult& right);

} // namespace Contest
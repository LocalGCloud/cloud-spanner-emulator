//
// Copyright 2020 Google LLC
//
// Licensed under the Apache License, Version 2.0 (the "License");
// you may not use this file except in compliance with the License.
// You may obtain a copy of the License at
//
//      http://www.apache.org/licenses/LICENSE-2.0
//
// Unless required by applicable law or agreed to in writing, software
// distributed under the License is distributed on an "AS IS" BASIS,
// WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
// See the License for the specific language governing permissions and
// limitations under the License.
//

#ifndef THIRD_PARTY_CLOUD_SPANNER_EMULATOR_BACKEND_QUERY_PLAN_QUERY_PLAN_BUILDER_H_
#define THIRD_PARTY_CLOUD_SPANNER_EMULATOR_BACKEND_QUERY_PLAN_QUERY_PLAN_BUILDER_H_

#include <cstdint>

#include "google/spanner/v1/query_plan.pb.h"
#include "googlesql/resolved_ast/resolved_ast.h"
#include "absl/container/flat_hash_map.h"
#include "absl/time/time.h"

namespace google {
namespace spanner {
namespace emulator {
namespace backend {

// Measurements of one table scan of a profiled statement.
struct ScanProfile {
  // Rows the scan produced, which are also the rows it scanned.
  int64_t rows = 0;
  // Times the scan was started, e.g. once per row on the map side of a join.
  int64_t executions = 0;
  // Time spent reading rows.
  absl::Duration latency;
};

// Measurements of a statement executed in PROFILE mode.
struct StatementProfile {
  // Rows returned by a query or modified by DML.
  int64_t rows = 0;
  absl::Duration latency;
  absl::Duration cpu_time;
  // Measurements of the table scans of the statement, keyed by scan node.
  absl::flat_hash_map<const googlesql::ResolvedTableScan*, ScanProfile> scans;
};

// Builds the query plan of `statement` from its resolved AST. Nodes are
// listed in pre-order, and their names and metadata follow Cloud Spanner's
// operators (Serialize Result, Distributed Union, Scan, Filter Scan, Cross
// Apply, Hash Join, Aggregate, Sort, Sort Limit, Limit, Compute, Union All,
// Unit Relation, Array Unnest, Apply Mutations, ...), with SCALAR child nodes
// that describe expressions. The emulator executes statements as written, so
// the plan mirrors the statement rather than an optimized execution.
//
// If `profile` is set, the root node and the table scans carry its
// measurements as execution statistics. Other nodes have none, because the
// emulator does not measure them.
v1::QueryPlan BuildQueryPlan(const googlesql::ResolvedStatement& statement,
                             const StatementProfile* profile);

}  // namespace backend
}  // namespace emulator
}  // namespace spanner
}  // namespace google

#endif  // THIRD_PARTY_CLOUD_SPANNER_EMULATOR_BACKEND_QUERY_PLAN_QUERY_PLAN_BUILDER_H_

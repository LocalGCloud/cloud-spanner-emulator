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

#ifndef THIRD_PARTY_CLOUD_SPANNER_EMULATOR_BACKEND_QUERY_PLAN_SCAN_PROFILER_H_
#define THIRD_PARTY_CLOUD_SPANNER_EMULATOR_BACKEND_QUERY_PLAN_SCAN_PROFILER_H_

#include <memory>

#include "googlesql/public/catalog.h"
#include "googlesql/resolved_ast/resolved_ast.h"
#include "absl/container/flat_hash_map.h"
#include "absl/status/statusor.h"
#include "backend/query/plan/query_plan_builder.h"

namespace google {
namespace spanner {
namespace emulator {
namespace backend {

// Measures the table scans of a statement for PROFILE mode. The GoogleSQL
// reference implementation has no per-operator statistics, so the profiler
// evaluates a copy of the statement whose table scans read through measuring
// proxies of their tables.
class ScanProfiler {
 public:
  ScanProfiler();
  ~ScanProfiler();

  // Returns a copy of `statement` to evaluate instead of it. The profiler must
  // outlive the evaluation of the copy.
  absl::StatusOr<std::unique_ptr<const googlesql::ResolvedStatement>>
  Instrument(const googlesql::ResolvedStatement& statement);

  // Returns the measurements of the table scans of the instrumented
  // statement, keyed by the scan nodes of the original statement.
  absl::flat_hash_map<const googlesql::ResolvedTableScan*, ScanProfile>
  ScanProfiles() const;

  class ProfilingTable;

 private:
  absl::flat_hash_map<const googlesql::ResolvedTableScan*,
                      std::unique_ptr<ProfilingTable>>
      tables_;
};

}  // namespace backend
}  // namespace emulator
}  // namespace spanner
}  // namespace google

#endif  // THIRD_PARTY_CLOUD_SPANNER_EMULATOR_BACKEND_QUERY_PLAN_SCAN_PROFILER_H_

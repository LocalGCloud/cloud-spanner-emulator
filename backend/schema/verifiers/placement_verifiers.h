//
// Copyright 2026 Google LLC
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

#ifndef THIRD_PARTY_CLOUD_SPANNER_EMULATOR_BACKEND_SCHEMA_VERIFIERS_PLACEMENT_VERIFIERS_H_
#define THIRD_PARTY_CLOUD_SPANNER_EMULATOR_BACKEND_SCHEMA_VERIFIERS_PLACEMENT_VERIFIERS_H_

#include "absl/status/status.h"
#include "absl/strings/string_view.h"
#include "absl/time/time.h"
#include "backend/schema/catalog/column.h"
#include "backend/schema/catalog/schema.h"
#include "backend/schema/catalog/table.h"
#include "backend/storage/storage.h"

namespace google {
namespace spanner {
namespace emulator {
namespace backend {

// Returns the placement key column of `table`, or nullptr if `table` is not a
// placement table.
const Column* GetPlacementKeyColumn(const Table* table);

// Verifies that no row of any placement table in `schema` has `placement_name`
// as its placement key value at `timestamp`.
//
// Unlike schema change actions, these checks read storage while the statement
// is being applied: a dropped table's data is marked as deleted before the
// statement's deferred actions run.
absl::Status VerifyPlacementNotInUse(const Storage* storage,
                                     absl::Time timestamp,
                                     const Schema* schema,
                                     absl::string_view placement_name);

// Verifies that placement table `table` has no rows at `timestamp`.
absl::Status VerifyPlacementTableIsEmpty(const Storage* storage,
                                         absl::Time timestamp,
                                         const Table* table);

}  // namespace backend
}  // namespace emulator
}  // namespace spanner
}  // namespace google

#endif  // THIRD_PARTY_CLOUD_SPANNER_EMULATOR_BACKEND_SCHEMA_VERIFIERS_PLACEMENT_VERIFIERS_H_

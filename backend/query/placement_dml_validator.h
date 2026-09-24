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

#ifndef THIRD_PARTY_CLOUD_SPANNER_EMULATOR_BACKEND_QUERY_PLACEMENT_DML_VALIDATOR_H_
#define THIRD_PARTY_CLOUD_SPANNER_EMULATOR_BACKEND_QUERY_PLACEMENT_DML_VALIDATOR_H_

#include <optional>
#include <string>

#include "googlesql/resolved_ast/resolved_ast.h"
#include "absl/status/status.h"
#include "backend/schema/catalog/schema.h"
#include "backend/schema/catalog/table.h"

namespace google {
namespace spanner {
namespace emulator {
namespace backend {

// Returns true if `table` has a placement key column.
bool IsPlacementTable(const Table* table);

// Returns true if `schema` has at least one table with a placement key column.
bool HasPlacementTables(const Schema* schema);

// Returns the name of the placement table that `statement` inserts into or
// deletes from, or nullopt if `statement` is not a top-level INSERT or DELETE
// on a placement table.
std::optional<std::string> PlacementInsertOrDeleteTable(
    const googlesql::ResolvedStatement* statement);

// Enforces the geo-partitioning (placement) limits that production Cloud
// Spanner applies to statements in read-write transactions:
//  - WHERE clauses may reference only the primary key columns of placement
//    tables. JOIN conditions are not checked.
//  - An INSERT or DELETE on a placement table must be the only statement in
//    its transaction. The validator reports such statements through
//    insert_or_delete_table(); the caller tracks the transaction.
class PlacementDmlValidator {
 public:
  PlacementDmlValidator() = default;

  // Returns an error if a WHERE clause in `statement` references a column of a
  // placement table that is not part of that table's primary key.
  absl::Status Validate(const googlesql::ResolvedStatement* statement);

  // After a successful Validate(), the name of the placement table that the
  // statement inserts into or deletes from, if any.
  const std::optional<std::string>& insert_or_delete_table() const {
    return insert_or_delete_table_;
  }

 private:
  std::optional<std::string> insert_or_delete_table_;
};

}  // namespace backend
}  // namespace emulator
}  // namespace spanner
}  // namespace google

#endif  // THIRD_PARTY_CLOUD_SPANNER_EMULATOR_BACKEND_QUERY_PLACEMENT_DML_VALIDATOR_H_

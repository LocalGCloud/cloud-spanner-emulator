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

#ifndef THIRD_PARTY_CLOUD_SPANNER_EMULATOR_BACKEND_QUERY_PRIVILEGE_VALIDATOR_H_
#define THIRD_PARTY_CLOUD_SPANNER_EMULATOR_BACKEND_QUERY_PRIVILEGE_VALIDATOR_H_

#include "googlesql/resolved_ast/resolved_ast.h"
#include "googlesql/resolved_ast/resolved_ast_visitor.h"
#include "absl/container/flat_hash_set.h"
#include "absl/status/status.h"
#include "backend/schema/catalog/access_policy.h"
#include "backend/schema/catalog/schema.h"

namespace google {
namespace spanner {
namespace emulator {
namespace backend {

// Checks that a database role holds the fine-grained access control privileges
// that a query or DML statement needs:
//
// - SELECT on each table column that the statement reads. A table scan that
//   reads no columns, such as for COUNT(*), needs SELECT on at least one.
// - SELECT on each view, and the privileges that the query that defines a
//   SQL SECURITY INVOKER view needs; a DEFINER view's query is not checked.
// - INSERT on the columns that an INSERT lists, UPDATE on the columns that an
//   UPDATE sets, and DELETE on the table of a DELETE.
// - EXECUTE on each model and SELECT or UPDATE on each sequence it uses,
//   including the sequences of the column default values that a DML statement
//   computes.
// - USAGE on the schema of each object.
// - Membership in spanner_sys_reader for SPANNER_SYS tables.
//
// INFORMATION_SCHEMA and pg_catalog tables can always be queried.
class PrivilegeValidator : public googlesql::ResolvedASTVisitor {
 public:
  PrivilegeValidator(const AccessPolicy* access, const Schema* schema)
      : access_(access), schema_(schema) {}

  absl::Status Validate(const googlesql::ResolvedStatement* statement) {
    return statement->Accept(this);
  }

 private:
  absl::Status VisitResolvedTableScan(
      const googlesql::ResolvedTableScan* node) override;
  absl::Status VisitResolvedInsertStmt(
      const googlesql::ResolvedInsertStmt* node) override;
  absl::Status VisitResolvedUpdateStmt(
      const googlesql::ResolvedUpdateStmt* node) override;
  absl::Status VisitResolvedDeleteStmt(
      const googlesql::ResolvedDeleteStmt* node) override;
  absl::Status VisitResolvedModel(
      const googlesql::ResolvedModel* node) override;
  absl::Status VisitResolvedFunctionCall(
      const googlesql::ResolvedFunctionCall* node) override;

  // Checks the privileges on the columns of a DML statement's target table:
  // columns that are read need SELECT and columns that are written need
  // `write_privilege`.
  absl::Status CheckDmlTarget(
      const googlesql::ResolvedTableScan* scan,
      absl::Span<const googlesql::ResolvedStatement::ObjectAccess>
          column_access_list,
      ddl::Privilege::Type write_privilege);

  // Checks the sequences of the default values that an INSERT computes.
  absl::Status CheckInsertDefaultValues(
      const googlesql::ResolvedInsertStmt* node);

  const AccessPolicy* access_;
  const Schema* schema_;

  // DML target table scans, whose columns are checked by CheckDmlTarget.
  absl::flat_hash_set<const googlesql::ResolvedTableScan*> dml_target_scans_;
};

}  // namespace backend
}  // namespace emulator
}  // namespace spanner
}  // namespace google

#endif  // THIRD_PARTY_CLOUD_SPANNER_EMULATOR_BACKEND_QUERY_PRIVILEGE_VALIDATOR_H_

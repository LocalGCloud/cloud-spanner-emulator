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

#ifndef THIRD_PARTY_CLOUD_SPANNER_EMULATOR_BACKEND_SCHEMA_CATALOG_ACCESS_POLICY_H_
#define THIRD_PARTY_CLOUD_SPANNER_EMULATOR_BACKEND_SCHEMA_CATALOG_ACCESS_POLICY_H_

#include <string>
#include <tuple>
#include <vector>

#include "absl/container/flat_hash_set.h"
#include "absl/status/status.h"
#include "absl/status/statusor.h"
#include "absl/strings/string_view.h"
#include "absl/types/span.h"
#include "backend/schema/catalog/column.h"
#include "backend/schema/catalog/index.h"
#include "backend/schema/catalog/schema.h"
#include "backend/schema/catalog/sequence.h"
#include "backend/schema/catalog/table.h"
#include "backend/schema/catalog/view.h"
#include "backend/schema/ddl/operations.pb.h"
#include "backend/schema/graph/schema_node.h"

namespace google {
namespace spanner {
namespace emulator {
namespace backend {

// The fine-grained access control privileges of the database role that a
// session uses. A role holds the privileges granted to it, to the roles it is a
// member of (directly or indirectly) and to the `public` role. Together these
// are the role's effective roles.
//
// Operations of sessions without a database role are not restricted, so they
// have no AccessPolicy.
class AccessPolicy {
 public:
  // Returns the policy of `role` in `schema`, or PERMISSION_DENIED if the role
  // does not exist.
  static absl::StatusOr<AccessPolicy> Create(const Schema* schema,
                                             absl::string_view role);

  const std::string& role() const { return role_; }

  // Returns true if `role` is one of the effective roles.
  bool IsEffectiveRole(absl::string_view role) const;

  // Members of spanner_info_reader see all INFORMATION_SCHEMA rows.
  bool unfiltered_information_schema() const;

  // Members of spanner_sys_reader may query SPANNER_SYS tables.
  bool can_read_spanner_sys() const;

  // Returns true if the role holds `type` on `object`, or on `column` of the
  // table `object` when `column` is set.
  bool Has(ddl::Privilege::Type type, const SchemaNode* object,
           const Column* column = nullptr) const;

  // Returns true if the role holds `type` on `table` or on any of its columns.
  bool HasOnAnyColumn(ddl::Privilege::Type type, const Table* table) const;

  // Returns true if the role holds any privilege on `object` or, for a table,
  // on any of its columns.
  bool HasAny(const SchemaNode* object) const;

  // Returns true if the role may use the objects in the schema of the object
  // named `object_name`: the USAGE privilege on its named schema, or on the
  // default schema, which `public` holds unless it was revoked.
  bool HasSchemaUsage(absl::string_view object_name) const;

  // Returns an error unless HasSchemaUsage(object_name).
  absl::Status CheckSchemaUsage(absl::string_view object_name) const;

  // Whether the role may see an object in the system catalogs
  // (INFORMATION_SCHEMA and pg_catalog):
  // - a table if it holds any privilege on the table or its columns;
  // - a column if it holds SELECT, INSERT or UPDATE on the column or its
  //   table, and an index column if it may see the indexed column;
  // - an index (nullptr for the primary key of `table`) if it may see all of
  //   its columns, or, with `table_delete_suffices`, holds DELETE on `table`;
  // - a view if it holds SELECT on it;
  // - a sequence if it holds SELECT or UPDATE on it;
  // - a user-defined function, or the change stream of a read function, if it
  //   holds EXECUTE on it.
  bool CanSeeTable(const Table* table) const;
  bool CanSeeColumn(const Column* column) const;
  bool CanSeeIndex(const Table* table, const Index* index,
                   bool table_delete_suffices) const;
  bool CanSeeView(const View* view) const;
  bool CanSeeSequence(const Sequence* sequence) const;
  bool CanSeeRoutine(const SchemaNode* routine) const;

  // Returns an error unless the role may call the sequences that the default
  // values of `columns` use: GET_NEXT_SEQUENCE_VALUE (nextval) needs SELECT or
  // UPDATE on the sequence. The internal sequences of identity columns need
  // no privilege.
  absl::Status CheckDefaultValueSequences(
      absl::Span<const Column* const> columns) const;

  // Returns the error for a missing privilege on an object of `object_kind`,
  // such as "table" or "change stream".
  absl::Status PrivilegeError(absl::string_view object_kind,
                              absl::string_view object_name) const;

 private:
  AccessPolicy() = default;

  const Schema* schema_ = nullptr;
  std::string role_;

  // The role, the roles it is a member of and `public`.
  std::vector<std::string> effective_roles_;

  // The privileges held through the effective roles, as (type, object, column)
  // with a null column for object-level privileges.
  absl::flat_hash_set<std::tuple<int, const SchemaNode*, const Column*>>
      privileges_;

  // The objects that the effective roles hold any privilege on.
  absl::flat_hash_set<const SchemaNode*> objects_with_privileges_;

  bool default_schema_usage_ = false;
};

}  // namespace backend
}  // namespace emulator
}  // namespace spanner
}  // namespace google

#endif  // THIRD_PARTY_CLOUD_SPANNER_EMULATOR_BACKEND_SCHEMA_CATALOG_ACCESS_POLICY_H_

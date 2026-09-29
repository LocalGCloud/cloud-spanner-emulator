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

#ifndef THIRD_PARTY_CLOUD_SPANNER_EMULATOR_BACKEND_SCHEMA_UPDATER_GRANT_RESOLVER_H_
#define THIRD_PARTY_CLOUD_SPANNER_EMULATOR_BACKEND_SCHEMA_UPDATER_GRANT_RESOLVER_H_

#include <string>
#include <vector>

#include "google/spanner/admin/database/v1/common.pb.h"
#include "absl/status/statusor.h"
#include "absl/strings/string_view.h"
#include "absl/types/span.h"
#include "backend/schema/catalog/grants.h"
#include "backend/schema/catalog/schema.h"
#include "backend/schema/ddl/operations.pb.h"
#include "google/protobuf/repeated_ptr_field.h"

namespace google {
namespace spanner {
namespace emulator {
namespace backend {

// Resolves the roles and objects named by GRANT and REVOKE statements against
// a schema, and checks them against the fine-grained access control rules.
class GrantResolver {
 public:
  GrantResolver(const Schema* schema,
                ::google::spanner::admin::database::v1::DatabaseDialect dialect)
      : schema_(schema), dialect_(dialect) {}

  // Returns one privilege per grantee, privilege type, object and column named
  // by a GRANT or REVOKE of `privileges` on `target`.
  absl::StatusOr<std::vector<Grants::Privilege>> ResolvePrivileges(
      const google::protobuf::RepeatedPtrField<ddl::Privilege>& privileges,
      const ddl::PrivilegeTarget& target,
      const google::protobuf::RepeatedPtrField<ddl::Grantee>& grantees) const;

  // Returns the memberships of `members` in `roles` named by a GRANT ROLE or
  // REVOKE ROLE statement. A GRANT may not create a membership cycle.
  absl::StatusOr<std::vector<Grants::Membership>> ResolveMemberships(
      const google::protobuf::RepeatedPtrField<ddl::Grantee>& roles,
      const google::protobuf::RepeatedPtrField<ddl::Grantee>& members,
      bool is_grant) const;

 private:
  // A schema object that privileges are granted on.
  struct Object {
    ddl::PrivilegeTarget::Type type;
    const SchemaNode* node;
    std::string name;
  };

  // Returns the name of a role as it was created, or the lower-case name of a
  // system role.
  absl::StatusOr<std::string> ResolveRole(absl::string_view name) const;

  absl::StatusOr<std::vector<Object>> ResolveObjects(
      const ddl::PrivilegeTarget& target) const;
  absl::StatusOr<Object> ResolveObject(ddl::PrivilegeTarget::Type type,
                                       const std::string& name) const;
  absl::StatusOr<std::vector<Object>> ResolveObjectsInSchema(
      ddl::PrivilegeTarget::Type type, const std::string& schema_name) const;

  // Returns the roles that `role` is a member of, directly or indirectly.
  static std::vector<std::string> AncestorRoles(
      absl::string_view role, absl::Span<const Grants::Membership> memberships);

  const Schema* schema_;
  const ::google::spanner::admin::database::v1::DatabaseDialect dialect_;
};

}  // namespace backend
}  // namespace emulator
}  // namespace spanner
}  // namespace google

#endif  // THIRD_PARTY_CLOUD_SPANNER_EMULATOR_BACKEND_SCHEMA_UPDATER_GRANT_RESOLVER_H_

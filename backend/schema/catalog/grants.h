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

#ifndef THIRD_PARTY_CLOUD_SPANNER_EMULATOR_BACKEND_SCHEMA_CATALOG_GRANTS_H_
#define THIRD_PARTY_CLOUD_SPANNER_EMULATOR_BACKEND_SCHEMA_CATALOG_GRANTS_H_

#include <memory>
#include <string>
#include <vector>

#include "absl/status/status.h"
#include "absl/strings/string_view.h"
#include "absl/types/span.h"
#include "backend/schema/ddl/operations.pb.h"
#include "backend/schema/graph/schema_node.h"

namespace google {
namespace spanner {
namespace emulator {
namespace backend {

class Column;
class Role;

// Names of the fine-grained access control system roles. They exist in every
// database without being created, so they are not schema nodes.
inline constexpr char kPublicRole[] = "public";
inline constexpr char kSpannerInfoReaderRole[] = "spanner_info_reader";
inline constexpr char kSpannerSysReaderRole[] = "spanner_sys_reader";

// Returns true if `role` names a system role. Role names are case-insensitive.
bool IsSystemRole(absl::string_view role);

// Grants holds the privileges and role memberships granted with GRANT
// statements. A schema has at most one Grants node.
//
// Privileges point at the objects they are granted on, so renaming an object
// keeps its privileges and dropping an object (or a column) revokes them.
class Grants final : public SchemaNode {
 public:
  // A privilege granted to a role on a schema object.
  struct Privilege {
    ddl::Privilege::Type type;

    // The kind of object: TABLE, VIEW, CHANGE_STREAM, TABLE_FUNCTION (the read
    // function of the change stream in `object`), SEQUENCE, MODEL or SCHEMA.
    ddl::PrivilegeTarget::Type object_type;

    // The object. Null for the USAGE privilege on the default schema.
    const SchemaNode* object = nullptr;

    // The column of a column-level privilege on a table, otherwise null.
    const Column* column = nullptr;

    // The role that holds the privilege, which may be the `public` role.
    std::string grantee;

    // The schema node of the grantee, null for `public`. Dropping the role
    // revokes the privilege.
    const Role* grantee_role = nullptr;
  };

  // Membership of `member` in `role`, from `GRANT ROLE role TO ROLE member`.
  // `member` inherits the privileges of `role`.
  struct Membership {
    std::string role;
    std::string member;

    // The schema nodes of the roles, null for system roles. Dropping either
    // role removes the membership.
    const Role* role_node = nullptr;
    const Role* member_node = nullptr;
  };

  Grants() = default;

  absl::Span<const Privilege> privileges() const { return privileges_; }

  absl::Span<const Membership> memberships() const { return memberships_; }

  // The `public` role holds USAGE on the default schema until it is revoked.
  bool public_default_schema_usage_revoked() const {
    return public_default_schema_usage_revoked_;
  }

  absl::Status Validate(SchemaValidationContext* context) const override;
  absl::Status ValidateUpdate(const SchemaNode* old,
                              SchemaValidationContext* context) const override;
  std::string DebugString() const override { return "Grants"; }

  class Editor;

 private:
  std::unique_ptr<SchemaNode> ShallowClone() const override {
    return std::make_unique<Grants>(*this);
  }
  absl::Status DeepClone(SchemaGraphEditor* editor,
                         const SchemaNode* orig) override;

  // Privileges in the order they were granted.
  std::vector<Privilege> privileges_;

  // Memberships in the order they were granted.
  std::vector<Membership> memberships_;

  bool public_default_schema_usage_revoked_ = false;
};

// Returns the name of the object that `privilege` is granted on: the read
// function name for TABLE_FUNCTION privileges and the empty name for the
// default schema.
std::string PrivilegeObjectName(const Grants::Privilege& privilege);

// Modifies a Grants node. Adding a privilege or membership that already
// exists and removing one that does not exist have no effect. Privileges are
// compared by the names of their objects, since an edit may mix nodes of the
// schema being edited and of its new version.
class Grants::Editor {
 public:
  explicit Editor(Grants* instance) : instance_(instance) {}

  void AddPrivilege(const Privilege& privilege);

  // Removing a privilege on a table also removes it from the table's columns.
  void RemovePrivilege(const Privilege& privilege);

  void AddMembership(const Membership& membership);
  void RemoveMembership(const Membership& membership);

  void set_public_default_schema_usage_revoked(bool revoked) {
    instance_->public_default_schema_usage_revoked_ = revoked;
  }

 private:
  Grants* instance_;
};

}  // namespace backend
}  // namespace emulator
}  // namespace spanner
}  // namespace google

#endif  // THIRD_PARTY_CLOUD_SPANNER_EMULATOR_BACKEND_SCHEMA_CATALOG_GRANTS_H_

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

#include "backend/schema/catalog/grants.h"

#include <algorithm>
#include <string>

#include "absl/status/status.h"
#include "absl/strings/match.h"
#include "absl/strings/string_view.h"
#include "backend/schema/catalog/change_stream.h"
#include "backend/schema/catalog/column.h"
#include "backend/schema/catalog/role.h"
#include "backend/schema/ddl/operations.pb.h"
#include "backend/schema/graph/schema_graph_editor.h"
#include "googlesql/base/status_macros.h"

namespace google {
namespace spanner {
namespace emulator {
namespace backend {

namespace {

bool SameColumn(const Column* a, const Column* b) {
  return a == nullptr ? b == nullptr : b != nullptr && a->Name() == b->Name();
}

bool SamePrivilege(const Grants::Privilege& a, const Grants::Privilege& b) {
  return a.type == b.type && a.object_type == b.object_type &&
         PrivilegeObjectName(a) == PrivilegeObjectName(b) &&
         SameColumn(a.column, b.column) &&
         absl::EqualsIgnoreCase(a.grantee, b.grantee);
}

bool SameMembership(const Grants::Membership& a, const Grants::Membership& b) {
  return absl::EqualsIgnoreCase(a.role, b.role) &&
         absl::EqualsIgnoreCase(a.member, b.member);
}

}  // namespace

bool IsSystemRole(absl::string_view role) {
  return absl::EqualsIgnoreCase(role, kPublicRole) ||
         absl::EqualsIgnoreCase(role, kSpannerInfoReaderRole) ||
         absl::EqualsIgnoreCase(role, kSpannerSysReaderRole);
}

std::string PrivilegeObjectName(const Grants::Privilege& privilege) {
  if (privilege.object == nullptr) {
    return "";
  }
  if (privilege.object_type == ddl::PrivilegeTarget::TABLE_FUNCTION) {
    return privilege.object->As<const ChangeStream>()->tvf_name();
  }
  return privilege.object->GetSchemaNameInfo()->name;
}

absl::Status Grants::Validate(SchemaValidationContext* context) const {
  return absl::OkStatus();
}

absl::Status Grants::ValidateUpdate(const SchemaNode* old,
                                    SchemaValidationContext* context) const {
  return absl::OkStatus();
}

// Replaces `node` with its clone. Returns true if it was deleted.
template <typename T>
absl::StatusOr<bool> CloneOrDeleted(SchemaGraphEditor* editor,
                                    const T*& node) {
  if (node == nullptr) {
    return false;
  }
  GOOGLESQL_ASSIGN_OR_RETURN(const SchemaNode* clone, editor->Clone(node));
  node = clone->As<const T>();
  return clone->is_deleted();
}

absl::Status Grants::DeepClone(SchemaGraphEditor* editor,
                               const SchemaNode* orig) {
  // Dropping an object, a column or a role revokes the privileges that refer
  // to it.
  for (auto it = privileges_.begin(); it != privileges_.end();) {
    GOOGLESQL_ASSIGN_OR_RETURN(bool object_deleted,
                               CloneOrDeleted(editor, it->object));
    GOOGLESQL_ASSIGN_OR_RETURN(bool column_deleted,
                               CloneOrDeleted(editor, it->column));
    GOOGLESQL_ASSIGN_OR_RETURN(bool grantee_deleted,
                               CloneOrDeleted(editor, it->grantee_role));
    it = object_deleted || column_deleted || grantee_deleted
             ? privileges_.erase(it)
             : it + 1;
  }
  for (auto it = memberships_.begin(); it != memberships_.end();) {
    GOOGLESQL_ASSIGN_OR_RETURN(bool role_deleted,
                               CloneOrDeleted(editor, it->role_node));
    GOOGLESQL_ASSIGN_OR_RETURN(bool member_deleted,
                               CloneOrDeleted(editor, it->member_node));
    it = role_deleted || member_deleted ? memberships_.erase(it) : it + 1;
  }
  return absl::OkStatus();
}

void Grants::Editor::AddPrivilege(const Privilege& privilege) {
  std::vector<Privilege>& privileges = instance_->privileges_;
  if (std::none_of(privileges.begin(), privileges.end(),
                   [&](const Privilege& existing) {
                     return SamePrivilege(existing, privilege);
                   })) {
    privileges.push_back(privilege);
  }
}

void Grants::Editor::RemovePrivilege(const Privilege& privilege) {
  std::vector<Privilege>& privileges = instance_->privileges_;
  privileges.erase(
      std::remove_if(privileges.begin(), privileges.end(),
                     [&](const Privilege& existing) {
                       if (privilege.column == nullptr &&
                           existing.column != nullptr) {
                         Privilege table_level = existing;
                         table_level.column = nullptr;
                         return SamePrivilege(table_level, privilege);
                       }
                       return SamePrivilege(existing, privilege);
                     }),
      privileges.end());
}

void Grants::Editor::AddMembership(const Membership& membership) {
  std::vector<Membership>& memberships = instance_->memberships_;
  if (std::none_of(memberships.begin(), memberships.end(),
                   [&](const Membership& existing) {
                     return SameMembership(existing, membership);
                   })) {
    memberships.push_back(membership);
  }
}

void Grants::Editor::RemoveMembership(const Membership& membership) {
  std::vector<Membership>& memberships = instance_->memberships_;
  memberships.erase(std::remove_if(memberships.begin(), memberships.end(),
                                   [&](const Membership& existing) {
                                     return SameMembership(existing,
                                                           membership);
                                   }),
                    memberships.end());
}

}  // namespace backend
}  // namespace emulator
}  // namespace spanner
}  // namespace google

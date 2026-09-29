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

#include "backend/schema/updater/grant_resolver.h"

#include <string>
#include <vector>

#include "absl/algorithm/container.h"
#include "absl/status/status.h"
#include "absl/status/statusor.h"
#include "absl/strings/ascii.h"
#include "absl/strings/match.h"
#include "absl/strings/string_view.h"
#include "absl/types/span.h"
#include "backend/schema/catalog/change_stream.h"
#include "backend/schema/catalog/column.h"
#include "backend/schema/catalog/model.h"
#include "backend/schema/catalog/named_schema.h"
#include "backend/schema/catalog/role.h"
#include "backend/schema/catalog/sequence.h"
#include "backend/schema/catalog/table.h"
#include "backend/schema/catalog/view.h"
#include "common/errors.h"
#include "googlesql/base/ret_check.h"
#include "googlesql/base/status_macros.h"

namespace google {
namespace spanner {
namespace emulator {
namespace backend {

namespace {

using ::google::spanner::admin::database::v1::DatabaseDialect;

std::string ObjectKind(ddl::PrivilegeTarget::Type type) {
  switch (type) {
    case ddl::PrivilegeTarget::TABLE:
      return "table";
    case ddl::PrivilegeTarget::VIEW:
      return "view";
    case ddl::PrivilegeTarget::CHANGE_STREAM:
      return "change stream";
    case ddl::PrivilegeTarget::SEQUENCE:
      return "sequence";
    case ddl::PrivilegeTarget::MODEL:
      return "model";
    case ddl::PrivilegeTarget::SCHEMA:
      return "schema";
    default:
      return "table function";
  }
}

// Returns true if privileges of type `privilege` can be granted on objects of
// type `object_type`.
bool IsValidPrivilege(ddl::Privilege::Type privilege,
                      ddl::PrivilegeTarget::Type object_type) {
  switch (object_type) {
    case ddl::PrivilegeTarget::TABLE:
      return privilege == ddl::Privilege::SELECT ||
             privilege == ddl::Privilege::INSERT ||
             privilege == ddl::Privilege::UPDATE ||
             privilege == ddl::Privilege::DELETE;
    case ddl::PrivilegeTarget::VIEW:
    case ddl::PrivilegeTarget::CHANGE_STREAM:
      return privilege == ddl::Privilege::SELECT;
    case ddl::PrivilegeTarget::SEQUENCE:
      return privilege == ddl::Privilege::SELECT ||
             privilege == ddl::Privilege::UPDATE;
    case ddl::PrivilegeTarget::TABLE_FUNCTION:
    case ddl::PrivilegeTarget::MODEL:
      return privilege == ddl::Privilege::EXECUTE;
    case ddl::PrivilegeTarget::SCHEMA:
      return privilege == ddl::Privilege::USAGE;
    default:
      return false;
  }
}

// Object names in GRANT and REVOKE statements must use the case the object
// was created with.
template <typename T>
const T* MatchingCase(const T* object, absl::string_view name) {
  return object != nullptr && object->Name() == name ? object : nullptr;
}

}  // namespace

absl::StatusOr<std::string> GrantResolver::ResolveRole(
    absl::string_view name) const {
  if (IsSystemRole(name)) {
    return absl::AsciiStrToLower(name);
  }
  const Role* role = schema_->FindRole(std::string(name));
  if (role == nullptr) {
    return error::DatabaseRoleNotFound(name);
  }
  return role->Name();
}

absl::StatusOr<std::vector<Grants::Privilege>> GrantResolver::ResolvePrivileges(
    const google::protobuf::RepeatedPtrField<ddl::Privilege>& privileges,
    const ddl::PrivilegeTarget& target,
    const google::protobuf::RepeatedPtrField<ddl::Grantee>& grantees) const {
  std::vector<std::string> grantee_names;
  for (const ddl::Grantee& grantee : grantees) {
    GOOGLESQL_ASSIGN_OR_RETURN(std::string name, ResolveRole(grantee.name()));
    if (name != kPublicRole && IsSystemRole(name)) {
      return error::CannotGrantPrivilegesToSystemRole(name);
    }
    grantee_names.push_back(name);
  }

  GOOGLESQL_ASSIGN_OR_RETURN(std::vector<Object> objects, ResolveObjects(target));
  std::vector<Grants::Privilege> result;
  for (const Object& object : objects) {
    for (const ddl::Privilege& privilege : privileges) {
      const std::string& privilege_name =
          ddl::Privilege::Type_Name(privilege.type());
      if (!IsValidPrivilege(privilege.type(), object.type)) {
        return error::InvalidPrivilegeForObject(
            privilege_name, ObjectKind(object.type), object.name);
      }
      // A null column stands for the whole object.
      std::vector<const Column*> columns;
      if (privilege.column().empty()) {
        columns.push_back(nullptr);
      } else {
        if (object.type != ddl::PrivilegeTarget::TABLE ||
            privilege.type() == ddl::Privilege::DELETE) {
          return error::ColumnPrivilegeNotAllowed(
              privilege_name, ObjectKind(object.type), object.name);
        }
        const Table* table = object.node->As<const Table>();
        for (const std::string& column_name : privilege.column()) {
          const Column* column = table->FindColumnCaseSensitive(column_name);
          if (column == nullptr) {
            return error::ColumnNotFound(table->Name(), column_name);
          }
          if (column->is_generated() &&
              privilege.type() != ddl::Privilege::SELECT) {
            return error::PrivilegeOnGeneratedColumn(
                privilege_name, table->Name(), column->Name());
          }
          columns.push_back(column);
        }
      }
      for (const Column* column : columns) {
        for (const std::string& grantee : grantee_names) {
          result.push_back({.type = privilege.type(),
                            .object_type = object.type,
                            .object = object.node,
                            .column = column,
                            .grantee = grantee,
                            .grantee_role = schema_->FindRole(grantee)});
        }
      }
    }
  }
  return result;
}

absl::StatusOr<std::vector<GrantResolver::Object>>
GrantResolver::ResolveObjects(const ddl::PrivilegeTarget& target) const {
  std::vector<Object> objects;
  for (const std::string& schema_name : target.all_in_schema()) {
    GOOGLESQL_ASSIGN_OR_RETURN(std::vector<Object> objects_in_schema,
                               ResolveObjectsInSchema(target.type(),
                                                      schema_name));
    objects.insert(objects.end(), objects_in_schema.begin(),
                   objects_in_schema.end());
  }
  for (const std::string& name : target.name()) {
    GOOGLESQL_ASSIGN_OR_RETURN(Object object,
                               ResolveObject(target.type(), name));
    objects.push_back(object);
  }
  return objects;
}

absl::StatusOr<GrantResolver::Object> GrantResolver::ResolveObject(
    ddl::PrivilegeTarget::Type type, const std::string& name) const {
  switch (type) {
    case ddl::PrivilegeTarget::TABLE: {
      if (const Table* table = schema_->FindTableCaseSensitive(name);
          table != nullptr) {
        return Object{type, table, table->Name()};
      }
      // PostgreSQL grants privileges on views as privileges on tables.
      if (dialect_ == DatabaseDialect::POSTGRESQL) {
        if (const View* view = schema_->FindViewCaseSensitive(name);
            view != nullptr) {
          return Object{ddl::PrivilegeTarget::VIEW, view, view->Name()};
        }
      }
      return error::TableNotFound(name);
    }
    case ddl::PrivilegeTarget::VIEW: {
      if (const View* view = schema_->FindViewCaseSensitive(name);
          view != nullptr) {
        return Object{type, view, view->Name()};
      }
      return error::ViewNotFound(name);
    }
    case ddl::PrivilegeTarget::CHANGE_STREAM: {
      if (const ChangeStream* change_stream =
              MatchingCase(schema_->FindChangeStream(name), name);
          change_stream != nullptr) {
        return Object{type, change_stream, change_stream->Name()};
      }
      return error::ChangeStreamNotFound(name);
    }
    case ddl::PrivilegeTarget::TABLE_FUNCTION:
    case ddl::PrivilegeTarget::FUNCTION:
    case ddl::PrivilegeTarget::ROUTINE: {
      // The only table functions that privileges apply to are the read
      // functions of change streams.
      for (const ChangeStream* change_stream : schema_->change_streams()) {
        if (change_stream->tvf_name() == name) {
          return Object{ddl::PrivilegeTarget::TABLE_FUNCTION, change_stream,
                        name};
        }
      }
      return error::TableValuedFunctionNotFound(name);
    }
    case ddl::PrivilegeTarget::SEQUENCE: {
      if (const Sequence* sequence = MatchingCase(
              schema_->FindSequence(name, /*exclude_internal=*/true), name);
          sequence != nullptr) {
        return Object{type, sequence, sequence->Name()};
      }
      return error::SequenceNotFound(name);
    }
    case ddl::PrivilegeTarget::MODEL: {
      if (const Model* model = MatchingCase(schema_->FindModel(name), name);
          model != nullptr) {
        return Object{type, model, model->Name()};
      }
      return error::ModelNotFound(name);
    }
    case ddl::PrivilegeTarget::SCHEMA: {
      if (name.empty()) {
        return Object{type, nullptr, name};
      }
      if (const NamedSchema* named_schema =
              MatchingCase(schema_->FindNamedSchema(name), name);
          named_schema != nullptr) {
        return Object{type, named_schema, named_schema->Name()};
      }
      return error::NamedSchemaNotFound(name);
    }
  }
  GOOGLESQL_RET_CHECK_FAIL() << "Unexpected privilege target type " << type;
}

absl::StatusOr<std::vector<GrantResolver::Object>>
GrantResolver::ResolveObjectsInSchema(ddl::PrivilegeTarget::Type type,
                                      const std::string& schema_name) const {
  if (!schema_name.empty() &&
      MatchingCase(schema_->FindNamedSchema(schema_name), schema_name) ==
          nullptr) {
    return error::NamedSchemaNotFound(schema_name);
  }
  // Only the objects that exist now are covered.
  std::vector<Object> objects;
  auto add = [&](const auto* node) {
    if (SDLObjectName::GetSchemaName(node->Name()) == schema_name) {
      objects.push_back(Object{type, node, node->Name()});
    }
  };
  switch (type) {
    case ddl::PrivilegeTarget::TABLE:
      absl::c_for_each(schema_->tables(), add);
      break;
    case ddl::PrivilegeTarget::VIEW:
      absl::c_for_each(schema_->views(), add);
      break;
    case ddl::PrivilegeTarget::CHANGE_STREAM:
      absl::c_for_each(schema_->change_streams(), add);
      break;
    case ddl::PrivilegeTarget::SEQUENCE:
      absl::c_for_each(schema_->user_visible_sequences(), add);
      break;
    default:
      GOOGLESQL_RET_CHECK_FAIL()
          << "Unexpected ALL IN SCHEMA privilege target type " << type;
  }
  return objects;
}

absl::StatusOr<std::vector<Grants::Membership>>
GrantResolver::ResolveMemberships(
    const google::protobuf::RepeatedPtrField<ddl::Grantee>& roles,
    const google::protobuf::RepeatedPtrField<ddl::Grantee>& members,
    bool is_grant) const {
  std::vector<std::string> role_names;
  for (const ddl::Grantee& role : roles) {
    GOOGLESQL_ASSIGN_OR_RETURN(std::string name, ResolveRole(role.name()));
    if (name == kPublicRole) {
      return error::CannotGrantMembershipInPublicRole();
    }
    role_names.push_back(name);
  }
  std::vector<std::string> member_names;
  for (const ddl::Grantee& member : members) {
    if (IsSystemRole(member.name())) {
      return error::SystemRoleCannotBeMember(
          absl::AsciiStrToLower(member.name()));
    }
    GOOGLESQL_ASSIGN_OR_RETURN(std::string name, ResolveRole(member.name()));
    member_names.push_back(name);
  }

  // Memberships granted so far, to detect cycles.
  std::vector<Grants::Membership> memberships;
  if (schema_->grants() != nullptr) {
    memberships.assign(schema_->grants()->memberships().begin(),
                       schema_->grants()->memberships().end());
  }
  std::vector<Grants::Membership> result;
  for (const std::string& role : role_names) {
    for (const std::string& member : member_names) {
      Grants::Membership membership{.role = role,
                                    .member = member,
                                    .role_node = schema_->FindRole(role),
                                    .member_node = schema_->FindRole(member)};
      if (is_grant) {
        if (absl::EqualsIgnoreCase(role, member) ||
            absl::c_any_of(AncestorRoles(role, memberships),
                           [&](const std::string& ancestor) {
                             return absl::EqualsIgnoreCase(ancestor, member);
                           })) {
          return error::RoleMembershipCycle(role, member);
        }
        memberships.push_back(membership);
      }
      result.push_back(membership);
    }
  }
  return result;
}

std::vector<std::string> GrantResolver::AncestorRoles(
    absl::string_view role, absl::Span<const Grants::Membership> memberships) {
  std::vector<std::string> ancestors;
  std::vector<std::string> pending = {std::string(role)};
  while (!pending.empty()) {
    const std::string current = pending.back();
    pending.pop_back();
    for (const Grants::Membership& membership : memberships) {
      if (absl::EqualsIgnoreCase(membership.member, current) &&
          absl::c_none_of(ancestors, [&](const std::string& ancestor) {
            return absl::EqualsIgnoreCase(ancestor, membership.role);
          })) {
        ancestors.push_back(membership.role);
        pending.push_back(membership.role);
      }
    }
  }
  return ancestors;
}

}  // namespace backend
}  // namespace emulator
}  // namespace spanner
}  // namespace google

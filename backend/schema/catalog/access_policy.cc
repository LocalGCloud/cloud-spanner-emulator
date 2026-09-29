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

#include "backend/schema/catalog/access_policy.h"

#include <string>
#include <vector>

#include "absl/algorithm/container.h"
#include "absl/status/status.h"
#include "absl/status/statusor.h"
#include "absl/strings/ascii.h"
#include "absl/strings/match.h"
#include "absl/strings/string_view.h"
#include "absl/types/span.h"
#include "backend/schema/catalog/grants.h"
#include "backend/schema/catalog/index.h"
#include "backend/schema/catalog/named_schema.h"
#include "backend/schema/catalog/role.h"
#include "backend/schema/catalog/sequence.h"
#include "backend/schema/catalog/table.h"
#include "backend/schema/catalog/view.h"
#include "common/errors.h"
#include "googlesql/base/status_macros.h"

namespace google {
namespace spanner {
namespace emulator {
namespace backend {

absl::StatusOr<AccessPolicy> AccessPolicy::Create(const Schema* schema,
                                                  absl::string_view role) {
  AccessPolicy policy;
  policy.schema_ = schema;
  if (IsSystemRole(role)) {
    policy.role_ = absl::AsciiStrToLower(role);
  } else if (const Role* schema_role = schema->FindRole(std::string(role));
             schema_role != nullptr) {
    policy.role_ = schema_role->Name();
  } else {
    return error::RoleNotFound(role);
  }

  const Grants* grants = schema->grants();
  policy.effective_roles_ = {policy.role_};
  if (grants != nullptr) {
    // Memberships are acyclic, so following them from the role terminates.
    for (int i = 0; i < policy.effective_roles_.size(); ++i) {
      for (const Grants::Membership& membership : grants->memberships()) {
        if (absl::EqualsIgnoreCase(membership.member,
                                   policy.effective_roles_[i]) &&
            !policy.IsEffectiveRole(membership.role)) {
          policy.effective_roles_.push_back(membership.role);
        }
      }
    }
  }
  if (!policy.IsEffectiveRole(kPublicRole)) {
    policy.effective_roles_.push_back(kPublicRole);
  }

  policy.default_schema_usage_ =
      grants == nullptr || !grants->public_default_schema_usage_revoked();
  if (grants != nullptr) {
    for (const Grants::Privilege& privilege : grants->privileges()) {
      if (!policy.IsEffectiveRole(privilege.grantee)) {
        continue;
      }
      if (privilege.object_type == ddl::PrivilegeTarget::SCHEMA &&
          privilege.object == nullptr) {
        policy.default_schema_usage_ = true;
        continue;
      }
      policy.privileges_.insert(
          {privilege.type, privilege.object, privilege.column});
      policy.objects_with_privileges_.insert(privilege.object);
    }
  }
  return policy;
}

bool AccessPolicy::IsEffectiveRole(absl::string_view role) const {
  return absl::c_any_of(effective_roles_, [&](const std::string& effective) {
    return absl::EqualsIgnoreCase(effective, role);
  });
}

bool AccessPolicy::unfiltered_information_schema() const {
  return IsEffectiveRole(kSpannerInfoReaderRole);
}

bool AccessPolicy::can_read_spanner_sys() const {
  return IsEffectiveRole(kSpannerSysReaderRole);
}

bool AccessPolicy::Has(ddl::Privilege::Type type, const SchemaNode* object,
                       const Column* column) const {
  return privileges_.contains({type, object, nullptr}) ||
         (column != nullptr && privileges_.contains({type, object, column}));
}

bool AccessPolicy::HasOnAnyColumn(ddl::Privilege::Type type,
                                  const Table* table) const {
  return absl::c_any_of(table->columns(), [&](const Column* column) {
    return Has(type, table, column);
  });
}

bool AccessPolicy::HasAny(const SchemaNode* object) const {
  return objects_with_privileges_.contains(object);
}

bool AccessPolicy::HasSchemaUsage(absl::string_view object_name) const {
  const absl::string_view schema_name =
      SDLObjectName::GetSchemaName(object_name);
  if (schema_name.empty()) {
    return default_schema_usage_;
  }
  const NamedSchema* named_schema =
      schema_->FindNamedSchema(std::string(schema_name));
  return named_schema != nullptr &&
         Has(ddl::Privilege::USAGE, named_schema);
}

absl::Status AccessPolicy::CheckSchemaUsage(
    absl::string_view object_name) const {
  if (HasSchemaUsage(object_name)) {
    return absl::OkStatus();
  }
  absl::string_view schema_name = SDLObjectName::GetSchemaName(object_name);
  if (schema_name.empty()) {
    schema_name = schema_->dialect() == database_api::DatabaseDialect::POSTGRESQL
                      ? "public"
                      : "DEFAULT";
  }
  return PrivilegeError("schema", schema_name);
}

namespace {

constexpr ddl::Privilege::Type kColumnVisibilityPrivileges[] = {
    ddl::Privilege::SELECT, ddl::Privilege::INSERT, ddl::Privilege::UPDATE};

}  // namespace

bool AccessPolicy::CanSeeTable(const Table* table) const {
  return HasAny(table);
}

bool AccessPolicy::CanSeeColumn(const Column* column) const {
  // The columns of an index stand for the columns of the indexed table.
  if (column->source_column() != nullptr) {
    column = column->source_column();
  }
  return absl::c_any_of(kColumnVisibilityPrivileges,
                        [&](ddl::Privilege::Type type) {
                          return Has(type, column->table(), column);
                        });
}

bool AccessPolicy::CanSeeIndex(const Table* table, const Index* index,
                               bool table_delete_suffices) const {
  if (table_delete_suffices && Has(ddl::Privilege::DELETE, table)) {
    return true;
  }
  std::vector<const Column*> columns;
  for (const KeyColumn* key_column :
       index == nullptr ? table->primary_key() : index->key_columns()) {
    columns.push_back(key_column->column());
  }
  if (index != nullptr) {
    for (const KeyColumn* key_column : index->order_by()) {
      columns.push_back(key_column->column());
    }
    columns.insert(columns.end(), index->stored_columns().begin(),
                   index->stored_columns().end());
    columns.insert(columns.end(), index->partition_by().begin(),
                   index->partition_by().end());
  }
  return absl::c_all_of(
      columns, [&](const Column* column) { return CanSeeColumn(column); });
}

bool AccessPolicy::CanSeeView(const View* view) const {
  return Has(ddl::Privilege::SELECT, view);
}

bool AccessPolicy::CanSeeSequence(const Sequence* sequence) const {
  return Has(ddl::Privilege::SELECT, sequence) ||
         Has(ddl::Privilege::UPDATE, sequence);
}

bool AccessPolicy::CanSeeRoutine(const SchemaNode* routine) const {
  return Has(ddl::Privilege::EXECUTE, routine);
}

absl::Status AccessPolicy::CheckDefaultValueSequences(
    absl::Span<const Column* const> columns) const {
  for (const Column* column : columns) {
    if (!column->has_default_value()) {
      continue;
    }
    for (const SchemaNode* node : column->sequences_used()) {
      const auto* sequence = node->As<const Sequence>();
      if (sequence == nullptr || sequence->is_internal_use()) {
        continue;
      }
      GOOGLESQL_RETURN_IF_ERROR(CheckSchemaUsage(sequence->Name()));
      if (!Has(ddl::Privilege::SELECT, sequence) &&
          !Has(ddl::Privilege::UPDATE, sequence)) {
        return PrivilegeError("sequence", sequence->Name());
      }
    }
  }
  return absl::OkStatus();
}

absl::Status AccessPolicy::PrivilegeError(absl::string_view object_kind,
                                          absl::string_view object_name) const {
  return error::RoleLacksPrivileges(role_, object_kind, object_name);
}

}  // namespace backend
}  // namespace emulator
}  // namespace spanner
}  // namespace google

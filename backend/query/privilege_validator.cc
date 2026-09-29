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

#include "backend/query/privilege_validator.h"

#include <string>
#include <vector>

#include "googlesql/public/catalog.h"
#include "googlesql/resolved_ast/resolved_ast.h"
#include "googlesql/resolved_ast/resolved_node_kind.pb.h"
#include "absl/algorithm/container.h"
#include "absl/container/flat_hash_set.h"
#include "absl/status/status.h"
#include "absl/strings/ascii.h"
#include "absl/strings/match.h"
#include "absl/types/span.h"
#include "backend/query/queryable_column.h"
#include "backend/query/queryable_model.h"
#include "backend/query/queryable_sequence.h"
#include "backend/query/queryable_table.h"
#include "backend/query/queryable_view.h"
#include "backend/schema/catalog/column.h"
#include "backend/schema/catalog/sequence.h"
#include "backend/schema/catalog/table.h"
#include "common/constants.h"
#include "common/pg_literals.h"
#include "googlesql/base/status_macros.h"

namespace google {
namespace spanner {
namespace emulator {
namespace backend {

namespace {

// Returns the schema column read by the i-th column of a table scan.
const Column* ScannedColumn(const googlesql::ResolvedTableScan* scan, int i) {
  return static_cast<const QueryableColumn*>(
             scan->table()->GetColumn(scan->column_index_list(i)))
      ->wrapped_column();
}

bool IsDefault(const googlesql::ResolvedDMLValue* value) {
  return value != nullptr &&
         value->value()->node_kind() == googlesql::RESOLVED_DMLDEFAULT;
}

}  // namespace

absl::Status PrivilegeValidator::VisitResolvedTableScan(
    const googlesql::ResolvedTableScan* node) {
  const googlesql::Table* table = node->table();
  if (dml_target_scans_.contains(node)) {
    return DefaultVisit(node);
  }
  if (const auto* queryable_table = dynamic_cast<const QueryableTable*>(table);
      queryable_table != nullptr) {
    const Table* schema_table = queryable_table->wrapped_table();
    GOOGLESQL_RETURN_IF_ERROR(access_->CheckSchemaUsage(schema_table->Name()));
    bool allowed =
        node->column_index_list().empty()
            // A scan without columns, such as for COUNT(*), needs SELECT on
            // at least one column.
            ? access_->HasOnAnyColumn(ddl::Privilege::SELECT, schema_table)
            : true;
    for (int i = 0; i < node->column_index_list_size(); ++i) {
      allowed = allowed && access_->Has(ddl::Privilege::SELECT, schema_table,
                                        ScannedColumn(node, i));
    }
    if (!allowed) {
      return access_->PrivilegeError("table", schema_table->Name());
    }
  } else if (const auto* view = dynamic_cast<const QueryableView*>(table);
             view != nullptr) {
    GOOGLESQL_RETURN_IF_ERROR(
        access_->CheckSchemaUsage(view->wrapped_view()->Name()));
    if (!access_->Has(ddl::Privilege::SELECT, view->wrapped_view())) {
      return access_->PrivilegeError("view", view->wrapped_view()->Name());
    }
    // Check the objects that an INVOKER view reads before it is evaluated, so
    // that PLAN mode, which evaluates nothing, checks them too.
    GOOGLESQL_RETURN_IF_ERROR(view->CheckInvokerPrivileges());
  } else if (absl::StartsWithIgnoreCase(table->FullName(), "SPANNER_SYS.") &&
             !access_->can_read_spanner_sys()) {
    return access_->PrivilegeError("table", table->FullName());
  }
  return DefaultVisit(node);
}

absl::Status PrivilegeValidator::CheckDmlTarget(
    const googlesql::ResolvedTableScan* scan,
    absl::Span<const googlesql::ResolvedStatement::ObjectAccess>
        column_access_list,
    ddl::Privilege::Type write_privilege) {
  dml_target_scans_.insert(scan);
  const auto* queryable_table =
      dynamic_cast<const QueryableTable*>(scan->table());
  if (queryable_table == nullptr) {
    return absl::OkStatus();
  }
  const Table* table = queryable_table->wrapped_table();
  GOOGLESQL_RETURN_IF_ERROR(access_->CheckSchemaUsage(table->Name()));
  for (int i = 0; i < column_access_list.size(); ++i) {
    const Column* column = ScannedColumn(scan, i);
    if (((column_access_list[i] & googlesql::ResolvedStatement::READ) &&
         !access_->Has(ddl::Privilege::SELECT, table, column)) ||
        ((column_access_list[i] & googlesql::ResolvedStatement::WRITE) &&
         !access_->Has(write_privilege, table, column))) {
      return access_->PrivilegeError("table", table->Name());
    }
  }
  return absl::OkStatus();
}

absl::Status PrivilegeValidator::CheckInsertDefaultValues(
    const googlesql::ResolvedInsertStmt* node) {
  const auto* queryable_table =
      dynamic_cast<const QueryableTable*>(node->table_scan()->table());
  if (queryable_table == nullptr) {
    return absl::OkStatus();
  }
  const Table* table = queryable_table->wrapped_table();
  // An INSERT computes the default values of the columns that it does not
  // list and of the columns that it sets to DEFAULT.
  std::vector<const Column*> defaulted_columns;
  absl::flat_hash_set<const Column*> listed_columns;
  for (int i = 0; i < node->insert_column_list_size(); ++i) {
    const Column* column =
        table->FindColumn(node->insert_column_list(i).name());
    if (column == nullptr) {
      continue;
    }
    listed_columns.insert(column);
    if (absl::c_any_of(node->row_list(), [i](const auto& row) {
          return IsDefault(row->value_list(i));
        })) {
      defaulted_columns.push_back(column);
    }
  }
  for (const Column* column : table->columns()) {
    if (!listed_columns.contains(column)) {
      defaulted_columns.push_back(column);
    }
  }
  return access_->CheckDefaultValueSequences(defaulted_columns);
}

absl::Status PrivilegeValidator::VisitResolvedInsertStmt(
    const googlesql::ResolvedInsertStmt* node) {
  GOOGLESQL_RETURN_IF_ERROR(CheckDmlTarget(
      node->table_scan(), node->column_access_list(), ddl::Privilege::INSERT));
  GOOGLESQL_RETURN_IF_ERROR(CheckInsertDefaultValues(node));
  // INSERT OR UPDATE and INSERT ... ON CONFLICT DO UPDATE may also update the
  // written columns.
  if (node->insert_mode() == googlesql::ResolvedInsertStmt::OR_UPDATE ||
      (node->on_conflict_clause() != nullptr &&
       !node->on_conflict_clause()->update_item_list().empty())) {
    GOOGLESQL_RETURN_IF_ERROR(CheckDmlTarget(node->table_scan(),
                                             node->column_access_list(),
                                             ddl::Privilege::UPDATE));
  }
  return DefaultVisit(node);
}

absl::Status PrivilegeValidator::VisitResolvedUpdateStmt(
    const googlesql::ResolvedUpdateStmt* node) {
  GOOGLESQL_RETURN_IF_ERROR(CheckDmlTarget(
      node->table_scan(), node->column_access_list(), ddl::Privilege::UPDATE));
  // SET column = DEFAULT computes the column's default value.
  if (const auto* queryable_table =
          dynamic_cast<const QueryableTable*>(node->table_scan()->table());
      queryable_table != nullptr) {
    std::vector<const Column*> defaulted_columns;
    for (const auto& item : node->update_item_list()) {
      if (!IsDefault(item->set_value()) ||
          item->target()->node_kind() != googlesql::RESOLVED_COLUMN_REF) {
        continue;
      }
      if (const Column* column = queryable_table->wrapped_table()->FindColumn(
              item->target()
                  ->GetAs<googlesql::ResolvedColumnRef>()
                  ->column()
                  .name());
          column != nullptr) {
        defaulted_columns.push_back(column);
      }
    }
    GOOGLESQL_RETURN_IF_ERROR(
        access_->CheckDefaultValueSequences(defaulted_columns));
  }
  return DefaultVisit(node);
}

absl::Status PrivilegeValidator::VisitResolvedDeleteStmt(
    const googlesql::ResolvedDeleteStmt* node) {
  GOOGLESQL_RETURN_IF_ERROR(CheckDmlTarget(
      node->table_scan(), node->column_access_list(), ddl::Privilege::DELETE));
  if (const auto* table =
          dynamic_cast<const QueryableTable*>(node->table_scan()->table());
      table != nullptr &&
      !access_->Has(ddl::Privilege::DELETE, table->wrapped_table())) {
    return access_->PrivilegeError("table", table->wrapped_table()->Name());
  }
  return DefaultVisit(node);
}

absl::Status PrivilegeValidator::VisitResolvedModel(
    const googlesql::ResolvedModel* node) {
  if (const auto* model = dynamic_cast<const QueryableModel*>(node->model());
      model != nullptr) {
    GOOGLESQL_RETURN_IF_ERROR(
        access_->CheckSchemaUsage(model->wrapped_model()->Name()));
    if (!access_->Has(ddl::Privilege::EXECUTE, model->wrapped_model())) {
      return access_->PrivilegeError("model", model->wrapped_model()->Name());
    }
  }
  return DefaultVisit(node);
}

absl::Status PrivilegeValidator::VisitResolvedFunctionCall(
    const googlesql::ResolvedFunctionCall* node) {
  const std::string function_name =
      absl::AsciiStrToLower(node->function()->Name());
  const bool next_value =
      absl::EndsWith(function_name, kGetNextSequenceValueFunctionName) ||
      absl::EndsWith(function_name, "nextval");
  const bool internal_state =
      absl::EndsWith(function_name, kGetInternalSequenceStateFunctionName);
  if (!next_value && !internal_state) {
    return DefaultVisit(node);
  }
  // GoogleSQL names the sequence with a SEQUENCE argument and PostgreSQL with
  // a string literal.
  const Sequence* sequence = nullptr;
  if (node->generic_argument_list_size() == 1 &&
      node->generic_argument_list(0)->sequence() != nullptr) {
    if (const auto* queryable_sequence = dynamic_cast<const QueryableSequence*>(
            node->generic_argument_list(0)->sequence()->sequence());
        queryable_sequence != nullptr) {
      sequence = queryable_sequence->wrapped_sequence();
    }
  } else if (node->argument_list_size() == 1 &&
             node->argument_list(0)->node_kind() ==
                 googlesql::RESOLVED_LITERAL) {
    const googlesql::Value& value =
        node->argument_list(0)->GetAs<googlesql::ResolvedLiteral>()->value();
    if (value.type()->IsString() && !value.is_null()) {
      sequence = schema_->FindSequence(
          schema_->dialect() == database_api::DatabaseDialect::POSTGRESQL
              ? GetFullyQualifiedNameFromPgLiteral(value.string_value())
              : value.string_value());
    }
  }
  if (sequence != nullptr) {
    GOOGLESQL_RETURN_IF_ERROR(access_->CheckSchemaUsage(sequence->Name()));
    // GET_NEXT_SEQUENCE_VALUE needs SELECT or UPDATE on the sequence and
    // GET_INTERNAL_SEQUENCE_STATE needs SELECT.
    if (!access_->Has(ddl::Privilege::SELECT, sequence) &&
        !(next_value && access_->Has(ddl::Privilege::UPDATE, sequence))) {
      return access_->PrivilegeError("sequence", sequence->Name());
    }
  }
  return DefaultVisit(node);
}

}  // namespace backend
}  // namespace emulator
}  // namespace spanner
}  // namespace google

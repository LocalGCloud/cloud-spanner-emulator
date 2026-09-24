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

#include "backend/query/placement_dml_validator.h"

#include <optional>
#include <string>
#include <vector>

#include "googlesql/public/catalog.h"
#include "googlesql/resolved_ast/resolved_ast.h"
#include "googlesql/resolved_ast/resolved_ast_visitor.h"
#include "absl/container/flat_hash_map.h"
#include "absl/status/status.h"
#include "backend/query/queryable_table.h"
#include "backend/schema/catalog/column.h"
#include "backend/schema/catalog/schema.h"
#include "backend/schema/catalog/table.h"
#include "common/errors.h"
#include "googlesql/base/status_macros.h"

namespace google {
namespace spanner {
namespace emulator {
namespace backend {

namespace {

// Returns the placement table that a resolved table scans, or nullptr if the
// scanned table isn't a placement table (or isn't a schema table at all, such
// as a view or an INFORMATION_SCHEMA table).
const Table* ScannedPlacementTable(const googlesql::Table* table) {
  if (table == nullptr || !table->Is<QueryableTable>()) {
    return nullptr;
  }
  const Table* schema_table = table->GetAs<QueryableTable>()->wrapped_table();
  return IsPlacementTable(schema_table) ? schema_table : nullptr;
}

struct PlacementColumn {
  const Table* table;
  const Column* column;
};

// Maps every column read from a placement table in a statement to its table
// column.
class PlacementColumnCollector : public googlesql::ResolvedASTVisitor {
 public:
  const absl::flat_hash_map<int, PlacementColumn>& columns() const {
    return columns_;
  }

 private:
  absl::Status VisitResolvedTableScan(
      const googlesql::ResolvedTableScan* node) override {
    const Table* table = ScannedPlacementTable(node->table());
    if (table != nullptr) {
      for (int i = 0; i < node->column_list_size(); ++i) {
        const googlesql::ResolvedColumn& resolved_column = node->column_list(i);
        std::string column_name = resolved_column.name();
        if (i < node->column_index_list_size()) {
          const googlesql::Column* scanned_column =
              node->table()->GetColumn(node->column_index_list(i));
          if (scanned_column != nullptr) {
            column_name = scanned_column->Name();
          }
        }
        const Column* column = table->FindColumn(column_name);
        if (column != nullptr) {
          columns_[resolved_column.column_id()] = {table, column};
        }
      }
    }
    return DefaultVisit(node);
  }

  absl::flat_hash_map<int, PlacementColumn> columns_;
};

// Collects the columns that an expression references directly. Subqueries are
// not entered: their own WHERE clauses are checked separately, and a column
// that a subquery only selects isn't referenced by the enclosing WHERE clause.
class ExpressionColumnCollector : public googlesql::ResolvedASTVisitor {
 public:
  const std::vector<int>& column_ids() const { return column_ids_; }

 private:
  absl::Status VisitResolvedColumnRef(
      const googlesql::ResolvedColumnRef* node) override {
    column_ids_.push_back(node->column().column_id());
    return DefaultVisit(node);
  }

  absl::Status VisitResolvedSubqueryExpr(
      const googlesql::ResolvedSubqueryExpr* node) override {
    // The left-hand side of `expr IN (subquery)` belongs to the enclosing
    // expression.
    if (node->in_expr() != nullptr) {
      GOOGLESQL_RETURN_IF_ERROR(node->in_expr()->Accept(this));
    }
    return absl::OkStatus();
  }

  std::vector<int> column_ids_;
};

// Rejects WHERE clauses that reference non-key columns of placement tables.
class WhereClauseValidator : public googlesql::ResolvedASTVisitor {
 public:
  explicit WhereClauseValidator(
      const absl::flat_hash_map<int, PlacementColumn>* placement_columns)
      : placement_columns_(placement_columns) {}

 private:
  absl::Status VisitResolvedFilterScan(
      const googlesql::ResolvedFilterScan* node) override {
    GOOGLESQL_RETURN_IF_ERROR(ValidateWhereClause(node->filter_expr()));
    return DefaultVisit(node);
  }

  absl::Status VisitResolvedUpdateStmt(
      const googlesql::ResolvedUpdateStmt* node) override {
    GOOGLESQL_RETURN_IF_ERROR(ValidateWhereClause(node->where_expr()));
    return DefaultVisit(node);
  }

  absl::Status VisitResolvedDeleteStmt(
      const googlesql::ResolvedDeleteStmt* node) override {
    GOOGLESQL_RETURN_IF_ERROR(ValidateWhereClause(node->where_expr()));
    return DefaultVisit(node);
  }

  absl::Status ValidateWhereClause(const googlesql::ResolvedExpr* expr) {
    if (expr == nullptr) {
      return absl::OkStatus();
    }
    ExpressionColumnCollector collector;
    GOOGLESQL_RETURN_IF_ERROR(expr->Accept(&collector));
    for (int column_id : collector.column_ids()) {
      auto it = placement_columns_->find(column_id);
      if (it == placement_columns_->end()) {
        continue;
      }
      const PlacementColumn& placement_column = it->second;
      if (placement_column.table->FindKeyColumn(
              placement_column.column->Name()) == nullptr) {
        return error::PlacementTableNonKeyColumnInWhereClause(
            placement_column.table->Name(), placement_column.column->Name());
      }
    }
    return absl::OkStatus();
  }

  const absl::flat_hash_map<int, PlacementColumn>* placement_columns_;
};

}  // namespace

bool IsPlacementTable(const Table* table) {
  if (table == nullptr) {
    return false;
  }
  for (const Column* column : table->columns()) {
    if (column->is_placement_key()) {
      return true;
    }
  }
  return false;
}

bool HasPlacementTables(const Schema* schema) {
  if (schema == nullptr) {
    return false;
  }
  for (const Table* table : schema->tables()) {
    if (IsPlacementTable(table)) {
      return true;
    }
  }
  return false;
}

std::optional<std::string> PlacementInsertOrDeleteTable(
    const googlesql::ResolvedStatement* statement) {
  const googlesql::ResolvedTableScan* target = nullptr;
  if (statement->Is<googlesql::ResolvedInsertStmt>()) {
    target = statement->GetAs<googlesql::ResolvedInsertStmt>()->table_scan();
  } else if (statement->Is<googlesql::ResolvedDeleteStmt>()) {
    target = statement->GetAs<googlesql::ResolvedDeleteStmt>()->table_scan();
  }
  if (target == nullptr) {
    return std::nullopt;
  }
  const Table* table = ScannedPlacementTable(target->table());
  if (table == nullptr) {
    return std::nullopt;
  }
  return table->Name();
}

absl::Status PlacementDmlValidator::Validate(
    const googlesql::ResolvedStatement* statement) {
  insert_or_delete_table_.reset();

  // Map columns to tables over the whole statement first, so that correlated
  // references resolve regardless of the order in which scans are visited.
  PlacementColumnCollector collector;
  GOOGLESQL_RETURN_IF_ERROR(statement->Accept(&collector));
  if (!collector.columns().empty()) {
    WhereClauseValidator where_validator(&collector.columns());
    GOOGLESQL_RETURN_IF_ERROR(statement->Accept(&where_validator));
  }

  insert_or_delete_table_ = PlacementInsertOrDeleteTable(statement);
  return absl::OkStatus();
}

}  // namespace backend
}  // namespace emulator
}  // namespace spanner
}  // namespace google

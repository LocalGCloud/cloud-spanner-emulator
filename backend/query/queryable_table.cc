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

#include "backend/query/queryable_table.h"

#include <algorithm>
#include <memory>
#include <optional>
#include <string>
#include <utility>
#include <vector>

#include "googlesql/public/analyzer.h"
#include "googlesql/public/analyzer_options.h"
#include "googlesql/public/analyzer_output.h"
#include "googlesql/public/catalog.h"
#include "googlesql/public/evaluator_table_iterator.h"
#include "googlesql/public/types/type.h"
#include "googlesql/public/types/type_factory.h"
#include "googlesql/public/value.h"
#include "absl/container/flat_hash_map.h"
#include "absl/log/check.h"
#include "absl/log/log.h"
#include "absl/status/status.h"
#include "absl/status/statusor.h"
#include "absl/strings/match.h"
#include "absl/strings/string_view.h"
#include "absl/strings/strip.h"  //
#include "absl/types/span.h"
#include "backend/access/read.h"
#include "backend/datamodel/key.h"
#include "backend/datamodel/key_set.h"
#include "backend/query/queryable_column.h"
#include "backend/schema/catalog/column.h"
#include "common/constants.h"
#include "common/feature_flags.h"
#include "googlesql/base/ret_check.h"
#include "googlesql/base/status_macros.h"

namespace google {
namespace spanner {
namespace emulator {
namespace backend {

// Wraps a RowCursor. FOR UPDATE defers its read until the evaluator supplies
// scan filters, allowing an exact primary-key filter to narrow the locked range.
class RowCursorEvaluatorTableIterator
    : public googlesql::EvaluatorTableIterator {
 public:
  explicit RowCursorEvaluatorTableIterator(std::unique_ptr<RowCursor> cursor)
      : cursor_(std::move(cursor)) {
    for (int i = 0; i < cursor_->NumColumns(); ++i) {
      column_names_.push_back(cursor_->ColumnName(i));
      column_types_.push_back(cursor_->ColumnType(i));
      values_.push_back(googlesql::values::Null(cursor_->ColumnType(i)));
    }
  }

  RowCursorEvaluatorTableIterator(
      RowReader* reader, ReadArg read_arg,
      std::vector<std::string> column_names,
      std::vector<const googlesql::Type*> column_types,
      std::optional<int> first_key_scan_index,
      const googlesql::Type* first_key_type, bool key_descending,
      bool key_nulls_last)
      : reader_(reader),
        read_arg_(std::move(read_arg)),
        column_names_(std::move(column_names)),
        column_types_(std::move(column_types)),
        first_key_scan_index_(first_key_scan_index),
        first_key_type_(first_key_type),
        key_descending_(key_descending),
        key_nulls_last_(key_nulls_last) {
    for (const googlesql::Type* type : column_types_) {
      values_.push_back(googlesql::values::Null(type));
    }
  }

  int NumColumns() const override { return column_names_.size(); }

  std::string GetColumnName(int i) const override { return column_names_[i]; }

  const googlesql::Type* GetColumnType(int i) const override {
    return column_types_[i];
  }

  absl::Status SetColumnFilterMap(
      absl::flat_hash_map<int, std::unique_ptr<googlesql::ColumnFilter>>
          filter_map) override {
    if (reader_ == nullptr || !first_key_scan_index_.has_value()) {
      return absl::OkStatus();
    }
    auto it = filter_map.find(*first_key_scan_index_);
    if (it == filter_map.end() || it->second == nullptr) {
      return absl::OkStatus();
    }

    // GoogleSQL applies the filter again to returned rows. Narrow the storage
    // scan only when its filter proves exact point values for the first key
    // column. Other predicates keep the full-table exclusive scan lock.
    const googlesql::ColumnFilter& filter = *it->second;
    KeySet points;
    auto add_point = [&](const googlesql::Value& value) {
      if (!value.is_valid() || value.is_null() ||
          value.type() != first_key_type_) {
        return false;
      }
      Key key;
      key.AddColumn(value, key_descending_, key_nulls_last_);
      points.AddKey(key);
      return true;
    };
    switch (filter.kind()) {
      case googlesql::ColumnFilter::kInList:
        for (const auto& value : filter.in_list()) {
          if (!add_point(value)) {
            return absl::OkStatus();
          }
        }
        break;
      case googlesql::ColumnFilter::kRange:
        if (!filter.lower_bound().is_valid() ||
            !filter.upper_bound().is_valid() ||
            !filter.lower_bound().Equals(filter.upper_bound()) ||
            !add_point(filter.lower_bound())) {
          return absl::OkStatus();
        }
        break;
      default:
        return absl::OkStatus();
    }
    read_arg_.key_set = std::move(points);
    return absl::OkStatus();
  }

  bool NextRow() override {
    if (reader_ != nullptr) {
      status_ = reader_->Read(read_arg_, &cursor_);
      reader_ = nullptr;
      if (!status_.ok()) {
        return false;
      }
    }
    if (cursor_ == nullptr || !cursor_->Next()) {
      return false;
    }
    for (int i = 0; i < cursor_->NumColumns(); ++i) {
      values_[i] = cursor_->ColumnValue(i);
    }
    return true;
  }

  const googlesql::Value& GetValue(int i) const override { return values_[i]; }

  absl::Status Status() const override {
    if (!status_.ok()) {
      return status_;
    }
    return cursor_ != nullptr ? cursor_->Status() : status_;
  }

  // Cancel is best-effort and not required.
  absl::Status Cancel() override { return absl::OkStatus(); }

 private:
  RowReader* reader_ = nullptr;
  ReadArg read_arg_;
  std::unique_ptr<RowCursor> cursor_;
  absl::Status status_;
  std::vector<std::string> column_names_;
  std::vector<const googlesql::Type*> column_types_;
  std::optional<int> first_key_scan_index_;
  const googlesql::Type* first_key_type_ = nullptr;
  bool key_descending_ = false;
  bool key_nulls_last_ = false;

  // EvaluatorTableIterator::GetValue returns a reference to the buffered row.
  std::vector<googlesql::Value> values_;
};

absl::StatusOr<std::unique_ptr<const googlesql::AnalyzerOutput>>
QueryableTable::AnalyzeColumnExpression(
    const Column* column, googlesql::TypeFactory* type_factory,
    googlesql::Catalog* catalog,
    std::optional<const googlesql::AnalyzerOptions> opt_options) const {
  std::unique_ptr<const googlesql::AnalyzerOutput> output = nullptr;
  bool enable_generated_pk =
      EmulatorFeatureFlags::instance().flags().enable_generated_pk;
  bool is_generated_column = enable_generated_pk && column->is_generated();
  if (opt_options.has_value() &&
      (column->has_default_value() || (is_generated_column))) {
    googlesql::AnalyzerOptions options = opt_options.value();
    if (is_generated_column) {
      for (const Column* dep : column->dependent_columns()) {
        GOOGLESQL_RETURN_IF_ERROR(
            options.AddExpressionColumn(dep->Name(), dep->GetType()))
            << "Failed to add dependent column " << dep->Name()
            << " for generated column : " << column->FullName();
      }
    }
    std::string expression_type = "default";
    if (is_generated_column) {
      expression_type = "generated";
    }
    GOOGLESQL_RETURN_IF_ERROR(googlesql::AnalyzeExpressionForAssignmentToType(
        column->expression().value(), options, catalog, type_factory,
        column->GetType(), &output))
        << "Failed to analyze " << expression_type << " expression for column "
        << column->FullName();
  }
  return std::move(output);
}

QueryableTable::QueryableTable(
    const backend::Table* table, RowReader* reader,
    std::optional<const googlesql::AnalyzerOptions> opt_options,
    googlesql::Catalog* catalog, googlesql::TypeFactory* type_factory,
    bool is_synonym, const bool* select_for_update)
    : is_synonym_(is_synonym),
      wrapped_table_(table),
      reader_(reader),
      select_for_update_(select_for_update) {
  bool enable_generated_pk =
      EmulatorFeatureFlags::instance().flags().enable_generated_pk;
  for (const auto* column : table->columns()) {
    absl::StatusOr<std::unique_ptr<const googlesql::AnalyzerOutput>>
        analyzer_output =
            AnalyzeColumnExpression(column, type_factory, catalog, opt_options);
    ABSL_CHECK_OK(analyzer_output.status());  // Crash OK
    std::unique_ptr<const googlesql::AnalyzerOutput> output =
        std::move(analyzer_output.value());
    bool is_generated_column = enable_generated_pk && column->is_generated();
    if (column->has_default_value() || (is_generated_column)) {
      googlesql::Column::ExpressionAttributes::ExpressionKind expression_kind =
          googlesql::Column::ExpressionAttributes::ExpressionKind::DEFAULT;
      if (is_generated_column) {
        expression_kind =
            googlesql::Column::ExpressionAttributes::ExpressionKind::GENERATED;
      }
      googlesql::Column::ExpressionAttributes expression_attributes =
          googlesql::Column::ExpressionAttributes(expression_kind,
                                                  column->expression().value(),
                                                  output->resolved_expr());
      columns_.push_back(std::make_unique<const QueryableColumn>(
          column, std::move(output),
          std::make_optional(expression_attributes)));
    } else {
      columns_.push_back(std::make_unique<const QueryableColumn>(
          column, std::move(output), std::nullopt));
    }
  }

  // Populate primary_key_column_indexes_.
  for (const auto& key_column : table->primary_key()) {
    for (int i = 0; i < wrapped_table_->columns().size(); ++i) {
      if (key_column->column() == wrapped_table_->columns()[i]) {
        primary_key_column_indexes_.push_back(i);
        break;
      }
    }
  }
}

absl::StatusOr<std::unique_ptr<googlesql::EvaluatorTableIterator>>
QueryableTable::CreateEvaluatorTableIterator(
    absl::Span<const int> column_idxs) const {
  GOOGLESQL_RET_CHECK_NE(reader_, nullptr);

  std::vector<std::string> column_names;
  for (int idx : column_idxs) {
    column_names.push_back(GetColumn(idx)->Name());
  }

  ReadArg read_arg;
  read_arg.table = FullName();
  read_arg.key_set = KeySet::All();
  read_arg.columns = column_names;
  // Pending commit timestamp restrictions for queries are implemented in
  // QueryValidator so we do not need enforcement during the read here.
  // Furthermore, without enabling this certain internal reads issued by the
  // GoogleSQL reference implementation will be rejected.
  read_arg.allow_pending_commit_timestamps = true;

  // If current table is a change stream internal data/partition table, change
  // the read arg to access internal tables directly.
  if (wrapped_table_->owner_change_stream() != nullptr) {
    absl::string_view change_stream_name = read_arg.table;
    if (absl::StartsWith(read_arg.table, kChangeStreamPartitionTablePrefix)) {
      absl::ConsumePrefix(&change_stream_name,
                          kChangeStreamPartitionTablePrefix);
      read_arg.change_stream_for_partition_table = change_stream_name;
    } else {
      absl::ConsumePrefix(&change_stream_name, kChangeStreamDataTablePrefix);
      read_arg.change_stream_for_data_table = change_stream_name;
    }
  }
  if (select_for_update_ != nullptr && *select_for_update_) {
    read_arg.lock_scanned_ranges_exclusive = true;
    std::vector<const googlesql::Type*> column_types;
    for (int idx : column_idxs) {
      column_types.push_back(GetColumn(idx)->GetType());
    }

    std::optional<int> first_key_scan_index;
    const googlesql::Type* first_key_type = nullptr;
    bool key_descending = false;
    bool key_nulls_last = false;
    if (!primary_key_column_indexes_.empty()) {
      int first_key_table_index = primary_key_column_indexes_.front();
      auto it = std::find(column_idxs.begin(), column_idxs.end(),
                          first_key_table_index);
      if (it != column_idxs.end()) {
        first_key_scan_index = it - column_idxs.begin();
        first_key_type = GetColumn(first_key_table_index)->GetType();
        key_descending = wrapped_table_->primary_key().front()->is_descending();
        key_nulls_last = wrapped_table_->primary_key().front()->is_nulls_last();
      }
    }
    return std::make_unique<RowCursorEvaluatorTableIterator>(
        reader_, std::move(read_arg), std::move(column_names),
        std::move(column_types), first_key_scan_index, first_key_type,
        key_descending, key_nulls_last);
  }

  std::unique_ptr<RowCursor> cursor;
  GOOGLESQL_RETURN_IF_ERROR(reader_->Read(read_arg, &cursor));
  return std::make_unique<RowCursorEvaluatorTableIterator>(std::move(cursor));
}

const googlesql::Column* QueryableTable::FindColumnByName(
    const std::string& name) const {
  const auto* to_find = wrapped_table_->FindColumn(name);
  auto it = std::find_if(columns_.begin(), columns_.end(),
                         [to_find](const auto& column) {
                           return column->wrapped_column() == to_find;
                         });
  if (it == columns_.end()) {
    return nullptr;
  }
  return it->get();
}

}  // namespace backend
}  // namespace emulator
}  // namespace spanner
}  // namespace google

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

#include "backend/query/plan/scan_profiler.h"

#include <cstdint>
#include <memory>
#include <optional>
#include <string>
#include <utility>
#include <vector>

#include "googlesql/public/catalog.h"
#include "googlesql/public/evaluator_table_iterator.h"
#include "googlesql/public/type.h"
#include "googlesql/public/value.h"
#include "googlesql/resolved_ast/resolved_ast.h"
#include "googlesql/resolved_ast/resolved_ast_deep_copy_visitor.h"
#include "absl/container/flat_hash_map.h"
#include "absl/status/status.h"
#include "absl/status/statusor.h"
#include "absl/time/clock.h"
#include "absl/time/time.h"
#include "absl/types/span.h"
#include "backend/query/plan/query_plan_builder.h"
#include "googlesql/base/status_macros.h"

namespace google {
namespace spanner {
namespace emulator {
namespace backend {

namespace {

// Forwards to another iterator while measuring the rows it produces and the
// time spent producing them.
class ProfilingIterator : public googlesql::EvaluatorTableIterator {
 public:
  ProfilingIterator(std::unique_ptr<googlesql::EvaluatorTableIterator> iterator,
                    ScanProfile* profile)
      : iterator_(std::move(iterator)), profile_(profile) {}

  int NumColumns() const override { return iterator_->NumColumns(); }
  std::string GetColumnName(int i) const override {
    return iterator_->GetColumnName(i);
  }
  const googlesql::Type* GetColumnType(int i) const override {
    return iterator_->GetColumnType(i);
  }
  absl::Status SetColumnFilterMap(
      absl::flat_hash_map<int, std::unique_ptr<googlesql::ColumnFilter>>
          filter_map) override {
    return iterator_->SetColumnFilterMap(std::move(filter_map));
  }
  absl::Status SetReadTime(absl::Time read_time) override {
    return iterator_->SetReadTime(read_time);
  }
  bool NextRow() override {
    const absl::Time start = absl::Now();
    const bool has_row = iterator_->NextRow();
    profile_->latency += absl::Now() - start;
    profile_->rows += has_row;
    return has_row;
  }
  const googlesql::Value& GetValue(int i) const override {
    return iterator_->GetValue(i);
  }
  absl::Status Status() const override { return iterator_->Status(); }
  absl::Status Cancel() override { return iterator_->Cancel(); }
  void SetDeadline(absl::Time deadline) override {
    iterator_->SetDeadline(deadline);
  }

 private:
  std::unique_ptr<googlesql::EvaluatorTableIterator> iterator_;
  ScanProfile* profile_;
};

}  // namespace

// Forwards to another table while measuring the scans of the table.
class ScanProfiler::ProfilingTable : public googlesql::Table {
 public:
  explicit ProfilingTable(const googlesql::Table* table) : table_(table) {}

  std::string Name() const override { return table_->Name(); }
  std::string FullName() const override { return table_->FullName(); }
  int NumColumns() const override { return table_->NumColumns(); }
  const googlesql::Column* GetColumn(int i) const override {
    return table_->GetColumn(i);
  }
  std::optional<std::vector<int>> PrimaryKey() const override {
    return table_->PrimaryKey();
  }
  std::optional<std::vector<int>> RowIdentityColumns() const override {
    return table_->RowIdentityColumns();
  }
  const googlesql::Column* FindColumnByName(
      const std::string& name) const override {
    return table_->FindColumnByName(name);
  }
  bool IsValueTable() const override { return table_->IsValueTable(); }
  int64_t GetSerializationId() const override {
    return table_->GetSerializationId();
  }

  absl::StatusOr<std::unique_ptr<googlesql::EvaluatorTableIterator>>
  CreateEvaluatorTableIterator(
      absl::Span<const int> column_idxs) const override {
    const absl::Time start = absl::Now();
    ++profile_.executions;
    absl::StatusOr<std::unique_ptr<googlesql::EvaluatorTableIterator>>
        iterator = table_->CreateEvaluatorTableIterator(column_idxs);
    profile_.latency += absl::Now() - start;
    GOOGLESQL_RETURN_IF_ERROR(iterator.status());
    return std::make_unique<ProfilingIterator>(*std::move(iterator),
                                               &profile_);
  }

  const ScanProfile& profile() const { return profile_; }

 private:
  const googlesql::Table* table_;
  // Updated by the scans, which only have const access to the table.
  mutable ScanProfile profile_;
};

namespace {

// Copies a statement, replacing the table of each table scan with a profiling
// proxy.
class ProfilingRewriter : public googlesql::ResolvedASTDeepCopyVisitor {
 public:
  explicit ProfilingRewriter(
      absl::flat_hash_map<const googlesql::ResolvedTableScan*,
                          std::unique_ptr<ScanProfiler::ProfilingTable>>*
          tables)
      : tables_(tables) {}

 private:
  absl::Status VisitResolvedTableScan(
      const googlesql::ResolvedTableScan* node) override {
    GOOGLESQL_RETURN_IF_ERROR(CopyVisitResolvedTableScan(node));
    auto table = std::make_unique<ScanProfiler::ProfilingTable>(node->table());
    GetUnownedTopOfStack<googlesql::ResolvedTableScan>()->set_table(
        table.get());
    (*tables_)[node] = std::move(table);
    return absl::OkStatus();
  }

  absl::flat_hash_map<const googlesql::ResolvedTableScan*,
                      std::unique_ptr<ScanProfiler::ProfilingTable>>* tables_;
};

}  // namespace

ScanProfiler::ScanProfiler() = default;

ScanProfiler::~ScanProfiler() = default;

absl::StatusOr<std::unique_ptr<const googlesql::ResolvedStatement>>
ScanProfiler::Instrument(const googlesql::ResolvedStatement& statement) {
  ProfilingRewriter rewriter(&tables_);
  GOOGLESQL_RETURN_IF_ERROR(statement.Accept(&rewriter));
  return rewriter.ConsumeRootNode<googlesql::ResolvedStatement>();
}

absl::flat_hash_map<const googlesql::ResolvedTableScan*, ScanProfile>
ScanProfiler::ScanProfiles() const {
  absl::flat_hash_map<const googlesql::ResolvedTableScan*, ScanProfile>
      profiles;
  for (const auto& [scan, table] : tables_) {
    profiles[scan] = table->profile();
  }
  return profiles;
}

}  // namespace backend
}  // namespace emulator
}  // namespace spanner
}  // namespace google

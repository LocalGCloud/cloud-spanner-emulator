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

#include "backend/query/spanner_sys_catalog.h"

#include <algorithm>
#include <memory>
#include <string>
#include <tuple>
#include <vector>

#include "googlesql/common/simple_evaluator_table_iterator.h"
#include "googlesql/public/evaluator_table_iterator.h"
#include "googlesql/public/simple_catalog.h"
#include "googlesql/public/value.h"
#include "absl/base/no_destructor.h"
#include "absl/container/flat_hash_map.h"
#include "absl/container/flat_hash_set.h"
#include "absl/log/check.h"
#include "absl/status/status.h"
#include "absl/status/statusor.h"
#include "absl/strings/str_cat.h"
#include "absl/strings/string_view.h"
#include "absl/time/clock.h"
#include "absl/types/span.h"
#include "backend/query/info_schema_columns_metadata_values.h"
#include "backend/query/tables_from_metadata.h"
#include "backend/schema/catalog/schema.h"
#include "backend/stats/spanner_sys_types.h"
#include "backend/stats/system_stats_collector.h"
#include "common/config.h"
#include "common/constants.h"
#include "googlesql/base/clock.h"

namespace google {
namespace spanner {
namespace emulator {
namespace backend {

namespace {

using ::googlesql::values::Bool;
using ::googlesql::values::Date;
using ::googlesql::values::Int64;

static constexpr char kSupportedOptimizerVersions[] =
    "SUPPORTED_OPTIMIZER_VERSIONS";

// Tables that exist in production but have nothing to report for the
// emulator's single split and lack of an optimizer advisor.
static const absl::NoDestructor<absl::flat_hash_set<std::string>>
    kAlwaysEmptyTables{{
        "QUERY_RECOMMENDATIONS",
        "SCHEMA_RECOMMENDATIONS",
        "SPLIT_HOTNESS_STATS_TOP_MINUTE",
        "SPLIT_STATS_TOP_MINUTE",
    }};

bool IsSupportedTable(absl::string_view table_name) {
  return table_name == kSupportedOptimizerVersions ||
         kAlwaysEmptyTables->contains(table_name) ||
         SystemStatsCollector::ServesTable(table_name);
}

// Returns the metadata of the columns of the supported tables, ordered by
// table name and ordinal position.
const std::vector<ColumnsMetaEntry>& SupportedColumnsMetadata() {
  static const absl::NoDestructor<std::vector<ColumnsMetaEntry>> kColumns([] {
    std::vector<SpannerSysColumnsMetaEntry> metadata =
        SpannerSysColumnsMetadata();
    std::stable_sort(metadata.begin(), metadata.end(),
                     [](const SpannerSysColumnsMetaEntry& a,
                        const SpannerSysColumnsMetaEntry& b) {
                       return std::make_tuple(absl::string_view(a.table_name),
                                              a.primary_key_ordinal) <
                              std::make_tuple(absl::string_view(b.table_name),
                                              b.primary_key_ordinal);
                     });
    std::vector<ColumnsMetaEntry> columns;
    for (const SpannerSysColumnsMetaEntry& column : metadata) {
      if (IsSupportedTable(column.table_name)) {
        columns.push_back({.table_name = column.table_name,
                           .column_name = column.column_name,
                           .is_nullable = column.is_nullable,
                           .spanner_type = column.spanner_type});
      }
    }
    return columns;
  }());
  return *kColumns;
}

const absl::flat_hash_set<std::string>& SupportedTables() {
  static const absl::NoDestructor<absl::flat_hash_set<std::string>> kTables(
      [] {
        absl::flat_hash_set<std::string> tables;
        for (const ColumnsMetaEntry& column : SupportedColumnsMetadata()) {
          tables.insert(column.table_name);
        }
        return tables;
      }());
  return *kTables;
}

// Maps the column types of the SPANNER_SYS metadata to GoogleSQL types. Only
// GoogleSQL databases have the ARRAY<STRUCT<...>> columns.
const absl::flat_hash_map<std::string, const googlesql::Type*>&
SpannerSysTypes(database_api::DatabaseDialect dialect) {
  static const absl::NoDestructor<
      absl::flat_hash_map<std::string, const googlesql::Type*>>
      kGoogleSqlTypes([] {
        absl::flat_hash_map<std::string, const googlesql::Type*> types =
            *kSpannerTypeToGSQLType;
        types["ARRAY<STRUCT<COUNT INT64, MEAN FLOAT64, "
              "SUM_OF_SQUARED_DEVIATION FLOAT64, NUM_FINITE_BUCKETS INT64, "
              "GROWTH_FACTOR FLOAT64, SCALE FLOAT64, BUCKET_COUNTS "
              "ARRAY<INT64>>>"] = LatencyDistributionType();
        types["ARRAY<STRUCT<TABLE_NAME STRING(MAX), INSERT_OR_UPDATE_COUNT "
              "INT64, INSERT_OR_UPDATE_BYTES INT64>>"] =
            OperationsByTableType();
        types["ARRAY<STRUCT<COLUMN STRING(MAX), LOCK_MODE STRING(MAX), "
              "TRANSACTION_TAG STRING(MAX)>>"] = SampleLockRequestsType();
        return types;
      }());
  return dialect == database_api::DatabaseDialect::POSTGRESQL
             ? *kSpannerTypeToGSQLType
             : *kGoogleSqlTypes;
}

}  // namespace

SpannerSysCatalog::SpannerSysCatalog(
    const Schema* schema, const SystemStatsCollector* stats_collector)
    : googlesql::SimpleCatalog(kName) {
  const database_api::DatabaseDialect dialect =
      schema == nullptr ? database_api::DatabaseDialect::GOOGLE_STANDARD_SQL
                        : schema->dialect();
  tables_by_name_ = AddTablesFromMetadata(
      SupportedColumnsMetadata(), SpannerSysTypes(dialect), SupportedTables());
  for (auto& [name, table] : tables_by_name_) {
    ABSL_CHECK_OK(table->set_full_name(absl::StrCat(kName, ".", name)));  // Crash OK
    AddTable(table.get());
    if (SystemStatsCollector::ServesTable(name)) {
      ServeStatistics(table.get(), stats_collector, schema);
    }
  }

  FillOptimizerVersionsTable();
}

void SpannerSysCatalog::FillOptimizerVersionsTable() {
  auto table = tables_by_name_.at(kSupportedOptimizerVersions).get();
  std::vector<std::vector<googlesql::Value>> rows;

  rows.push_back({// version
                  Int64(kDefaultOptimizerVersion),
                  // release_date: 2023-09-19
                  Date(19619),
                  // is_default
                  Bool(true)});

  table->SetContents(rows);
}

void SpannerSysCatalog::ServeStatistics(
    googlesql::SimpleTable* table,
    const SystemStatsCollector* stats_collector, const Schema* schema) {
  table->SetEvaluatorTableIteratorFactory(
      [table, stats_collector, schema](absl::Span<const int> column_idxs)
          -> absl::StatusOr<
              std::unique_ptr<googlesql::EvaluatorTableIterator>> {
        std::vector<SpannerSysRow> rows;
        if (stats_collector != nullptr) {
          rows = stats_collector->Snapshot(
              table->Name(), absl::Now(),
              config::spanner_sys_expose_open_interval(), schema);
        }
        std::vector<const googlesql::Column*> columns;
        std::vector<std::shared_ptr<const std::vector<googlesql::Value>>>
            column_values;
        for (int column_idx : column_idxs) {
          const googlesql::Column* column = table->GetColumn(column_idx);
          auto values = std::make_shared<std::vector<googlesql::Value>>();
          values->reserve(rows.size());
          for (const SpannerSysRow& row : rows) {
            auto it = row.find(column->Name());
            values->push_back(it == row.end()
                                  ? googlesql::Value::Null(column->GetType())
                                  : it->second);
          }
          columns.push_back(column);
          column_values.push_back(std::move(values));
        }
        return googlesql::SimpleEvaluatorTableIterator::Create(
            columns, column_values, rows.size(),
            /*end_status=*/absl::OkStatus(), /*filter_column_idxs=*/{},
            /*cancel_cb=*/[]() {}, /*set_deadline_cb=*/[](absl::Time) {},
            googlesql_base::Clock::RealClock());
      });
}

}  // namespace backend
}  // namespace emulator
}  // namespace spanner
}  // namespace google

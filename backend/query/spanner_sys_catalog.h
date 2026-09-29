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

#ifndef THIRD_PARTY_CLOUD_SPANNER_EMULATOR_BACKEND_QUERY_SPANNER_SYS_CATALOG_H_
#define THIRD_PARTY_CLOUD_SPANNER_EMULATOR_BACKEND_QUERY_SPANNER_SYS_CATALOG_H_

#include <memory>
#include <string>

#include "googlesql/public/simple_catalog.h"
#include "absl/container/flat_hash_map.h"
#include "backend/schema/catalog/schema.h"
#include "backend/stats/system_stats_collector.h"

namespace google {
namespace spanner {
namespace emulator {
namespace backend {

// The SPANNER_SYS schema. Columns are in the order of production's
// INFORMATION_SCHEMA.COLUMNS. PostgreSQL databases see the *_JSON_STRING
// columns but not the ARRAY<STRUCT<...>> ones.
class SpannerSysCatalog : public googlesql::SimpleCatalog {
 public:
  static constexpr char kName[] = "SPANNER_SYS";

  // The statistics tables read their rows from `stats_collector` when they
  // are scanned, and are empty if it is null. `schema` determines the dialect
  // and names the tables and columns of lock statistics.
  explicit SpannerSysCatalog(
      const Schema* schema = nullptr,
      const SystemStatsCollector* stats_collector = nullptr);

 private:
  // Explicitly storing the tables because we are using SimpleCatalog::AddTable
  // which expects that the caller maintains the ownership of the added objects.
  absl::flat_hash_map<std::string, std::unique_ptr<googlesql::SimpleTable>>
      tables_by_name_;

  void FillOptimizerVersionsTable();

  // Serves the rows of `table` from `stats_collector` at scan time.
  void ServeStatistics(googlesql::SimpleTable* table,
                       const SystemStatsCollector* stats_collector,
                       const Schema* schema);
};

}  // namespace backend
}  // namespace emulator
}  // namespace spanner
}  // namespace google

#endif  // THIRD_PARTY_CLOUD_SPANNER_EMULATOR_BACKEND_QUERY_SPANNER_SYS_CATALOG_H_

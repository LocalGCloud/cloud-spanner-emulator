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

#ifndef THIRD_PARTY_CLOUD_SPANNER_EMULATOR_BACKEND_QUERY_PG_CATALOG_H_
#define THIRD_PARTY_CLOUD_SPANNER_EMULATOR_BACKEND_QUERY_PG_CATALOG_H_

#include <memory>
#include <string>

#include "googlesql/public/simple_catalog.h"
#include "absl/container/flat_hash_map.h"
#include "backend/query/info_schema_columns_metadata_values.h"
#include "backend/schema/catalog/access_policy.h"
#include "backend/schema/catalog/schema.h"
#include "third_party/spanner_pg/catalog/engine_system_catalog.h"

namespace postgres_translator {

// A sub-catalog used for serving queries to the PG catalog by the Cloud Spanner
// Emulator.
class PGCatalog : public googlesql::SimpleCatalog {
 public:
  static constexpr char kName[] = "pg_catalog";

  // `access` is the fine-grained access control policy of the database role
  // that reads the tables, or nullptr if the reader has no database role. A
  // role sees the rows about the schema objects that it may see in
  // INFORMATION_SCHEMA; members of spanner_info_reader see all rows.
  PGCatalog(
      const EnumerableCatalog* root_catalog,
      const google::spanner::emulator::backend::Schema* default_schema,
      const google::spanner::emulator::backend::AccessPolicy* access =
          nullptr);

 private:
  // Row filtering: whether the database role may see an object. All objects
  // are visible without a policy. `index` is nullptr for the primary key of
  // `table`.
  bool CanSeeTable(const google::spanner::emulator::backend::Table* table) const;
  bool CanSeeColumn(
      const google::spanner::emulator::backend::Column* column) const;
  bool CanSeeIndex(const google::spanner::emulator::backend::Table* table,
                   const google::spanner::emulator::backend::Index* index,
                   bool table_delete_suffices) const;
  bool CanSeeView(const google::spanner::emulator::backend::View* view) const;
  bool CanSeeSequence(
      const google::spanner::emulator::backend::Sequence* sequence) const;
  bool CanSeeRoutine(
      const google::spanner::emulator::backend::SchemaNode* routine) const;

  const EnumerableCatalog* root_catalog_;
  const google::spanner::emulator::backend::Schema* default_schema_;
  // The policy that filters rows, or nullptr if all rows are visible.
  const google::spanner::emulator::backend::AccessPolicy* access_;

  const postgres_translator::EngineSystemCatalog* system_catalog_ =
      postgres_translator::EngineSystemCatalog::GetEngineSystemCatalog();

  // Explicitly storing the tables because we are using SimpleCatalog::AddTable
  // which expects that the caller maintains the ownership of the added objects.
  absl::flat_hash_map<std::string, std::unique_ptr<googlesql::SimpleTable>>
      tables_by_name_;

  std::map<std::string,
           std::vector<google::spanner::emulator::backend::ColumnsMetaEntry>>
      info_schema_table_name_to_column_metadata_;
  std::map<std::string,
           std::vector<google::spanner::emulator::backend::ColumnsMetaEntry>>
      pg_catalog_table_name_to_column_metadata_;
  std::map<std::string,
           std::vector<
               google::spanner::emulator::backend::SpannerSysColumnsMetaEntry>>
      spanner_sys_table_name_to_column_metadata_;

  void FillPGAmTable();
  void FillPGAttrdefTable();
  void FillPGAttributeTable();
  void FillPGClassTable();
  void FillPGCollationTable();
  void FillPGConstraintTable();
  void FillPGIndexTable();
  void FillPGIndexesTable();
  void FillPGNamespaceTable();
  void FillPGProcTable();
  void FillPGSequenceTable();
  void FillPGSequencesTable();
  void FillPGSettingsTable();
  void FillPGTablesTable();
  void FillPGTypeTable();
  void FillPGViewsTable();
};

}  // namespace postgres_translator

#endif  // THIRD_PARTY_CLOUD_SPANNER_EMULATOR_BACKEND_QUERY_PG_CATALOG_H_

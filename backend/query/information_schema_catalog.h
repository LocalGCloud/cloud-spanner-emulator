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

#ifndef THIRD_PARTY_CLOUD_SPANNER_EMULATOR_BACKEND_QUERY_INFORMATION_SCHEMA_CATALOG_H_
#define THIRD_PARTY_CLOUD_SPANNER_EMULATOR_BACKEND_QUERY_INFORMATION_SCHEMA_CATALOG_H_

#include <memory>
#include <string>
#include <utility>
#include <vector>

#include "google/spanner/admin/database/v1/common.pb.h"
#include "googlesql/public/simple_catalog.h"
#include "googlesql/public/value.h"
#include "absl/container/flat_hash_map.h"
#include "absl/strings/string_view.h"
#include "absl/types/span.h"
#include "backend/query/change_stream/queryable_change_stream_tvf.h"
#include "backend/query/spanner_sys_catalog.h"
#include "backend/schema/catalog/access_policy.h"
#include "backend/schema/catalog/model.h"
#include "backend/schema/catalog/schema.h"
#include "third_party/spanner_pg/ddl/spangres_schema_printer.h"

namespace google {
namespace spanner {
namespace emulator {
namespace backend {

struct ColumnsMetaEntry;
struct IndexColumnsMetaEntry;

// InformationSchemaCatalog provides the INFORMATION_SCHEMA tables.
//
// GoogleSQL reference implementation accesses table data via the catalog
// objects themselves. Hence, this class provides both the catalog and the data
// backing the information schema tables.
//
// Cloud Spanner's information schema is documented at:
//   https://cloud.google.com/spanner/docs/information-schema
//
// The emulator only exposes the default (user) schema and INFORMATION_SCHEMA.
// In production, SPANNER_SYS schemas are also exposed which are not available
// in the emulator.
//
// A database role that is not a member of spanner_info_reader sees only the
// rows about the objects and privileges it has access to, as documented for
// each table.
//
// This class is tested via tests/conformance/cases/information_schema.cc
class InformationSchemaCatalog : public googlesql::SimpleCatalog {
 public:
  static constexpr char kName[] = "INFORMATION_SCHEMA";
  static constexpr char kPGName[] = "PG_INFORMATION_SCHEMA";

  // `access` is the fine-grained access control policy of the database role
  // that reads the tables, or nullptr if the reader has no database role.
  explicit InformationSchemaCatalog(
      const std::string& catalog_name, const Schema* default_schema,
      const SpannerSysCatalog* spanner_sys_catalog,
      const AccessPolicy* access = nullptr);

 private:
  // A row of a table that lists granted privileges.
  struct PrivilegeRow {
    // The values by column name. Columns missing from the table of a dialect
    // are ignored, and columns without a value are NULL.
    absl::flat_hash_map<std::string, googlesql::Value> values;

    // The role that holds the privilege.
    std::string grantee;

    // Whether the database role may see the object of the privilege.
    bool object_visible = true;
  };

  // The rows of a privilege table that a database role sees.
  enum class PrivilegeFilter {
    // Privileges on objects that the role may see.
    kVisibleObject,
    // Privileges granted to the effective roles of the role.
    kEffectiveGrantee,
    // Privileges granted to the effective roles of the role other than public.
    kEffectiveGranteeExceptPublic,
  };

  const Schema* default_schema_;
  const SpannerSysCatalog* spanner_sys_catalog_;
  // The policy that filters rows, or nullptr if all rows are visible.
  const AccessPolicy* access_;
  const ::google::spanner::admin::database::v1::DatabaseDialect dialect_;
  absl::flat_hash_map<std::string, std::unique_ptr<googlesql::SimpleTable>>
      tables_by_name_;
  std::unique_ptr<postgres_translator::spangres::SpangresSchemaPrinter>
      pg_schema_printer_;

  inline std::string GetNameForDialect(absl::string_view name);
  std::pair<googlesql::Value, googlesql::Value> GetPGDataTypeAndSpannerType(
      const googlesql::Type* type, std::optional<int64_t> length);

  googlesql::Value GetSpannerType(const Column* column);
  googlesql::Value GetSpannerType(const googlesql::Type* type,
                                  std::optional<int64_t> length);
  googlesql::Value PGDataType(const googlesql::Type* type);
  inline googlesql::Value DialectDefaultSchema();
  inline googlesql::Value DialectBoolValue(bool value);
  googlesql::Value DialectColumnOrdering(const KeyColumn* column);
  inline googlesql::Value DialectTableCatalog();
  inline std::pair<std::string, std::string>
  GetSchemaAndNameForInformationSchema(std::string table_name);

  // Row filtering: whether the database role may see an object. All objects
  // are visible without a policy.
  bool CanSeeTable(const Table* table) const;
  bool CanSeeTablePrivileges(const Table* table) const;
  bool CanSeeColumn(const Column* column) const;
  bool CanSeeView(const View* view) const;
  // `index` is nullptr for the primary key of `table`.
  bool CanSeeIndex(const Table* table, const Index* index,
                   bool table_delete_suffices) const;
  bool CanSeeSequence(const Sequence* sequence) const;
  bool CanSeeChangeStream(const ChangeStream* change_stream) const;
  // `routine` is a user-defined function or the change stream of a read
  // function.
  bool CanSeeRoutine(const SchemaNode* routine) const;
  bool CanSeeModel(const Model* model) const;
  bool CanSeePropertyGraph(const PropertyGraph* property_graph) const;
  bool CanSeeRole(absl::string_view role) const;
  bool CanSeeGrantee(absl::string_view grantee, bool include_public) const;

  // Returns the read function of a change stream, which has the signature that
  // queries use.
  std::unique_ptr<QueryableChangeStreamTvf> CreateReadFunction(
      const ChangeStream* change_stream);

  void AddProtoBundleToSchemataTable();
  googlesql::Value GetProtoBundleValue();

  void FillSchemataTable();
  void FillSpannerStatisticsTable();
  void FillDatabaseOptionsTable();
  void FillColumnOptionsTable();

  void FillTablesTable();
  void FillColumnsTable();
  void FillColumnColumnUsageTable();
  void FillIndexesTable();
  void FillIndexColumnsTable();
  void FillTableConstraintsTable();
  void FillCheckConstraintsTable();
  void FillConstraintTableUsageTable();
  void FillReferentialConstraintsTable();
  void FillKeyColumnUsageTable();
  void FillConstraintColumnUsageTable();
  void FillViewsTable();

  void FillChangeStreamsTable();
  void FillChangeStreamTablesTable();
  void FillChangeStreamOptionsTable();
  void FillChangeStreamColumnsTable();

  void FillSequencesTable();
  void FillSequenceOptionsTable();

  void FillModelsTable();
  void FillModelOptionsTable();
  void FillModelColumnsTable();
  void FillModelColumnOptionsTable();

  void FillModelColumnsTable(const Model& model,
                             const Model::ModelColumn& column,
                             absl::string_view column_kind,
                             int64_t ordinal_position,
                             std::vector<std::vector<googlesql::Value>>* rows);
  void FillModelColumnOptionsTable(
      const Model& model, const Model::ModelColumn& column,
      absl::string_view column_kind,
      std::vector<std::vector<googlesql::Value>>* rows);
  googlesql::Value ParseLocalityGroupOptions(ddl::SetOption option);
  void FillLocalityGroupOptionsTable();

  void FillPlacementsTable();
  void FillPlacementOptionsTable();

  void FillPropertyGraphsTable();

  void FillRolesTable();
  void FillRoleGranteesTable();
  void FillPrivilegeTable(absl::string_view table_name,
                          absl::Span<const PrivilegeRow> privilege_rows,
                          PrivilegeFilter filter);
  void FillTablePrivilegesTables();
  void FillColumnPrivilegesTables();
  void FillChangeStreamPrivilegesTables();
  void FillRoutinePrivilegesTables();
  void FillModelPrivilegesTables();
  void FillRoutinesAndParametersTables();
  void FillTableSynonymsTable();
  void FillInformationSchemaCatalogNameTable();
};

}  // namespace backend
}  // namespace emulator
}  // namespace spanner
}  // namespace google

#endif  // THIRD_PARTY_CLOUD_SPANNER_EMULATOR_BACKEND_QUERY_INFORMATION_SCHEMA_CATALOG_H_

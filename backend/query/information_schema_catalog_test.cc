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

#include "backend/query/information_schema_catalog.h"

#include <map>
#include <memory>
#include <string>
#include <vector>

#include "gmock/gmock.h"
#include "gtest/gtest.h"
#include "googlesql/base/testing/status_matchers.h"
#include "googlesql/public/evaluator_table_iterator.h"
#include "googlesql/public/types/type_factory.h"
#include "googlesql/public/value.h"
#include "absl/log/check.h"
#include "absl/status/statusor.h"
#include "absl/strings/match.h"
#include "absl/strings/str_cat.h"
#include "backend/query/info_schema_columns_metadata_values.h"
#include "backend/query/spanner_sys_catalog.h"
#include "backend/schema/catalog/access_policy.h"
#include "tests/common/schema_constructor.h"

namespace google::spanner::emulator::backend {

namespace {

using ::testing::Contains;
using ::testing::Each;
using ::testing::ElementsAre;
using ::testing::IsEmpty;
using ::testing::Not;
using ::testing::StartsWith;
using ::testing::UnorderedElementsAre;

using Row = std::vector<std::string>;

// Formats a value the way the expectations below spell it.
std::string ValueToString(const googlesql::Value& value) {
  if (value.is_null()) {
    return "NULL";
  }
  if (value.type()->IsString()) {
    return value.string_value();
  }
  if (value.type()->IsBool()) {
    return value.bool_value() ? "true" : "false";
  }
  return value.DebugString();
}

// Returns every row of the named information schema table.
std::vector<Row> ReadTable(InformationSchemaCatalog& catalog,
                           const std::string& table_name) {
  const googlesql::Table* table = nullptr;
  ABSL_CHECK_OK(catalog.FindTable({table_name}, &table));  // crash ok
  std::vector<int> column_idxs;
  for (int i = 0; i < table->NumColumns(); ++i) {
    column_idxs.push_back(i);
  }
  auto iterator = table->CreateEvaluatorTableIterator(column_idxs);
  ABSL_CHECK_OK(iterator.status());  // crash ok
  std::vector<Row> rows;
  while ((*iterator)->NextRow()) {
    Row row;
    for (int i = 0; i < (*iterator)->NumColumns(); ++i) {
      row.push_back(ValueToString((*iterator)->GetValue(i)));
    }
    rows.push_back(std::move(row));
  }
  ABSL_CHECK_OK((*iterator)->Status());  // crash ok
  return rows;
}

// Returns the index of the named column of an information schema table.
int ColumnIndex(InformationSchemaCatalog& catalog,
                const std::string& table_name,
                const std::string& column_name) {
  const googlesql::Table* table = nullptr;
  ABSL_CHECK_OK(catalog.FindTable({table_name}, &table));  // crash ok
  for (int i = 0; i < table->NumColumns(); ++i) {
    if (absl::EqualsIgnoreCase(table->GetColumn(i)->Name(), column_name)) {
      return i;
    }
  }
  ADD_FAILURE() << "Missing column " << table_name << "." << column_name;
  return 0;
}

// Returns the named columns of every row of an information schema table.
std::vector<Row> ReadColumns(InformationSchemaCatalog& catalog,
                             const std::string& table_name,
                             const std::vector<std::string>& column_names) {
  std::vector<int> column_indexes;
  for (const std::string& column_name : column_names) {
    column_indexes.push_back(ColumnIndex(catalog, table_name, column_name));
  }
  std::vector<Row> rows;
  for (const Row& row : ReadTable(catalog, table_name)) {
    Row columns;
    for (int index : column_indexes) {
      columns.push_back(row[index]);
    }
    rows.push_back(std::move(columns));
  }
  return rows;
}

// Returns the rows of `rows` whose first value is `value`.
std::vector<Row> RowsStartingWith(const std::vector<Row>& rows,
                                  const std::string& value) {
  std::vector<Row> matching;
  for (const Row& row : rows) {
    if (row[0] == value) {
      matching.push_back(row);
    }
  }
  return matching;
}

// Returns the TABLE_CONSTRAINTS rows whose type is PLACEMENT KEY.
std::vector<Row> PlacementKeyConstraints(InformationSchemaCatalog& catalog,
                                         const std::string& table_name) {
  int type_index = ColumnIndex(catalog, table_name, "CONSTRAINT_TYPE");
  std::vector<Row> rows;
  for (const Row& row : ReadTable(catalog, table_name)) {
    if (row[type_index] == "PLACEMENT KEY") {
      rows.push_back(row);
    }
  }
  return rows;
}

std::unique_ptr<const Schema> CreatePlacementSchema(
    googlesql::TypeFactory* type_factory) {
  std::vector<std::string> statements = {
      R"(CREATE PLACEMENT eu OPTIONS (instance_partition = 'eu-partition',
                                      default_leader = 'europe-west1'))",
      R"(CREATE PLACEMENT asia
           OPTIONS (instance_partition = 'asia-partition'))",
      R"(CREATE TABLE Singers (
           SingerId INT64 NOT NULL,
           Location STRING(MAX) NOT NULL PLACEMENT KEY,
         ) PRIMARY KEY (SingerId))",
      R"(CREATE TABLE Plain (
           Id INT64 NOT NULL,
         ) PRIMARY KEY (Id))",
  };
  auto schema = test::CreateSchemaFromDDL(statements, type_factory);
  ABSL_CHECK_OK(schema.status());  // crash ok
  return std::move(*schema);
}

TEST(InformationSchemaCatalogTest, ColumnsMetadataCount) {
  Schema schema;
  SpannerSysCatalog spanner_sys_catalog;
  InformationSchemaCatalog catalog(InformationSchemaCatalog::kName, &schema,
                                   &spanner_sys_catalog);
  EXPECT_EQ(ColumnsMetadata().size(), 297);
}

TEST(InformationSchemaCatalogTest, IndexColumnsMetadataCount) {
  Schema schema;
  SpannerSysCatalog spanner_sys_catalog;
  InformationSchemaCatalog catalog(InformationSchemaCatalog::kName, &schema,
                                   &spanner_sys_catalog);
  EXPECT_EQ(IndexColumnsMetadata().size(), 184);
}

TEST(InformationSchemaCatalogTest, SpannerSysColumnsMetadataCount) {
  Schema schema;
  SpannerSysCatalog spanner_sys_catalog;
  InformationSchemaCatalog catalog(InformationSchemaCatalog::kName, &schema,
                                   &spanner_sys_catalog);
  EXPECT_EQ(SpannerSysColumnsMetadata().size(), 489);
}

TEST(InformationSchemaCatalogTest, PGColumnsMetadataCount) {
  Schema schema;
  SpannerSysCatalog spanner_sys_catalog;
  InformationSchemaCatalog catalog(InformationSchemaCatalog::kPGName, &schema,
                                   &spanner_sys_catalog);
  EXPECT_EQ(PGColumnsMetadata().size(), 430);
}

TEST(InformationSchemaCatalogTest, PGIndexColumnsMetadataCount) {
  Schema schema;
  SpannerSysCatalog spanner_sys_catalog;
  InformationSchemaCatalog catalog(InformationSchemaCatalog::kPGName, &schema,
                                   &spanner_sys_catalog);
  EXPECT_EQ(PGIndexColumnsMetadata().size(), 1);
}

TEST(InformationSchemaCatalogTest, PGSpannerSysColumnsMetadataCount) {
  Schema schema;
  SpannerSysCatalog spanner_sys_catalog;
  InformationSchemaCatalog catalog(InformationSchemaCatalog::kPGName, &schema,
                                   &spanner_sys_catalog);
  EXPECT_EQ(SpannerSysColumnsMetadata().size(), 489);
}

TEST(InformationSchemaCatalogTest, DefaultPlacementAlwaysListed) {
  Schema schema;
  SpannerSysCatalog spanner_sys_catalog;
  InformationSchemaCatalog catalog(InformationSchemaCatalog::kName, &schema,
                                   &spanner_sys_catalog);
  EXPECT_THAT(ReadTable(catalog, "PLACEMENTS"),
              ElementsAre(Row{"default", "true"}));
  EXPECT_THAT(ReadTable(catalog, "PLACEMENT_OPTIONS"), IsEmpty());
  EXPECT_THAT(PlacementKeyConstraints(catalog, "TABLE_CONSTRAINTS"),
              IsEmpty());
}

TEST(InformationSchemaCatalogTest, PlacementTables) {
  googlesql::TypeFactory type_factory;
  std::unique_ptr<const Schema> schema = CreatePlacementSchema(&type_factory);
  SpannerSysCatalog spanner_sys_catalog;
  InformationSchemaCatalog catalog(InformationSchemaCatalog::kName,
                                   schema.get(), &spanner_sys_catalog);

  EXPECT_THAT(ReadTable(catalog, "PLACEMENTS"),
              ElementsAre(Row{"default", "true"}, Row{"eu", "false"},
                          Row{"asia", "false"}));
  EXPECT_THAT(
      ReadTable(catalog, "PLACEMENT_OPTIONS"),
      UnorderedElementsAre(
          Row{"eu", "instance_partition", "STRING(MAX)", "eu-partition"},
          Row{"eu", "default_leader", "STRING(MAX)", "europe-west1"},
          Row{"asia", "instance_partition", "STRING(MAX)", "asia-partition"}));
  EXPECT_THAT(PlacementKeyConstraints(catalog, "TABLE_CONSTRAINTS"),
              ElementsAre(Row{"", "", "PLACEMENT_KEY_Singers", "", "",
                              "Singers", "PLACEMENT KEY", "NO", "NO", "YES"}));
}

TEST(InformationSchemaCatalogTest, PGPlacementTables) {
  googlesql::TypeFactory type_factory;
  std::unique_ptr<const Schema> schema = CreatePlacementSchema(&type_factory);
  SpannerSysCatalog spanner_sys_catalog;
  InformationSchemaCatalog catalog(InformationSchemaCatalog::kPGName,
                                   schema.get(), &spanner_sys_catalog);

  EXPECT_THAT(ReadTable(catalog, "placements"),
              ElementsAre(Row{"default", "YES"}, Row{"eu", "NO"},
                          Row{"asia", "NO"}));
  EXPECT_THAT(ReadTable(catalog, "placement_options"),
              UnorderedElementsAre(Row{"eu", "instance_partition",
                                       "character varying", "eu-partition"},
                                   Row{"eu", "default_leader",
                                       "character varying", "europe-west1"},
                                   Row{"asia", "instance_partition",
                                       "character varying", "asia-partition"}));
  EXPECT_THAT(PlacementKeyConstraints(catalog, "table_constraints"),
              ElementsAre(Row{"", "public", "PLACEMENT_KEY_Singers", "",
                              "public", "Singers", "PLACEMENT KEY", "NO", "NO",
                              "YES"}));
}

TEST(InformationSchemaCatalogTest, DatabaseOptionsIncludeRegionalSettings) {
  googlesql::TypeFactory type_factory;
  for (const auto dialect : {database_api::GOOGLE_STANDARD_SQL,
                             database_api::POSTGRESQL}) {
    const bool pg = dialect == database_api::POSTGRESQL;
    const std::vector<std::string> statements =
        pg ? std::vector<std::string>{
                 "ALTER DATABASE db SET spanner.default_leader = 'us-east1'",
                 "ALTER DATABASE db SET spanner.witness_location = 'us-west1'",
                 "ALTER DATABASE db SET spanner.read_lease_regions = 'us-east1,us-west1'",
             }
           : std::vector<std::string>{
                 "ALTER DATABASE db SET OPTIONS (default_leader = 'us-east1')",
                 "ALTER DATABASE db SET OPTIONS (witness_location = 'us-west1')",
                 "ALTER DATABASE db SET OPTIONS "
                 "(read_lease_regions = 'us-east1,us-west1')",
             };
    auto schema =
        test::CreateSchemaFromDDL(statements, &type_factory, "", dialect, "db");
    ASSERT_TRUE(schema.ok()) << schema.status();
    SpannerSysCatalog spanner_sys_catalog;
    InformationSchemaCatalog catalog(
        pg ? InformationSchemaCatalog::kPGName
           : InformationSchemaCatalog::kName,
        schema->get(), &spanner_sys_catalog);
    const std::string table = pg ? "database_options" : "DATABASE_OPTIONS";
    const int name_col = ColumnIndex(catalog, table, "OPTION_NAME");
    const int type_col = ColumnIndex(catalog, table, "OPTION_TYPE");
    const int value_col = ColumnIndex(catalog, table, "OPTION_VALUE");
    std::map<std::string, Row> options;
    for (const Row& row : ReadTable(catalog, table)) {
      options.emplace(row[name_col], row);
    }
    for (const auto& [name, value] :
         std::map<std::string, std::string>{{"default_leader", "us-east1"},
                                            {"witness_location", "us-west1"},
                                            {"read_lease_regions",
                                             "us-east1,us-west1"}}) {
      ASSERT_EQ(options.count(name), 1) << name;
      EXPECT_EQ(options.at(name)[type_col],
                pg ? "character varying" : "STRING");
      EXPECT_EQ(options.at(name)[value_col], value);
    }
  }
}

TEST(InformationSchemaCatalogTest, DatabaseOptionsOmitResetReadLeaseRegions) {
  googlesql::TypeFactory type_factory;
  auto schema = test::CreateSchemaFromDDL(
      {"ALTER DATABASE db SET OPTIONS (read_lease_regions = 'us-east1')",
       "ALTER DATABASE db SET OPTIONS (read_lease_regions = NULL)"},
      &type_factory);
  ASSERT_TRUE(schema.ok()) << schema.status();
  SpannerSysCatalog spanner_sys_catalog;
  InformationSchemaCatalog catalog(InformationSchemaCatalog::kName,
                                   schema->get(), &spanner_sys_catalog);
  const int name_col = ColumnIndex(catalog, "DATABASE_OPTIONS", "OPTION_NAME");
  for (const Row& row : ReadTable(catalog, "DATABASE_OPTIONS")) {
    EXPECT_NE(row[name_col], "read_lease_regions");
  }
}

constexpr char kModelEndpoint[] =
    "//aiplatform.googleapis.com/projects/p/locations/l/endpoints/e";

std::unique_ptr<const Schema> CreateGrantsSchema(
    googlesql::TypeFactory* type_factory) {
  std::vector<std::string> statements = {
      "CREATE TABLE T (K INT64, A STRING(MAX), B STRING(MAX)) PRIMARY KEY (K)",
      "CREATE INDEX TByA ON T(A)",
      "CREATE INDEX TByB ON T(B)",
      "CREATE TABLE U (K INT64) PRIMARY KEY (K)",
      "ALTER TABLE U ADD SYNONYM U2",
      "CREATE TABLE W (K INT64) PRIMARY KEY (K)",
      "CREATE VIEW V SQL SECURITY INVOKER AS SELECT T.K FROM T",
      "CREATE CHANGE STREAM CS FOR T",
      "CREATE CHANGE STREAM CS2 FOR W",
      absl::StrCat("CREATE MODEL M INPUT (x INT64) OUTPUT (y INT64) ",
                   "REMOTE OPTIONS (endpoint = '", kModelEndpoint, "')"),
      absl::StrCat("CREATE MODEL M2 INPUT (x INT64) OUTPUT (y INT64) ",
                   "REMOTE OPTIONS (endpoint = '", kModelEndpoint, "')"),
      "CREATE ROLE parent",
      "CREATE ROLE reader",
      "CREATE ROLE other",
      "CREATE ROLE auditor",
      "GRANT ROLE parent TO ROLE reader",
      "GRANT ROLE spanner_info_reader TO ROLE auditor",
      "GRANT SELECT(K, A) ON TABLE T TO ROLE reader",
      "GRANT INSERT ON TABLE U TO ROLE parent",
      "GRANT DELETE ON TABLE U TO ROLE other",
      "GRANT SELECT ON TABLE W TO ROLE other",
      "GRANT SELECT ON VIEW V TO ROLE public",
      "GRANT SELECT ON CHANGE STREAM CS TO ROLE parent",
      "GRANT EXECUTE ON TABLE FUNCTION READ_CS TO ROLE public",
      "GRANT EXECUTE ON TABLE FUNCTION READ_CS2 TO ROLE other",
      "GRANT EXECUTE ON MODEL M TO ROLE reader",
  };
  auto schema = test::CreateSchemaFromDDL(statements, type_factory);
  ABSL_CHECK_OK(schema.status());  // crash ok
  return std::move(*schema);
}

TEST(InformationSchemaCatalogTest, RolesAndPrivileges) {
  googlesql::TypeFactory type_factory;
  std::unique_ptr<const Schema> schema = CreateGrantsSchema(&type_factory);
  SpannerSysCatalog spanner_sys_catalog;
  InformationSchemaCatalog catalog(InformationSchemaCatalog::kName,
                                   schema.get(), &spanner_sys_catalog);

  EXPECT_THAT(ReadTable(catalog, "ROLES"),
              UnorderedElementsAre(Row{"public", "true"},
                                   Row{"spanner_info_reader", "true"},
                                   Row{"spanner_sys_reader", "true"},
                                   Row{"parent", "false"},
                                   Row{"reader", "false"},
                                   Row{"other", "false"},
                                   Row{"auditor", "false"}));
  EXPECT_THAT(ReadTable(catalog, "ROLE_GRANTEES"),
              UnorderedElementsAre(Row{"parent", "reader"},
                                   Row{"spanner_info_reader", "auditor"}));
  EXPECT_THAT(ReadTable(catalog, "TABLE_PRIVILEGES"),
              UnorderedElementsAre(Row{"", "", "U", "INSERT", "parent"},
                                   Row{"", "", "U", "DELETE", "other"},
                                   Row{"", "", "W", "SELECT", "other"},
                                   Row{"", "", "V", "SELECT", "public"}));
  // Privileges on whole tables and views are listed for each column.
  EXPECT_THAT(ReadTable(catalog, "COLUMN_PRIVILEGES"),
              UnorderedElementsAre(Row{"", "", "T", "K", "SELECT", "reader"},
                                   Row{"", "", "T", "A", "SELECT", "reader"},
                                   Row{"", "", "U", "K", "INSERT", "parent"},
                                   Row{"", "", "W", "K", "SELECT", "other"},
                                   Row{"", "", "V", "K", "SELECT", "public"}));
  EXPECT_THAT(
      ReadTable(catalog, "ROLE_TABLE_GRANTS"),
      UnorderedElementsAre(
          Row{"NULL", "parent", "", "", "U", "INSERT", "NO"},
          Row{"NULL", "other", "", "", "U", "DELETE", "NO"},
          Row{"NULL", "other", "", "", "W", "SELECT", "NO"},
          Row{"NULL", "public", "", "", "V", "SELECT", "NO"}));
  EXPECT_THAT(
      ReadTable(catalog, "ROLE_COLUMN_GRANTS"),
      UnorderedElementsAre(
          Row{"NULL", "reader", "", "", "T", "K", "SELECT", "NO"},
          Row{"NULL", "reader", "", "", "T", "A", "SELECT", "NO"},
          Row{"NULL", "parent", "", "", "U", "K", "INSERT", "NO"},
          Row{"NULL", "other", "", "", "W", "K", "SELECT", "NO"},
          Row{"NULL", "public", "", "", "V", "K", "SELECT", "NO"}));
  EXPECT_THAT(ReadTable(catalog, "CHANGE_STREAM_PRIVILEGES"),
              ElementsAre(Row{"", "", "CS", "SELECT", "parent"}));
  EXPECT_THAT(ReadTable(catalog, "ROLE_CHANGE_STREAM_GRANTS"),
              ElementsAre(Row{"", "", "CS", "SELECT", "parent"}));
  EXPECT_THAT(
      ReadTable(catalog, "ROUTINE_PRIVILEGES"),
      UnorderedElementsAre(Row{"", "", "READ_CS", "EXECUTE", "public"},
                           Row{"", "", "READ_CS2", "EXECUTE", "other"}));
  EXPECT_THAT(
      ReadTable(catalog, "ROLE_ROUTINE_GRANTS"),
      UnorderedElementsAre(
          Row{"NULL", "public", "", "", "READ_CS", "EXECUTE", "NO"},
          Row{"NULL", "other", "", "", "READ_CS2", "EXECUTE", "NO"}));
  EXPECT_THAT(ReadTable(catalog, "MODEL_PRIVILEGES"),
              ElementsAre(Row{"", "", "M", "EXECUTE", "reader"}));
  EXPECT_THAT(ReadTable(catalog, "ROLE_MODEL_GRANTS"),
              ElementsAre(Row{"NULL", "reader", "", "", "M", "EXECUTE", "NO"}));
  EXPECT_THAT(ReadTable(catalog, "TABLE_SYNONYMS"),
              ElementsAre(Row{"", "", "U", "", "", "U2"}));
  EXPECT_THAT(ReadTable(catalog, "ROUTINE_OPTIONS"), IsEmpty());
}

TEST(InformationSchemaCatalogTest, ChangeStreamReadFunctions) {
  googlesql::TypeFactory type_factory;
  std::unique_ptr<const Schema> schema = CreateGrantsSchema(&type_factory);
  SpannerSysCatalog spanner_sys_catalog;
  InformationSchemaCatalog catalog(InformationSchemaCatalog::kName,
                                   schema.get(), &spanner_sys_catalog);

  EXPECT_THAT(
      ReadColumns(catalog, "ROUTINES",
                  {"SPECIFIC_NAME", "SPECIFIC_CATALOG", "SPECIFIC_SCHEMA",
                   "ROUTINE_NAME", "ROUTINE_TYPE", "ROUTINE_BODY",
                   "ROUTINE_DEFINITION", "SECURITY_TYPE"}),
      UnorderedElementsAre(
          Row{"READ_CS", "", "", "READ_CS", "FUNCTION", "EXTERNAL", "",
              "INVOKER"},
          Row{"READ_CS2", "", "", "READ_CS2", "FUNCTION", "EXTERNAL", "",
              "INVOKER"}));
  EXPECT_THAT(ReadColumns(catalog, "ROUTINES", {"DATA_TYPE"}),
              Each(ElementsAre(StartsWith(
                  "TABLE<ChangeRecord ARRAY<STRUCT<data_change_record "))));
  EXPECT_THAT(
      RowsStartingWith(ReadColumns(catalog, "PARAMETERS",
                                   {"SPECIFIC_NAME", "ORDINAL_POSITION",
                                    "PARAMETER_NAME", "DATA_TYPE",
                                    "PARAMETER_DEFAULT"}),
                       "READ_CS"),
      ElementsAre(
          Row{"READ_CS", "1", "start_timestamp", "TIMESTAMP", "NULL"},
          Row{"READ_CS", "2", "end_timestamp", "TIMESTAMP", "NULL"},
          Row{"READ_CS", "3", "partition_token", "STRING(MAX)", "NULL"},
          Row{"READ_CS", "4", "heartbeat_milliseconds", "INT64", "NULL"},
          Row{"READ_CS", "5", "read_options", "ARRAY<STRING(MAX)>", "NULL"}));
}

TEST(InformationSchemaCatalogTest, RowsAreFilteredForDatabaseRole) {
  googlesql::TypeFactory type_factory;
  std::unique_ptr<const Schema> schema = CreateGrantsSchema(&type_factory);
  absl::StatusOr<AccessPolicy> access =
      AccessPolicy::Create(schema.get(), "reader");
  ASSERT_TRUE(access.ok()) << access.status();
  SpannerSysCatalog spanner_sys_catalog;
  InformationSchemaCatalog catalog(InformationSchemaCatalog::kName,
                                   schema.get(), &spanner_sys_catalog,
                                   &*access);

  // The role holds privileges on T's columns K and A, on U through parent,
  // and on V through public. Rows about the information schema stay.
  const std::vector<Row> tables =
      ReadColumns(catalog, "TABLES", {"TABLE_SCHEMA", "TABLE_NAME"});
  EXPECT_THAT(RowsStartingWith(tables, ""),
              UnorderedElementsAre(Row{"", "T"}, Row{"", "U"}, Row{"", "V"}));
  EXPECT_THAT(tables, Contains(Row{"INFORMATION_SCHEMA", "TABLES"}));
  EXPECT_THAT(
      RowsStartingWith(ReadColumns(catalog, "COLUMNS",
                                   {"TABLE_SCHEMA", "TABLE_NAME",
                                    "COLUMN_NAME", "ORDINAL_POSITION"}),
                       ""),
      UnorderedElementsAre(Row{"", "T", "K", "1"}, Row{"", "T", "A", "2"},
                           Row{"", "U", "K", "1"}, Row{"", "V", "K", "1"}));
  EXPECT_THAT(
      RowsStartingWith(ReadColumns(catalog, "INDEXES",
                                   {"TABLE_SCHEMA", "TABLE_NAME",
                                    "INDEX_NAME"}),
                       ""),
      UnorderedElementsAre(Row{"", "T", "PRIMARY_KEY"}, Row{"", "T", "TByA"},
                           Row{"", "U", "PRIMARY_KEY"}));
  EXPECT_THAT(ReadColumns(catalog, "VIEWS", {"TABLE_NAME"}),
              ElementsAre(Row{"V"}));

  // TABLE_PRIVILEGES and COLUMN_PRIVILEGES list the privileges of all roles on
  // the objects the role may see. The ROLE_*_GRANTS tables list the privileges
  // of the role and the roles it is a member of, but not those of public.
  EXPECT_THAT(ReadTable(catalog, "TABLE_PRIVILEGES"),
              UnorderedElementsAre(Row{"", "", "U", "INSERT", "parent"},
                                   Row{"", "", "U", "DELETE", "other"},
                                   Row{"", "", "V", "SELECT", "public"}));
  EXPECT_THAT(ReadTable(catalog, "COLUMN_PRIVILEGES"),
              UnorderedElementsAre(Row{"", "", "T", "K", "SELECT", "reader"},
                                   Row{"", "", "T", "A", "SELECT", "reader"},
                                   Row{"", "", "U", "K", "INSERT", "parent"},
                                   Row{"", "", "V", "K", "SELECT", "public"}));
  EXPECT_THAT(ReadTable(catalog, "ROLE_TABLE_GRANTS"),
              ElementsAre(Row{"NULL", "parent", "", "", "U", "INSERT", "NO"}));
  EXPECT_THAT(
      ReadColumns(catalog, "ROLE_COLUMN_GRANTS",
                  {"GRANTEE", "TABLE_NAME", "COLUMN_NAME", "PRIVILEGE_TYPE"}),
      UnorderedElementsAre(Row{"reader", "T", "K", "SELECT"},
                           Row{"reader", "T", "A", "SELECT"},
                           Row{"parent", "U", "K", "INSERT"}));

  EXPECT_THAT(ReadTable(catalog, "ROLES"),
              UnorderedElementsAre(Row{"public", "true"},
                                   Row{"parent", "false"},
                                   Row{"reader", "false"}));
  EXPECT_THAT(ReadTable(catalog, "ROLE_GRANTEES"),
              ElementsAre(Row{"parent", "reader"}));

  EXPECT_THAT(ReadColumns(catalog, "CHANGE_STREAMS", {"CHANGE_STREAM_NAME"}),
              ElementsAre(Row{"CS"}));
  EXPECT_THAT(ReadColumns(catalog, "CHANGE_STREAM_TABLES",
                          {"CHANGE_STREAM_NAME", "TABLE_NAME"}),
              ElementsAre(Row{"CS", "T"}));
  EXPECT_THAT(ReadTable(catalog, "CHANGE_STREAM_PRIVILEGES"),
              ElementsAre(Row{"", "", "CS", "SELECT", "parent"}));
  EXPECT_THAT(ReadColumns(catalog, "ROUTINES", {"SPECIFIC_NAME"}),
              ElementsAre(Row{"READ_CS"}));
  EXPECT_THAT(ReadColumns(catalog, "PARAMETERS", {"SPECIFIC_NAME"}),
              Each(Row{"READ_CS"}));
  EXPECT_THAT(ReadTable(catalog, "ROUTINE_PRIVILEGES"),
              ElementsAre(Row{"", "", "READ_CS", "EXECUTE", "public"}));
  EXPECT_THAT(ReadTable(catalog, "ROLE_ROUTINE_GRANTS"), IsEmpty());

  EXPECT_THAT(ReadColumns(catalog, "MODELS", {"MODEL_NAME"}),
              ElementsAre(Row{"M"}));
  EXPECT_THAT(ReadColumns(catalog, "MODEL_COLUMNS", {"MODEL_NAME"}),
              Each(Row{"M"}));
  EXPECT_THAT(ReadTable(catalog, "MODEL_PRIVILEGES"),
              ElementsAre(Row{"", "", "M", "EXECUTE", "reader"}));
}

TEST(InformationSchemaCatalogTest, InfoReaderMembersSeeAllRows) {
  googlesql::TypeFactory type_factory;
  std::unique_ptr<const Schema> schema = CreateGrantsSchema(&type_factory);
  absl::StatusOr<AccessPolicy> access =
      AccessPolicy::Create(schema.get(), "auditor");
  ASSERT_TRUE(access.ok()) << access.status();
  SpannerSysCatalog spanner_sys_catalog;
  InformationSchemaCatalog catalog(InformationSchemaCatalog::kName,
                                   schema.get(), &spanner_sys_catalog,
                                   &*access);

  EXPECT_THAT(
      RowsStartingWith(
          ReadColumns(catalog, "TABLES", {"TABLE_SCHEMA", "TABLE_NAME"}), ""),
      UnorderedElementsAre(Row{"", "T"}, Row{"", "U"}, Row{"", "W"},
                           Row{"", "V"}));
  EXPECT_THAT(ReadTable(catalog, "ROLES"), Contains(Row{"other", "false"}));
  EXPECT_THAT(ReadColumns(catalog, "CHANGE_STREAMS", {"CHANGE_STREAM_NAME"}),
              UnorderedElementsAre(Row{"CS"}, Row{"CS2"}));
}

TEST(InformationSchemaCatalogTest, PGRolesPrivilegesAndRoutines) {
  googlesql::TypeFactory type_factory;
  auto schema = test::CreateSchemaFromDDL(
      {
          "CREATE TABLE t (k bigint PRIMARY KEY, a varchar)",
          "CREATE CHANGE STREAM cs FOR t",
          R"(CREATE FUNCTION my_add(a bigint, b bigint) RETURNS bigint
             LANGUAGE sql IMMUTABLE RETURN b + a)",
          "CREATE ROLE parent",
          "CREATE ROLE reader",
          "CREATE ROLE other",
          "GRANT parent TO reader",
          "GRANT SELECT ON TABLE t TO parent",
          "GRANT INSERT(a) ON TABLE t TO other",
          "GRANT EXECUTE ON FUNCTION spanner.read_json_cs TO public",
      },
      &type_factory, "", database_api::POSTGRESQL, "db");
  ASSERT_TRUE(schema.ok()) << schema.status();
  SpannerSysCatalog spanner_sys_catalog;
  InformationSchemaCatalog catalog(InformationSchemaCatalog::kPGName,
                                   schema->get(), &spanner_sys_catalog);

  // PostgreSQL does not list the public role.
  EXPECT_THAT(ReadTable(catalog, "enabled_roles"),
              UnorderedElementsAre(Row{"spanner_info_reader", "YES"},
                                   Row{"spanner_sys_reader", "YES"},
                                   Row{"parent", "NO"}, Row{"reader", "NO"},
                                   Row{"other", "NO"}));
  EXPECT_THAT(ReadTable(catalog, "applicable_roles"),
              ElementsAre(Row{"reader", "parent", "NO"}));
  EXPECT_THAT(ReadTable(catalog, "table_privileges"),
              ElementsAre(Row{"NULL", "parent", "db", "public", "t", "SELECT",
                              "NO", "NULL"}));
  EXPECT_THAT(ReadTable(catalog, "column_privileges"),
              UnorderedElementsAre(
                  Row{"NULL", "parent", "db", "public", "t", "k", "SELECT",
                      "NO"},
                  Row{"NULL", "parent", "db", "public", "t", "a", "SELECT",
                      "NO"},
                  Row{"NULL", "other", "db", "public", "t", "a", "INSERT",
                      "NO"}));
  EXPECT_THAT(ReadTable(catalog, "routine_privileges"),
              ElementsAre(Row{"NULL", "public", "db", "public", "read_json_cs",
                              "db", "public", "read_json_cs", "EXECUTE",
                              "NO"}));
  EXPECT_THAT(ReadColumns(catalog, "routines",
                          {"specific_schema", "specific_name", "routine_type",
                           "data_type", "routine_body", "routine_definition",
                           "security_type", "spanner_type",
                           "spanner_determinism"}),
              UnorderedElementsAre(
                  Row{"public", "read_json_cs", "FUNCTION", "jsonb",
                      "EXTERNAL", "", "INVOKER", "jsonb", "NULL"},
                  Row{"public", "my_add", "FUNCTION", "bigint", "SQL",
                      "(b + a)", "INVOKER", "bigint", "DETERMINISTIC"}));
  EXPECT_THAT(
      RowsStartingWith(ReadColumns(catalog, "parameters",
                                   {"specific_name", "ordinal_position",
                                    "parameter_name", "data_type",
                                    "spanner_type"}),
                       "read_json_cs"),
      ElementsAre(Row{"read_json_cs", "1", "start_timestamp",
                      "timestamp with time zone", "timestamp with time zone"},
                  Row{"read_json_cs", "2", "end_timestamp",
                      "timestamp with time zone", "timestamp with time zone"},
                  Row{"read_json_cs", "3", "partition_token",
                      "character varying", "character varying"},
                  Row{"read_json_cs", "4", "heartbeat_milliseconds", "bigint",
                      "bigint"},
                  Row{"read_json_cs", "5", "read_options", "ARRAY",
                      "character varying[]"}));
  EXPECT_THAT(
      RowsStartingWith(ReadColumns(catalog, "parameters",
                                   {"specific_name", "ordinal_position",
                                    "parameter_name", "data_type"}),
                       "my_add"),
      ElementsAre(Row{"my_add", "1", "a", "bigint"},
                  Row{"my_add", "2", "b", "bigint"}));
  EXPECT_THAT(ReadTable(catalog, "information_schema_catalog_name"),
              ElementsAre(Row{"db"}));
}

TEST(InformationSchemaCatalogTest, PGPrivilegesAreFilteredByGrantee) {
  googlesql::TypeFactory type_factory;
  auto schema = test::CreateSchemaFromDDL(
      {
          "CREATE TABLE t (k bigint PRIMARY KEY, a varchar)",
          "CREATE ROLE parent",
          "CREATE ROLE reader",
          "CREATE ROLE other",
          "GRANT parent TO reader",
          "GRANT SELECT ON TABLE t TO parent",
          "GRANT DELETE ON TABLE t TO other",
          "GRANT INSERT(a) ON TABLE t TO other",
      },
      &type_factory, "", database_api::POSTGRESQL, "db");
  ASSERT_TRUE(schema.ok()) << schema.status();
  absl::StatusOr<AccessPolicy> access =
      AccessPolicy::Create(schema->get(), "reader");
  ASSERT_TRUE(access.ok()) << access.status();
  SpannerSysCatalog spanner_sys_catalog;
  InformationSchemaCatalog catalog(InformationSchemaCatalog::kPGName,
                                   schema->get(), &spanner_sys_catalog,
                                   &*access);

  // Unlike GoogleSQL, PostgreSQL lists only the privileges of the role, the
  // roles it is a member of and public.
  EXPECT_THAT(ReadColumns(catalog, "table_privileges",
                          {"grantee", "table_name", "privilege_type"}),
              ElementsAre(Row{"parent", "t", "SELECT"}));
  EXPECT_THAT(ReadColumns(catalog, "column_privileges",
                          {"grantee", "column_name", "privilege_type"}),
              UnorderedElementsAre(Row{"parent", "k", "SELECT"},
                                   Row{"parent", "a", "SELECT"}));
  EXPECT_THAT(ReadTable(catalog, "enabled_roles"),
              UnorderedElementsAre(Row{"parent", "NO"}, Row{"reader", "NO"}));
}

}  // namespace
}  // namespace google::spanner::emulator::backend

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
#include "absl/strings/match.h"
#include "backend/query/info_schema_columns_metadata_values.h"
#include "backend/query/spanner_sys_catalog.h"
#include "tests/common/schema_constructor.h"

namespace google::spanner::emulator::backend {

namespace {

using ::testing::ElementsAre;
using ::testing::IsEmpty;
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
  EXPECT_EQ(ColumnsMetadata().size(), 272);
}

TEST(InformationSchemaCatalogTest, IndexColumnsMetadataCount) {
  Schema schema;
  SpannerSysCatalog spanner_sys_catalog;
  InformationSchemaCatalog catalog(InformationSchemaCatalog::kName, &schema,
                                   &spanner_sys_catalog);
  EXPECT_EQ(IndexColumnsMetadata().size(), 171);
}

TEST(InformationSchemaCatalogTest, SpannerSysColumnsMetadataCount) {
  Schema schema;
  SpannerSysCatalog spanner_sys_catalog;
  InformationSchemaCatalog catalog(InformationSchemaCatalog::kName, &schema,
                                   &spanner_sys_catalog);
  EXPECT_EQ(SpannerSysColumnsMetadata().size(), 450);
}

TEST(InformationSchemaCatalogTest, PGColumnsMetadataCount) {
  Schema schema;
  SpannerSysCatalog spanner_sys_catalog;
  InformationSchemaCatalog catalog(InformationSchemaCatalog::kPGName, &schema,
                                   &spanner_sys_catalog);
  EXPECT_EQ(PGColumnsMetadata().size(), 421);
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
  EXPECT_EQ(SpannerSysColumnsMetadata().size(), 450);
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

}  // namespace
}  // namespace google::spanner::emulator::backend

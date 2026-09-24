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

#include <memory>
#include <optional>
#include <string>
#include <vector>

#include "google/spanner/admin/database/v1/common.pb.h"
#include "googlesql/public/value.h"
#include "gmock/gmock.h"
#include "gtest/gtest.h"
#include "googlesql/base/testing/status_matchers.h"
#include "absl/container/flat_hash_set.h"
#include "absl/status/status.h"
#include "absl/status/statusor.h"
#include "absl/time/time.h"
#include "backend/datamodel/key.h"
#include "backend/schema/catalog/column.h"
#include "backend/schema/catalog/placement.h"
#include "backend/schema/catalog/schema.h"
#include "backend/schema/catalog/table.h"
#include "backend/schema/updater/schema_updater.h"
#include "backend/schema/updater/schema_updater_tests/base.h"

namespace google {
namespace spanner {
namespace emulator {
namespace backend {
namespace test {

namespace {

using database_api::DatabaseDialect::GOOGLE_STANDARD_SQL;
using database_api::DatabaseDialect::POSTGRESQL;
using ::googlesql_base::testing::StatusIs;
using ::testing::HasSubstr;

// Timestamp at which test rows are written; schema changes in these tests
// validate at the Unix epoch, so rows must be visible by then.
const absl::Time kRowTimestamp = absl::UnixEpoch() - absl::Seconds(1);

class PlacementTest : public SchemaUpdaterTest {
 protected:
  // Applies `operation` on top of `base_schema` without running backfills.
  absl::StatusOr<std::unique_ptr<const Schema>> ApplyOperation(
      const Schema* base_schema, const SchemaChangeOperation& operation) {
    SchemaUpdater updater;
    SchemaChangeContext context{.type_factory = &type_factory_,
                                .table_id_generator = &table_id_generator_,
                                .column_id_generator = &column_id_generator_,
                                .storage = storage_.get(),
                                .pg_oid_assigner = pg_oid_assigner_.get()};
    return updater.ValidateSchemaFromDDL(operation, context, base_schema);
  }

  // Writes a row of the Singers table created by `CreateSingersSchema`.
  absl::Status WriteSinger(const Schema* schema, int64_t singer_id,
                           const std::string& location) {
    const Table* table = schema->FindTable("Singers");
    return storage_->Write(
        kRowTimestamp, table->id(), Key({googlesql::Value::Int64(singer_id)}),
        {table->FindColumn("SingerId")->id(),
         table->FindColumn("Location")->id()},
        {googlesql::Value::Int64(singer_id),
         googlesql::Value::String(location)});
  }

  absl::StatusOr<std::unique_ptr<const Schema>> CreateSingersSchema() {
    return CreateSchema({
        R"(CREATE PLACEMENT europe OPTIONS (instance_partition = 'eu'))",
        R"(CREATE PLACEMENT asia OPTIONS (instance_partition = 'as'))",
        R"(
          CREATE TABLE Singers (
            SingerId INT64 NOT NULL,
            Name STRING(MAX),
            Location STRING(MAX) NOT NULL PLACEMENT KEY
          ) PRIMARY KEY (SingerId)
        )",
    });
  }
};

INSTANTIATE_TEST_SUITE_P(
    SchemaUpdaterPerDialectTests, PlacementTest,
    testing::Values(GOOGLE_STANDARD_SQL, POSTGRESQL),
    [](const testing::TestParamInfo<PlacementTest::ParamType>& info) {
      return database_api::DatabaseDialect_Name(info.param);
    });

TEST_P(PlacementTest, CreatePlacementAndPlacementTable) {
  std::unique_ptr<const Schema> schema;
  if (GetParam() == POSTGRESQL) {
    GOOGLESQL_ASSERT_OK_AND_ASSIGN(
        schema, CreateSchema(
                    {
                        R"(CREATE PLACEMENT europe WITH (instance_partition = 'eu', default_leader = 'europe-west1'))",
                        R"(
                          CREATE TABLE singers (
                            singerid bigint PRIMARY KEY,
                            location varchar(1024) NOT NULL PLACEMENT KEY
                          )
                        )",
                    },
                    /*proto_descriptor_bytes=*/"", POSTGRESQL,
                    /*use_gsql_to_pg_translation=*/false));
  } else {
    GOOGLESQL_ASSERT_OK_AND_ASSIGN(schema, CreateSchema({
                                     R"(CREATE PLACEMENT europe OPTIONS (instance_partition = 'eu', default_leader = 'europe-west1'))",
                                     R"(
                                       CREATE TABLE singers (
                                         singerid INT64 NOT NULL,
                                         location STRING(1024) NOT NULL PLACEMENT KEY
                                       ) PRIMARY KEY (singerid)
                                     )",
                                 }));
  }
  const Placement* placement = schema->FindPlacement("europe");
  ASSERT_NE(placement, nullptr);
  EXPECT_EQ(placement->InstancePartition(), "eu");
  EXPECT_EQ(placement->DefaultLeader(), "europe-west1");
  const Table* table = schema->FindTable("singers");
  ASSERT_NE(table, nullptr);
  EXPECT_TRUE(table->FindColumn("location")->is_placement_key());
  EXPECT_FALSE(table->FindColumn("singerid")->is_placement_key());
}

TEST_P(PlacementTest, DropPlacement) {
  std::unique_ptr<const Schema> schema;
  if (GetParam() == POSTGRESQL) {
    GOOGLESQL_ASSERT_OK_AND_ASSIGN(
        schema, CreateSchema(
                    {
                        R"(CREATE PLACEMENT europe WITH (instance_partition = 'eu'))",
                        R"(DROP PLACEMENT europe)",
                        R"(DROP PLACEMENT IF EXISTS europe)",
                    },
                    /*proto_descriptor_bytes=*/"", POSTGRESQL,
                    /*use_gsql_to_pg_translation=*/false));
  } else {
    GOOGLESQL_ASSERT_OK_AND_ASSIGN(
        schema, CreateSchema({
                    R"(CREATE PLACEMENT europe OPTIONS (instance_partition = 'eu'))",
                    R"(DROP PLACEMENT europe)",
                    R"(DROP PLACEMENT IF EXISTS europe)",
                }));
  }
  EXPECT_EQ(schema->FindPlacement("europe"), nullptr);
}

TEST_P(PlacementTest, CreatePlacementIfNotExistsIsNoOpWhenItExists) {
  if (GetParam() == POSTGRESQL) GTEST_SKIP();
  GOOGLESQL_ASSERT_OK_AND_ASSIGN(
      auto schema,
      CreateSchema({
          R"(CREATE PLACEMENT europe OPTIONS (instance_partition = 'eu'))",
          R"(CREATE PLACEMENT IF NOT EXISTS europe OPTIONS (instance_partition = 'other'))",
      }));
  EXPECT_EQ(schema->FindPlacement("europe")->InstancePartition(), "eu");

  EXPECT_THAT(
      UpdateSchema(schema.get(), {R"(CREATE PLACEMENT europe OPTIONS (instance_partition = 'eu'))"}),
      StatusIs(absl::StatusCode::kFailedPrecondition));
}

TEST_P(PlacementTest, AlterAndDropMissingPlacementWithIfExists) {
  if (GetParam() == POSTGRESQL) GTEST_SKIP();
  GOOGLESQL_EXPECT_OK(CreateSchema({
      R"(ALTER PLACEMENT IF EXISTS missing SET OPTIONS (default_leader = 'us-east1'))",
      R"(DROP PLACEMENT IF EXISTS missing)",
  }));
  EXPECT_THAT(CreateSchema({R"(DROP PLACEMENT missing)"}),
              StatusIs(absl::StatusCode::kNotFound));
}

TEST_P(PlacementTest, DefaultPlacementNameIsReserved) {
  if (GetParam() == POSTGRESQL) GTEST_SKIP();
  EXPECT_THAT(
      CreateSchema({R"(CREATE PLACEMENT `default` OPTIONS (instance_partition = 'eu'))"}),
      StatusIs(absl::StatusCode::kInvalidArgument, HasSubstr("reserved")));
}

TEST_P(PlacementTest, AlterPlacementOnlyChangesNamedOptions) {
  if (GetParam() == POSTGRESQL) GTEST_SKIP();
  GOOGLESQL_ASSERT_OK_AND_ASSIGN(
      auto schema,
      CreateSchema({
          R"(CREATE PLACEMENT europe OPTIONS (instance_partition = 'eu', default_leader = 'europe-west1'))",
          R"(ALTER PLACEMENT europe SET OPTIONS (default_leader = 'europe-west4'))",
      }));
  const Placement* placement = schema->FindPlacement("europe");
  EXPECT_EQ(placement->InstancePartition(), "eu");
  EXPECT_EQ(placement->DefaultLeader(), "europe-west4");
  auto options = placement->options();
  ASSERT_EQ(options.size(), 2);
  EXPECT_EQ(options[0].option_name(), "instance_partition");
  EXPECT_EQ(options[1].option_name(), "default_leader");
  EXPECT_EQ(options[1].string_value(), "europe-west4");

  GOOGLESQL_ASSERT_OK_AND_ASSIGN(
      schema,
      UpdateSchema(schema.get(), {R"(ALTER PLACEMENT europe SET OPTIONS (default_leader = NULL))"}));
  placement = schema->FindPlacement("europe");
  EXPECT_EQ(placement->InstancePartition(), "eu");
  EXPECT_FALSE(placement->DefaultLeader().has_value());
  options = placement->options();
  ASSERT_EQ(options.size(), 1);
  EXPECT_EQ(options[0].option_name(), "instance_partition");
}

TEST_P(PlacementTest, PlacementKeyMustBeNotNullString) {
  if (GetParam() == POSTGRESQL) GTEST_SKIP();
  EXPECT_THAT(CreateSchema({R"(
                CREATE TABLE T (
                  K INT64 NOT NULL,
                  L INT64 NOT NULL PLACEMENT KEY
                ) PRIMARY KEY (K))"}),
              StatusIs(absl::StatusCode::kFailedPrecondition,
                       HasSubstr("must be of type STRING")));
  EXPECT_THAT(CreateSchema({R"(
                CREATE TABLE T (
                  K INT64 NOT NULL,
                  L STRING(MAX) PLACEMENT KEY
                ) PRIMARY KEY (K))"}),
              StatusIs(absl::StatusCode::kFailedPrecondition,
                       HasSubstr("must be NOT NULL")));
  EXPECT_THAT(CreateSchema({R"(
                CREATE TABLE T (
                  K INT64 NOT NULL,
                  L1 STRING(MAX) NOT NULL PLACEMENT KEY,
                  L2 STRING(MAX) NOT NULL PLACEMENT KEY
                ) PRIMARY KEY (K))"}),
              StatusIs(absl::StatusCode::kFailedPrecondition,
                       HasSubstr("more than one placement key")));
}

TEST_P(PlacementTest, PlacementKeyCanOnlyBeDefinedAtCreateTable) {
  if (GetParam() == POSTGRESQL) GTEST_SKIP();
  GOOGLESQL_ASSERT_OK_AND_ASSIGN(auto schema, CreateSingersSchema());
  EXPECT_THAT(
      UpdateSchema(schema.get(),
                   {R"(ALTER TABLE Singers ADD COLUMN L2 STRING(MAX) PLACEMENT KEY)"}),
      StatusIs(absl::StatusCode::kFailedPrecondition,
               HasSubstr("Cannot make column Singers.L2 a placement key")));
  EXPECT_THAT(
      UpdateSchema(schema.get(),
                   {R"(ALTER TABLE Singers ALTER COLUMN Name STRING(MAX) NOT NULL PLACEMENT KEY)"}),
      StatusIs(absl::StatusCode::kFailedPrecondition,
               HasSubstr("Cannot make column Singers.Name a placement key")));
}

TEST_P(PlacementTest, PlacementKeyCannotBeDroppedOrLoosened) {
  if (GetParam() == POSTGRESQL) GTEST_SKIP();
  GOOGLESQL_ASSERT_OK_AND_ASSIGN(auto schema, CreateSingersSchema());
  EXPECT_THAT(UpdateSchema(schema.get(),
                           {R"(ALTER TABLE Singers DROP COLUMN Location)"}),
              StatusIs(absl::StatusCode::kFailedPrecondition,
                       HasSubstr("Cannot drop placement key column")));
  EXPECT_THAT(
      UpdateSchema(schema.get(),
                   {R"(ALTER TABLE Singers ALTER COLUMN Location STRING(MAX))"}),
      StatusIs(absl::StatusCode::kFailedPrecondition,
               HasSubstr("Cannot alter placement key column")));
  EXPECT_THAT(
      UpdateSchema(schema.get(),
                   {R"(ALTER TABLE Singers ALTER COLUMN Location BYTES(MAX) NOT NULL)"}),
      StatusIs(absl::StatusCode::kFailedPrecondition,
               HasSubstr("Cannot alter placement key column")));

  // Resizing keeps the column a placement key.
  GOOGLESQL_ASSERT_OK_AND_ASSIGN(
      schema,
      UpdateSchema(schema.get(),
                   {R"(ALTER TABLE Singers ALTER COLUMN Location STRING(100) NOT NULL)"}));
  EXPECT_TRUE(schema->FindTable("Singers")
                  ->FindColumn("Location")
                  ->is_placement_key());
}

TEST_P(PlacementTest, DropPlacementInUse) {
  if (GetParam() == POSTGRESQL) GTEST_SKIP();
  GOOGLESQL_ASSERT_OK_AND_ASSIGN(auto schema, CreateSingersSchema());
  GOOGLESQL_ASSERT_OK(WriteSinger(schema.get(), 1, "europe"));

  EXPECT_THAT(UpdateSchema(schema.get(), {R"(DROP PLACEMENT europe)"}),
              StatusIs(absl::StatusCode::kFailedPrecondition,
                       HasSubstr("Placement europe can't be dropped because "
                                 "it is in use by placement table Singers")));
  // A placement no row uses can be dropped.
  GOOGLESQL_EXPECT_OK(UpdateSchema(schema.get(), {R"(DROP PLACEMENT asia)"}));
}

TEST_P(PlacementTest, DropPlacementTable) {
  if (GetParam() == POSTGRESQL) GTEST_SKIP();
  GOOGLESQL_ASSERT_OK_AND_ASSIGN(auto schema, CreateSingersSchema());
  GOOGLESQL_ASSERT_OK(WriteSinger(schema.get(), 1, "default"));
  EXPECT_THAT(UpdateSchema(schema.get(), {R"(DROP TABLE Singers)"}),
              StatusIs(absl::StatusCode::kFailedPrecondition,
                       HasSubstr("Cannot drop placement table Singers")));

  // An empty placement table can be dropped. CreateSingersSchema assigns a new
  // table id, so the row above isn't visible to it.
  GOOGLESQL_ASSERT_OK_AND_ASSIGN(schema, CreateSingersSchema());
  GOOGLESQL_EXPECT_OK(UpdateSchema(schema.get(), {R"(DROP TABLE Singers)"}));
}

TEST_P(PlacementTest, PerPlacementRoutingMetadata) {
  std::unique_ptr<const Schema> schema;
  if (GetParam() == POSTGRESQL) {
    GOOGLESQL_ASSERT_OK_AND_ASSIGN(
        schema,
        CreateSchema(
            {R"(ALTER DATABASE db SET spanner.per_placement_routing_metadata = false)"},
            /*proto_descriptor_bytes=*/"", POSTGRESQL,
            /*use_gsql_to_pg_translation=*/false));
  } else {
    GOOGLESQL_ASSERT_OK_AND_ASSIGN(
        schema,
        CreateSchema(
            {R"(ALTER DATABASE db SET OPTIONS (per_placement_routing_metadata = false))"}));
  }
  ASSERT_NE(schema->options(), nullptr);
  EXPECT_EQ(schema->options()->per_placement_routing_metadata(), false);

  if (GetParam() == POSTGRESQL) return;
  GOOGLESQL_ASSERT_OK_AND_ASSIGN(
      schema,
      UpdateSchema(schema.get(), {R"(CREATE PLACEMENT europe OPTIONS (instance_partition = 'eu'))"}));
  EXPECT_THAT(
      UpdateSchema(schema.get(),
                   {R"(ALTER DATABASE db SET OPTIONS (per_placement_routing_metadata = true))"}),
      StatusIs(absl::StatusCode::kFailedPrecondition,
               HasSubstr("per_placement_routing_metadata")));
}

TEST_P(PlacementTest, InstancePartitionMustExistWhenKnown) {
  if (GetParam() == POSTGRESQL) GTEST_SKIP();
  const std::vector<std::string> create = {
      R"(CREATE PLACEMENT europe OPTIONS (instance_partition = 'eu'))"};
  EXPECT_THAT(
      ApplyOperation(nullptr, SchemaChangeOperation{
                                  .statements = create,
                                  .database_dialect = GOOGLE_STANDARD_SQL,
                                  .instance_partitions =
                                      absl::flat_hash_set<std::string>{"as"}}),
      StatusIs(absl::StatusCode::kNotFound,
               HasSubstr("Instance partition eu referenced by placement "
                         "europe does not exist")));

  GOOGLESQL_ASSERT_OK_AND_ASSIGN(
      auto schema,
      ApplyOperation(nullptr, SchemaChangeOperation{
                                  .statements = create,
                                  .database_dialect = GOOGLE_STANDARD_SQL,
                                  .instance_partitions =
                                      absl::flat_hash_set<std::string>{"eu"}}));

  const std::vector<std::string> alter = {
      R"(ALTER PLACEMENT europe SET OPTIONS (instance_partition = 'missing'))"};
  EXPECT_THAT(
      ApplyOperation(schema.get(),
                     SchemaChangeOperation{
                         .statements = alter,
                         .database_dialect = GOOGLE_STANDARD_SQL,
                         .instance_partitions =
                             absl::flat_hash_set<std::string>{"eu"}}),
      StatusIs(absl::StatusCode::kNotFound));
}

TEST_P(PlacementTest, ReplaySkipsChecksAddedAfterStatementsWereAccepted) {
  if (GetParam() == POSTGRESQL) GTEST_SKIP();
  const std::vector<std::string> statements = {
      R"(CREATE PLACEMENT europe OPTIONS (instance_partition = 'missing'))",
      R"(CREATE TABLE T (K INT64 NOT NULL, L INT64 PLACEMENT KEY) PRIMARY KEY (K))",
      R"(ALTER TABLE T ADD COLUMN L2 STRING(MAX) PLACEMENT KEY)",
  };
  EXPECT_THAT(
      ApplyOperation(nullptr, SchemaChangeOperation{
                                  .statements = statements,
                                  .database_dialect = GOOGLE_STANDARD_SQL,
                                  .instance_partitions =
                                      absl::flat_hash_set<std::string>{}}),
      StatusIs(absl::StatusCode::kNotFound));
  GOOGLESQL_EXPECT_OK(
      ApplyOperation(nullptr, SchemaChangeOperation{
                                  .statements = statements,
                                  .database_dialect = GOOGLE_STANDARD_SQL,
                                  .replaying_committed_ddl = true,
                                  .instance_partitions =
                                      absl::flat_hash_set<std::string>{}}));
}

}  // namespace

}  // namespace test
}  // namespace backend
}  // namespace emulator
}  // namespace spanner
}  // namespace google

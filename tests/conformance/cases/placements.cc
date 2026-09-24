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
#include <string>
#include <vector>

#include "google/longrunning/operations.pb.h"
#include "google/spanner/admin/database/v1/common.pb.h"
#include "google/spanner/admin/instance/v1/spanner_instance_admin.grpc.pb.h"
#include "google/spanner/admin/instance/v1/spanner_instance_admin.pb.h"
#include "gmock/gmock.h"
#include "gtest/gtest.h"
#include "googlesql/base/testing/status_matchers.h"
#include "tests/common/proto_matchers.h"
#include "absl/status/status.h"
#include "absl/strings/str_cat.h"
#include "common/config.h"
#include "grpcpp/client_context.h"
#include "grpcpp/create_channel.h"
#include "tests/conformance/common/database_test_base.h"
#include "tests/conformance/common/environment.h"

namespace google {
namespace spanner {
namespace emulator {
namespace test {

namespace {

namespace instance_api = ::google::spanner::admin::instance::v1;

using ::googlesql_base::testing::StatusIs;
using ::testing::AllOf;
using ::testing::Contains;
using ::testing::HasSubstr;

// Instance partitions shared by all tests; placements must reference an
// instance partition that exists in the database's instance.
constexpr char kEuropePartition[] = "placement-test-eu";
constexpr char kAsiaPartition[] = "placement-test-as";

// Creates `partition_id` in the test instance unless it already exists.
absl::Status CreateInstancePartition(const std::string& partition_id) {
  const ConformanceTestGlobals& globals = GetConformanceTestGlobals();
  std::shared_ptr<grpc::Channel> channel = grpc::CreateChannel(
      globals.connection_options->get<google::cloud::EndpointOption>(),
      globals.connection_options->get<google::cloud::GrpcCredentialOption>());
  std::unique_ptr<instance_api::InstanceAdmin::Stub> stub =
      instance_api::InstanceAdmin::NewStub(channel);

  instance_api::CreateInstancePartitionRequest request;
  request.set_parent(absl::StrCat("projects/", globals.project_id,
                                  "/instances/", globals.instance_id));
  request.set_instance_partition_id(partition_id);
  instance_api::InstancePartition* partition =
      request.mutable_instance_partition();
  partition->set_config(absl::StrCat("projects/", globals.project_id,
                                     "/instanceConfigs/emulator-config"));
  partition->set_display_name(partition_id);
  partition->set_node_count(1);

  grpc::ClientContext context;
  longrunning::Operation operation;
  grpc::Status status =
      stub->CreateInstancePartition(&context, request, &operation);
  if (status.error_code() == grpc::StatusCode::ALREADY_EXISTS) {
    return absl::OkStatus();
  }
  return absl::Status(static_cast<absl::StatusCode>(status.error_code()),
                      status.error_message());
}

class PlacementsTest
    : public DatabaseTest,
      public testing::WithParamInterface<database_api::DatabaseDialect> {
 public:
  void SetUp() override {
    dialect_ = GetParam();
    DatabaseTest::SetUp();
  }

  void TearDown() override {
    config::set_enforce_placement_dml_restrictions(true);
    DatabaseTest::TearDown();
  }

  absl::Status SetUpDatabase() override {
    // Geo-partitioning needs an Enterprise Plus instance with instance
    // partitions; these tests only run against the emulator.
    if (in_prod_env()) {
      return absl::OkStatus();
    }
    GOOGLESQL_RETURN_IF_ERROR(CreateInstancePartition(kEuropePartition));
    GOOGLESQL_RETURN_IF_ERROR(CreateInstancePartition(kAsiaPartition));
    if (dialect_ == database_api::DatabaseDialect::POSTGRESQL) {
      return SetSchema({
          absl::StrCat("CREATE PLACEMENT europe WITH (instance_partition = '",
                       kEuropePartition, "')"),
          absl::StrCat("CREATE PLACEMENT asia WITH (instance_partition = '",
                       kAsiaPartition, "', default_leader = 'asia-east1')"),
          R"(
            CREATE TABLE singers (
              singerid bigint PRIMARY KEY,
              name varchar,
              location varchar NOT NULL PLACEMENT KEY
            )
          )",
          "CREATE TABLE plain (id bigint PRIMARY KEY)",
      });
    }
    return SetSchema({
        absl::StrCat("CREATE PLACEMENT europe OPTIONS (instance_partition = '",
                     kEuropePartition, "')"),
        absl::StrCat("CREATE PLACEMENT asia OPTIONS (instance_partition = '",
                     kAsiaPartition, "', default_leader = 'asia-east1')"),
        R"(
          CREATE TABLE singers (
            singerid INT64 NOT NULL,
            name STRING(MAX),
            location STRING(MAX) NOT NULL PLACEMENT KEY
          ) PRIMARY KEY (singerid)
        )",
        "CREATE TABLE plain (id INT64 NOT NULL) PRIMARY KEY (id)",
    });
  }

 protected:
  absl::StatusOr<CommitResult> InsertSinger(int64_t singer_id,
                                            const std::string& location) {
    return Insert("singers", {"singerid", "name", "location"},
                  {singer_id, "singer", location});
  }

  bool is_postgresql() const {
    return dialect_ == database_api::DatabaseDialect::POSTGRESQL;
  }
};

INSTANTIATE_TEST_SUITE_P(
    PerDialectPlacementsTest, PlacementsTest,
    testing::Values(database_api::DatabaseDialect::GOOGLE_STANDARD_SQL,
                    database_api::DatabaseDialect::POSTGRESQL),
    [](const testing::TestParamInfo<PlacementsTest::ParamType>& info) {
      return database_api::DatabaseDialect_Name(info.param);
    });

TEST_P(PlacementsTest, GetDatabaseDdlIncludesPlacements) {
  if (in_prod_env()) GTEST_SKIP();
  GOOGLESQL_ASSERT_OK_AND_ASSIGN(std::vector<std::string> ddl, GetDatabaseDdl());
  EXPECT_THAT(ddl, Contains(AllOf(HasSubstr("CREATE PLACEMENT europe"),
                                  HasSubstr(kEuropePartition))));
  EXPECT_THAT(ddl, Contains(AllOf(HasSubstr("CREATE PLACEMENT asia"),
                                  HasSubstr("asia-east1"))));
  EXPECT_THAT(ddl, Contains(HasSubstr("PLACEMENT KEY")));
}

TEST_P(PlacementsTest, WritesRequireKnownPlacementOrDefault) {
  if (in_prod_env()) GTEST_SKIP();
  GOOGLESQL_EXPECT_OK(InsertSinger(1, "europe"));
  GOOGLESQL_EXPECT_OK(InsertSinger(2, "default"));
  EXPECT_THAT(InsertSinger(3, "nowhere"),
              StatusIs(absl::StatusCode::kInvalidArgument,
                       HasSubstr("Unknown placement: nowhere")));
  EXPECT_THAT(
      Query("SELECT singerid, location FROM singers ORDER BY singerid"),
      IsOkAndHoldsRows({{1, "europe"}, {2, "default"}}));
}

TEST_P(PlacementsTest, MoveRowBetweenPlacements) {
  if (in_prod_env()) GTEST_SKIP();
  GOOGLESQL_ASSERT_OK(InsertSinger(1, "europe"));
  GOOGLESQL_EXPECT_OK(
      CommitDml({"UPDATE singers SET location = 'asia' WHERE singerid = 1"}));
  EXPECT_THAT(Query("SELECT location FROM singers WHERE singerid = 1"),
              IsOkAndHoldsRows({{"asia"}}));
  GOOGLESQL_EXPECT_OK(Update("singers", {"singerid", "location"}, {1, "default"}));
  EXPECT_THAT(Query("SELECT location FROM singers WHERE singerid = 1"),
              IsOkAndHoldsRows({{"default"}}));
}

TEST_P(PlacementsTest, ReadWriteTransactionsFilterOnlyOnPrimaryKeyColumns) {
  if (in_prod_env()) GTEST_SKIP();
  GOOGLESQL_ASSERT_OK(InsertSinger(1, "europe"));
  EXPECT_THAT(
      CommitDml({"UPDATE singers SET name = 'x' WHERE location = 'europe'"}),
      StatusIs(absl::StatusCode::kInvalidArgument,
               HasSubstr("primary key columns of placement table")));

  // Partitioned DML and read-only transactions may filter on any column.
  GOOGLESQL_EXPECT_OK(ExecutePartitionedDml(SqlStatement(
      "UPDATE singers SET location = 'asia' WHERE location = 'europe'")));
  EXPECT_THAT(Query("SELECT singerid FROM singers WHERE location = 'asia'"),
              IsOkAndHoldsRows({{1}}));
}

TEST_P(PlacementsTest, InsertOrDeleteMustBeOnlyStatementInTransaction) {
  if (in_prod_env()) GTEST_SKIP();
  EXPECT_THAT(
      CommitDml({"INSERT INTO singers (singerid, name, location) "
                 "VALUES (10, 'a', 'europe')",
                 "INSERT INTO plain (id) VALUES (10)"}),
      StatusIs(absl::StatusCode::kFailedPrecondition,
               HasSubstr("must be the only statement in the transaction")));
  GOOGLESQL_EXPECT_OK(CommitDml({"INSERT INTO singers (singerid, name, location) "
                       "VALUES (10, 'a', 'europe')"}));
  EXPECT_THAT(
      CommitDml({"INSERT INTO plain (id) VALUES (11)",
                 "DELETE FROM singers WHERE singerid = 10"}),
      StatusIs(absl::StatusCode::kFailedPrecondition,
               HasSubstr("must be the only statement in the transaction")));
  GOOGLESQL_EXPECT_OK(CommitDml({"DELETE FROM singers WHERE singerid = 10"}));
}

TEST_P(PlacementsTest, DmlRestrictionsCanBeDisabled) {
  if (in_prod_env()) GTEST_SKIP();
  config::set_enforce_placement_dml_restrictions(false);
  GOOGLESQL_EXPECT_OK(CommitDml({"INSERT INTO singers (singerid, name, location) "
                       "VALUES (10, 'a', 'europe')",
                       "INSERT INTO plain (id) VALUES (10)",
                       "UPDATE singers SET name = 'b' WHERE location = 'europe'"}));
}

TEST_P(PlacementsTest, DropPlacementInUseFails) {
  if (in_prod_env()) GTEST_SKIP();
  GOOGLESQL_ASSERT_OK(InsertSinger(1, "europe"));
  EXPECT_THAT(UpdateSchema({"DROP PLACEMENT europe"}),
              StatusIs(absl::StatusCode::kFailedPrecondition,
                       HasSubstr("Placement europe can't be dropped because "
                                 "it is in use by placement table singers")));
  GOOGLESQL_EXPECT_OK(UpdateSchema({"DROP PLACEMENT asia"}));
}

TEST_P(PlacementsTest, DropNonEmptyPlacementTableFails) {
  if (in_prod_env()) GTEST_SKIP();
  GOOGLESQL_ASSERT_OK(InsertSinger(1, "europe"));
  EXPECT_THAT(UpdateSchema({"DROP TABLE singers"}),
              StatusIs(absl::StatusCode::kFailedPrecondition,
                       HasSubstr("Cannot drop placement table singers")));
  GOOGLESQL_ASSERT_OK(Delete("singers", cloud::spanner::MakeKey(1)));
  GOOGLESQL_EXPECT_OK(UpdateSchema({"DROP TABLE singers"}));
}

TEST_P(PlacementsTest, PlacementRequiresExistingInstancePartition) {
  if (in_prod_env()) GTEST_SKIP();
  const std::string statement =
      is_postgresql()
          ? "CREATE PLACEMENT nowhere WITH (instance_partition = 'missing')"
          : "CREATE PLACEMENT nowhere OPTIONS (instance_partition = 'missing')";
  EXPECT_THAT(UpdateSchema({statement}),
              StatusIs(absl::StatusCode::kNotFound,
                       HasSubstr("Instance partition missing referenced by "
                                 "placement nowhere does not exist")));
}

TEST_P(PlacementsTest, InformationSchemaListsPlacements) {
  if (in_prod_env()) GTEST_SKIP();
  EXPECT_THAT(Query("SELECT placement_name FROM information_schema.placements "
                    "ORDER BY placement_name"),
              IsOkAndHoldsRows({{"asia"}, {"default"}, {"europe"}}));
  EXPECT_THAT(
      Query("SELECT placement_name, option_name, option_value "
            "FROM information_schema.placement_options "
            "ORDER BY placement_name, option_name"),
      IsOkAndHoldsRows({{"asia", "default_leader", "asia-east1"},
                        {"asia", "instance_partition", kAsiaPartition},
                        {"europe", "instance_partition", kEuropePartition}}));
  if (!is_postgresql()) {
    EXPECT_THAT(Query("SELECT PLACEMENT_NAME, IS_DEFAULT "
                      "FROM INFORMATION_SCHEMA.PLACEMENTS "
                      "ORDER BY PLACEMENT_NAME"),
                IsOkAndHoldsRows(
                    {{"asia", false}, {"default", true}, {"europe", false}}));
  }
}

TEST_P(PlacementsTest, PlacementKeyCannotBeAddedOrDropped) {
  if (in_prod_env() || is_postgresql()) GTEST_SKIP();
  // These statements used to abort the emulator process.
  EXPECT_THAT(
      UpdateSchema({"ALTER TABLE singers ALTER COLUMN name SET PLACEMENT KEY"}),
      StatusIs(absl::StatusCode::kInvalidArgument,
               HasSubstr("Cannot make column singers.name a placement key")));
  EXPECT_THAT(
      UpdateSchema(
          {"ALTER TABLE singers ALTER COLUMN location DROP PLACEMENT KEY"}),
      StatusIs(absl::StatusCode::kInvalidArgument,
               HasSubstr("Cannot drop placement key column")));
  EXPECT_THAT(UpdateSchema({"ALTER TABLE singers ALTER COLUMN name "
                            "STRING(MAX) NOT NULL PLACEMENT KEY"}),
              StatusIs(absl::StatusCode::kFailedPrecondition,
                       HasSubstr("Cannot make column singers.name a "
                                 "placement key")));
  EXPECT_THAT(UpdateSchema({"ALTER TABLE plain ADD COLUMN location "
                            "STRING(MAX) PLACEMENT KEY"}),
              StatusIs(absl::StatusCode::kFailedPrecondition,
                       HasSubstr("Cannot make column plain.location a "
                                 "placement key")));
  EXPECT_THAT(Query("SELECT COUNT(*) FROM singers"), IsOkAndHoldsRows({{0}}));
}

}  // namespace

}  // namespace test
}  // namespace emulator
}  // namespace spanner
}  // namespace google

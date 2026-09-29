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

#include <string>

#include "google/spanner/admin/database/v1/common.pb.h"
#include "google/spanner/v1/spanner.pb.h"
#include "gmock/gmock.h"
#include "gtest/gtest.h"
#include "googlesql/base/testing/status_matchers.h"
#include "tests/common/scoped_feature_flags_setter.h"
#include "tests/conformance/common/database_test_base.h"
#include "grpcpp/client_context.h"

namespace google {
namespace spanner {
namespace emulator {
namespace test {
namespace {

constexpr char kUuidPattern[] =
    "[0-9a-f]{8}-[0-9a-f]{4}-[0-9a-f]{4}-[0-9a-f]{4}-[0-9a-f]{12}";

class UuidApiTest
    : public DatabaseTest,
      public testing::WithParamInterface<database_api::DatabaseDialect> {
 public:
  UuidApiTest() : feature_flags_({.enable_postgresql_interface = true}) {}

  void SetUp() override {
    dialect_ = GetParam();
    DatabaseTest::SetUp();
  }

  absl::Status SetUpDatabase() override {
    if (dialect_ == database_api::DatabaseDialect::POSTGRESQL) {
      return SetSchema(
          {"CREATE TABLE uuid_values (id uuid PRIMARY KEY, payload uuid)"});
    }
    return SetSchema({"CREATE TABLE uuid_values (id UUID NOT NULL, "
                      "payload UUID) PRIMARY KEY (id)"});
  }

 protected:
  absl::StatusOr<std::string> CreateSession() {
    spanner_api::CreateSessionRequest request;
    request.set_database(database()->FullName());
    spanner_api::Session response;
    grpc::ClientContext context;
    GOOGLESQL_RETURN_IF_ERROR(
        raw_client()->CreateSession(&context, request, &response));
    return response.name();
  }

  absl::StatusOr<spanner_api::ResultSet> ExecuteSql(const std::string& sql) {
    GOOGLESQL_ASSIGN_OR_RETURN(std::string session, CreateSession());
    spanner_api::ExecuteSqlRequest request;
    request.set_session(session);
    request.set_sql(sql);
    request.mutable_transaction()
        ->mutable_single_use()
        ->mutable_read_only()
        ->set_strong(true);
    spanner_api::ResultSet response;
    grpc::ClientContext context;
    GOOGLESQL_RETURN_IF_ERROR(
        raw_client()->ExecuteSql(&context, request, &response));
    return response;
  }

  absl::StatusOr<spanner_api::ResultSet> ReadUuid(const std::string& table,
                                                 const std::string& id) {
    GOOGLESQL_ASSIGN_OR_RETURN(std::string session, CreateSession());
    spanner_api::ReadRequest request;
    request.set_session(session);
    request.mutable_transaction()
        ->mutable_single_use()
        ->mutable_read_only()
        ->set_strong(true);
    request.set_table(table);
    request.add_columns("id");
    request.add_columns("payload");
    request.mutable_key_set()->add_keys()->add_values()->set_string_value(id);
    spanner_api::ResultSet response;
    grpc::ClientContext context;
    GOOGLESQL_RETURN_IF_ERROR(raw_client()->Read(&context, request, &response));
    return response;
  }

 private:
  ScopedEmulatorFeatureFlagsSetter feature_flags_;
};

INSTANTIATE_TEST_SUITE_P(
    PerDialect, UuidApiTest,
    testing::Values(database_api::DatabaseDialect::GOOGLE_STANDARD_SQL,
                    database_api::DatabaseDialect::POSTGRESQL),
    [](const testing::TestParamInfo<UuidApiTest::ParamType>& info) {
      return database_api::DatabaseDialect_Name(info.param);
    });

TEST_P(UuidApiTest, MutationReadAndQuery) {
  const std::string id = "9a31411b-caca-4ff1-86e9-39fbd2bc3f39";
  const std::string payload = "12345678-1234-4abc-8def-123456789abc";
  GOOGLESQL_ASSERT_OK(Insert("uuid_values", {"id", "payload"}, {id, payload}));

  GOOGLESQL_ASSERT_OK_AND_ASSIGN(spanner_api::ResultSet read_result,
                                 ReadUuid("uuid_values", id));

  const std::string query =
      "SELECT id, payload FROM uuid_values WHERE id = "
      "CAST('9a31411b-caca-4ff1-86e9-39fbd2bc3f39' AS UUID)";
  GOOGLESQL_ASSERT_OK_AND_ASSIGN(spanner_api::ResultSet query_result,
                                 ExecuteSql(query));

  for (const spanner_api::ResultSet* result : {&read_result, &query_result}) {
    ASSERT_EQ(result->metadata().row_type().fields_size(), 2);
    EXPECT_EQ(result->metadata().row_type().fields(0).type().code(),
              spanner_api::TypeCode::UUID);
    EXPECT_EQ(result->metadata().row_type().fields(1).type().code(),
              spanner_api::TypeCode::UUID);
    ASSERT_EQ(result->rows_size(), 1);
    ASSERT_EQ(result->rows(0).values_size(), 2);
    EXPECT_EQ(result->rows(0).values(0).string_value(), id);
    EXPECT_EQ(result->rows(0).values(1).string_value(), payload);
  }
}

TEST_P(UuidApiTest, GenerationFunctions) {
  const bool pg = dialect_ == database_api::DatabaseDialect::POSTGRESQL;
  GOOGLESQL_ASSERT_OK_AND_ASSIGN(
      spanner_api::ResultSet result,
      ExecuteSql(pg ? "SELECT gen_random_uuid()" : "SELECT NEW_UUID()"));
  ASSERT_EQ(result.metadata().row_type().fields_size(), 1);
  EXPECT_EQ(result.metadata().row_type().fields(0).type().code(),
            spanner_api::TypeCode::UUID);
  ASSERT_EQ(result.rows_size(), 1);
  ASSERT_EQ(result.rows(0).values_size(), 1);
  EXPECT_THAT(result.rows(0).values(0).string_value(),
              testing::MatchesRegex(kUuidPattern));

  if (!pg) {
    GOOGLESQL_ASSERT_OK_AND_ASSIGN(result, ExecuteSql("SELECT GENERATE_UUID()"));
    ASSERT_EQ(result.metadata().row_type().fields_size(), 1);
    EXPECT_EQ(result.metadata().row_type().fields(0).type().code(),
              spanner_api::TypeCode::STRING);
    ASSERT_EQ(result.rows_size(), 1);
    ASSERT_EQ(result.rows(0).values_size(), 1);
    EXPECT_THAT(result.rows(0).values(0).string_value(),
                testing::MatchesRegex(kUuidPattern));
  }
}

TEST_P(UuidApiTest, PostgreSqlDefaultReturningAndStoredValue) {
  if (dialect_ != database_api::DatabaseDialect::POSTGRESQL) {
    GTEST_SKIP();
  }
  GOOGLESQL_ASSERT_OK(UpdateSchema(
      {"CREATE TABLE generated_uuid_values "
       "(id uuid DEFAULT gen_random_uuid(), payload text, PRIMARY KEY (id))"}));

  GOOGLESQL_ASSERT_OK_AND_ASSIGN(std::string session, CreateSession());
  spanner_api::BeginTransactionRequest begin;
  begin.set_session(session);
  begin.mutable_options()->mutable_read_write();
  spanner_api::Transaction transaction;
  grpc::ClientContext begin_context;
  GOOGLESQL_ASSERT_OK(
      raw_client()->BeginTransaction(&begin_context, begin, &transaction));

  spanner_api::ExecuteSqlRequest insert;
  insert.set_session(session);
  insert.mutable_transaction()->set_id(transaction.id());
  insert.set_sql("INSERT INTO generated_uuid_values (payload) "
                 "VALUES ('stored') RETURNING id");
  insert.set_seqno(1);
  spanner_api::ResultSet returned;
  grpc::ClientContext insert_context;
  GOOGLESQL_ASSERT_OK(raw_client()->ExecuteSql(&insert_context, insert, &returned));
  ASSERT_EQ(returned.metadata().row_type().fields_size(), 1);
  EXPECT_EQ(returned.metadata().row_type().fields(0).type().code(),
            spanner_api::TypeCode::UUID);
  ASSERT_EQ(returned.rows_size(), 1);
  ASSERT_EQ(returned.rows(0).values_size(), 1);
  const std::string id = returned.rows(0).values(0).string_value();
  EXPECT_THAT(id, testing::MatchesRegex(kUuidPattern));

  spanner_api::CommitRequest commit;
  commit.set_session(session);
  commit.set_transaction_id(transaction.id());
  spanner_api::CommitResponse committed;
  grpc::ClientContext commit_context;
  GOOGLESQL_ASSERT_OK(raw_client()->Commit(&commit_context, commit, &committed));

  GOOGLESQL_ASSERT_OK_AND_ASSIGN(spanner_api::ResultSet read_result,
                                 ReadUuid("generated_uuid_values", id));
  GOOGLESQL_ASSERT_OK_AND_ASSIGN(
      spanner_api::ResultSet query_result,
      ExecuteSql("SELECT id, payload FROM generated_uuid_values "
                 "WHERE id = CAST('" + id + "' AS uuid)"));
  for (const spanner_api::ResultSet* result : {&read_result, &query_result}) {
    ASSERT_EQ(result->metadata().row_type().fields_size(), 2);
    EXPECT_EQ(result->metadata().row_type().fields(0).type().code(),
              spanner_api::TypeCode::UUID);
    ASSERT_EQ(result->rows_size(), 1);
    ASSERT_EQ(result->rows(0).values_size(), 2);
    EXPECT_EQ(result->rows(0).values(0).string_value(), id);
    EXPECT_EQ(result->rows(0).values(1).string_value(), "stored");
  }
}

}  // namespace
}  // namespace test
}  // namespace emulator
}  // namespace spanner
}  // namespace google

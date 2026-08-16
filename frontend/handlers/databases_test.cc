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

#include <chrono>
#include <filesystem>
#include <memory>
#include <string>
#include <vector>

#include "absl/flags/declare.h"
#include "absl/flags/flag.h"
#include "absl/status/status.h"
#include "absl/strings/str_cat.h"
#include "absl/synchronization/mutex.h"
#include "backend/database/database.h"
#include "backend/schema/catalog/sequence.h"
#include "backend/schema/printer/print_ddl.h"
#include "common/constants.h"
#include "common/limits.h"
#include "frontend/collections/database_manager.h"
#include "frontend/common/uris.h"
#include "gmock/gmock.h"
#include "google/protobuf/any.pb.h"
#include "google/protobuf/descriptor.pb.h"
#include "google/spanner/admin/database/v1/spanner_database_admin.pb.h"
#include "google/spanner/v1/commit_response.pb.h"
#include "googlesql/base/testing/status_matchers.h"
#include "gtest/gtest.h"
#include "tests/common/proto_matchers.h"
#include "tests/common/scoped_feature_flags_setter.h"
#include "tests/common/test_env.h"

ABSL_DECLARE_FLAG(std::string, data_dir);

namespace google {
namespace spanner {
namespace emulator {
namespace frontend {

namespace {

using google::spanner::emulator::backend::Sequence;
using googlesql_base::testing::StatusIs;

namespace database_api = ::google::spanner::admin::database::v1;
namespace instance_api = ::google::spanner::admin::instance::v1;
namespace operations_api = ::google::longrunning;
namespace protobuf_api = ::google::protobuf;
namespace spanner_api = ::google::spanner::v1;

class PersistentDatabaseDataDirectory {
 public:
  PersistentDatabaseDataDirectory()
      : previous_(absl::GetFlag(FLAGS_data_dir)),
        path_((std::filesystem::temp_directory_path() /
               absl::StrCat(
                   "spanner-database-handler-",
                   std::chrono::steady_clock::now().time_since_epoch().count()))
                  .string()) {
    std::filesystem::create_directories(path_);
    absl::SetFlag(&FLAGS_data_dir, path_);
  }

  ~PersistentDatabaseDataDirectory() {
    absl::SetFlag(&FLAGS_data_dir, previous_);
    std::error_code ignored;
    std::filesystem::remove_all(path_, ignored);
  }

  const std::string& path() const { return path_; }

 private:
  std::string previous_;
  std::string path_;
};

class DatabaseApiTest : public test::ServerTest {
 protected:
  void SetUp() override { GOOGLESQL_EXPECT_OK(CreateTestInstance()); }
  void TearDown() override { GOOGLESQL_EXPECT_OK(CleanupTestInstance()); }

  // Generate proto descriptor bytes as string
  std::string GenerateProtoDescriptorBytesAsString() {
    const google::protobuf::FileDescriptorProto file_descriptor =
        PARSE_TEXT_PROTO(R"pb(
          syntax: "proto2"
          name: "0"
          package: "customer.app"
          message_type { name: "User" }
          enum_type {
            name: "State"
            value { name: "UNSPECIFIED" number: 0 }
          }
        )pb");
    google::protobuf::FileDescriptorSet file_descriptor_set;
    *file_descriptor_set.add_file() = file_descriptor;
    return file_descriptor_set.SerializeAsString();
  }

  absl::Status ListDatabases(const std::string& instance_uri, int32_t page_size,
                             const std::string& page_token,
                             database_api::ListDatabasesResponse* response) {
    grpc::ClientContext context;
    database_api::ListDatabasesRequest request;
    request.set_parent(instance_uri);
    request.set_page_size(page_size);
    request.set_page_token(page_token);
    return test_env()->database_admin_client()->ListDatabases(&context, request,
                                                              response);
  }

  absl::Status CreateDatabase(
      const std::string& instance_uri, const std::string& database_name,
      const std::vector<std::string>& extra_statements = {},
      const database_api::DatabaseDialect& dialect =
          // DATABASE_DIALECT_UNSPECIFIED defaults to creating a
          // database with the GOOGLE_STANDARD_SQL dialect.
      database_api::DatabaseDialect::DATABASE_DIALECT_UNSPECIFIED) {
    grpc::ClientContext context;
    database_api::CreateDatabaseRequest request;
    request.set_parent(instance_uri);
    std::string quote =
        (dialect == database_api::POSTGRESQL) ? kPGQuote : kGSQLQuote;
    request.set_create_statement(
        absl::StrCat("CREATE DATABASE ", quote, database_name, quote));
    for (auto extra_statement : extra_statements) {
      request.add_extra_statements(extra_statement);
    }
    request.set_proto_descriptors(GenerateProtoDescriptorBytesAsString());
    request.set_database_dialect(dialect);
    operations_api::Operation operation;
    GOOGLESQL_RETURN_IF_ERROR(
        test_env()->database_admin_client()->CreateDatabase(&context, request,
                                                            &operation));
    GOOGLESQL_RETURN_IF_ERROR(WaitForOperation(operation.name(), &operation));
    database_api::CreateDatabaseMetadata metadata;
    GOOGLESQL_RET_CHECK(operation.metadata().UnpackTo(&metadata));
    GOOGLESQL_RET_CHECK_EQ(metadata.database(),
                           MakeDatabaseUri(instance_uri, database_name));
    return absl::OkStatus();
  }

  absl::Status GetDatabase(const std::string& database_uri,
                           database_api::Database* database) {
    grpc::ClientContext context;
    database_api::GetDatabaseRequest request;
    request.set_name(database_uri);
    return test_env()->database_admin_client()->GetDatabase(&context, request,
                                                            database);
  }

  absl::Status GetDatabaseDdl(const std::string& database_uri,
                              database_api::GetDatabaseDdlResponse* response) {
    grpc::ClientContext context;
    database_api::GetDatabaseDdlRequest request;
    request.set_database(database_uri);
    GOOGLESQL_RETURN_IF_ERROR(
        test_env()->database_admin_client()->GetDatabaseDdl(&context, request,
                                                            response));
    return absl::OkStatus();
  }

  absl::Status DropDatabase(const std::string& database_uri) {
    grpc::ClientContext context;
    database_api::DropDatabaseRequest request;
    request.set_database(database_uri);
    protobuf_api::Empty response;
    return test_env()->database_admin_client()->DropDatabase(&context, request,
                                                             &response);
  }

 private:
  absl::Status CleanupTestInstance() {
    database_api::ListDatabasesResponse response;
    GOOGLESQL_RETURN_IF_ERROR(ListDatabases(test_instance_uri_, 0 /*page_size*/,
                                            "" /*page_token*/, &response));
    while (!response.databases().empty()) {
      for (const auto& database : response.databases()) {
        GOOGLESQL_RETURN_IF_ERROR(DropDatabase(database.name()));
      }
      response.clear_databases();
      if (!response.next_page_token().empty()) {
        std::string next_page_token = response.next_page_token();
        response.clear_next_page_token();
        GOOGLESQL_RETURN_IF_ERROR(ListDatabases(
            test_instance_uri_, 0 /*page_size*/, next_page_token, &response));
      }
    }
    return absl::OkStatus();
  }
};

class PersistentDatabaseDdlTest : public ::testing::Test {
 protected:
  void SetUp() override {
    instance_api::CreateInstanceRequest instance_request;
    instance_request.set_parent(project_name_);
    instance_request.set_instance_id("instance");
    instance_request.mutable_instance()->set_config("emulator-config");
    instance_request.mutable_instance()->set_node_count(1);
    operations_api::Operation instance_operation;
    grpc::ClientContext instance_context;
    ASSERT_TRUE(env_.instance_admin_client()
                    ->CreateInstance(&instance_context, instance_request,
                                     &instance_operation)
                    .ok());
    ASSERT_TRUE(instance_operation.done());

    database_api::CreateDatabaseRequest database_request;
    database_request.set_parent(instance_name_);
    database_request.set_create_statement("CREATE DATABASE `database`");
    database_request.add_extra_statements(
        "CREATE TABLE T (K INT64) PRIMARY KEY (K)");
    operations_api::Operation database_operation;
    grpc::ClientContext database_context;
    ASSERT_TRUE(env_.database_admin_client()
                    ->CreateDatabase(&database_context, database_request,
                                     &database_operation)
                    .ok());
    ASSERT_TRUE(database_operation.done());
  }

  database_api::GetDatabaseDdlResponse GetDdl() {
    database_api::GetDatabaseDdlRequest request;
    request.set_database(database_name_);
    database_api::GetDatabaseDdlResponse response;
    grpc::ClientContext context;
    EXPECT_TRUE(
        env_.database_admin_client()->GetDatabaseDdl(&context, request, &response)
            .ok());
    return response;
  }

  PersistentDatabaseDataDirectory data_dir_;
  test::TestEnv env_;
  const std::string project_name_ = "projects/ddl-persistence";
  const std::string instance_name_ = project_name_ + "/instances/instance";
  const std::string database_name_ =
      instance_name_ + "/databases/database";
};

TEST_F(PersistentDatabaseDdlTest,
       IntentSaveFailureLeavesSchemaUnchangedAndAllowsRetry) {
  database_api::UpdateDatabaseDdlRequest request;
  request.set_database(database_name_);
  request.set_operation_id("ddljournal");
  request.add_statements(
      "ALTER TABLE T ADD COLUMN ProtoValue STRING(MAX)");

  const std::filesystem::path blocked_temporary_metadata =
      std::filesystem::path(data_dir_.path()) / "metadata.json.tmp";
  ASSERT_TRUE(
      std::filesystem::create_directory(blocked_temporary_metadata));

  operations_api::Operation rejected_operation;
  grpc::ClientContext rejected_context;
  grpc::Status status = env_.database_admin_client()->UpdateDatabaseDdl(
      &rejected_context, request, &rejected_operation);
  EXPECT_EQ(status.error_code(), grpc::StatusCode::FAILED_PRECONDITION);

  database_api::GetDatabaseDdlResponse unchanged = GetDdl();
  ASSERT_EQ(unchanged.statements_size(), 1);
  EXPECT_THAT(unchanged.statements(0), testing::Not(testing::HasSubstr(
                                           "ProtoValue")));

  ASSERT_TRUE(std::filesystem::remove(blocked_temporary_metadata));
  operations_api::Operation retried_operation;
  grpc::ClientContext retry_context;
  status = env_.database_admin_client()->UpdateDatabaseDdl(
      &retry_context, request, &retried_operation);
  ASSERT_TRUE(status.ok()) << status.error_message();
  EXPECT_TRUE(retried_operation.done());

  database_api::GetDatabaseDdlResponse updated = GetDdl();
  ASSERT_EQ(updated.statements_size(), 1);
  EXPECT_THAT(updated.statements(0), testing::HasSubstr("ProtoValue"));
}

TEST_F(PersistentDatabaseDdlTest,
       PromotionSaveFailureQuarantinesCommittedSchemaUntilRestart) {
  GOOGLESQL_ASSERT_OK_AND_ASSIGN(
      std::shared_ptr<Database> database,
      env_.server()->env()->database_manager()->GetDatabase(database_name_));
  GOOGLESQL_ASSERT_OK_AND_ASSIGN(
      std::unique_ptr<backend::ReadWriteTransaction> preexisting_transaction,
      database->backend()->CreateReadWriteTransaction(
          backend::ReadWriteOptions(), backend::RetryState()));

  int save_calls = 0;
  env_.server()->env()->metadata_store()->SetSaveHookForTesting([&] {
    ++save_calls;
    return save_calls == 2
               ? absl::InternalError("injected promotion save failure")
               : absl::OkStatus();
  });

  database_api::UpdateDatabaseDdlRequest request;
  request.set_database(database_name_);
  request.set_operation_id("quarantinedddl");
  request.add_statements(
      "ALTER TABLE T ADD COLUMN QuarantinedValue STRING(MAX)");
  operations_api::Operation operation;
  grpc::ClientContext context;
  grpc::Status status = env_.database_admin_client()->UpdateDatabaseDdl(
      &context, request, &operation);

  ASSERT_TRUE(status.ok()) << status.error_message();
  EXPECT_TRUE(operation.done());
  EXPECT_EQ(save_calls, 2);
  EXPECT_TRUE(database->backend()->restore_required());
  EXPECT_THAT(env_.server()->env()->database_manager()->GetDatabase(
                  database_name_),
              StatusIs(absl::StatusCode::kFailedPrecondition));
  backend::Mutation mutation;
  mutation.AddWriteOp(backend::MutationOpType::kInsert, "T", {"K"},
                      {{googlesql::values::Int64(1)}});
  EXPECT_THAT(preexisting_transaction->Write(mutation),
              StatusIs(absl::StatusCode::kFailedPrecondition));

  database->set_enable_drop_protection(true);
  database_api::DropDatabaseRequest drop_request;
  drop_request.set_database(database_name_);
  protobuf_api::Empty drop_response;
  grpc::ClientContext drop_context;
  EXPECT_EQ(env_.database_admin_client()
                ->DropDatabase(&drop_context, drop_request, &drop_response)
                .error_code(),
            grpc::StatusCode::FAILED_PRECONDITION);

  env_.server()->env()->metadata_store()->SetSaveHookForTesting({});
  MetadataStore disk_state(data_dir_.path());
  GOOGLESQL_ASSERT_OK(disk_state.Load());
  EXPECT_TRUE(disk_state.AllPendingDdlOperations().contains(database_name_));
}

// Tests for CreateDatabase.

TEST_F(DatabaseApiTest, CreateDatabaseWithInvalidDatabaseName) {
  // Name less than 2 characters.
  EXPECT_THAT(CreateDatabase(test_instance_uri_, "a"),
              StatusIs(absl::StatusCode::kInvalidArgument));

  // Name greater than 30 characters.
  EXPECT_THAT(CreateDatabase(test_instance_uri_,
                             "aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa"),
              StatusIs(absl::StatusCode::kInvalidArgument));

  // Only lowercase allowed.
  EXPECT_THAT(CreateDatabase(test_instance_uri_, "AAAAA"),
              StatusIs(absl::StatusCode::kInvalidArgument));

  // Non-alphanumeric characters are not allowed.
  EXPECT_THAT(CreateDatabase(test_instance_uri_, "aaaa!@#$aaa"),
              StatusIs(absl::StatusCode::kInvalidArgument));

  // Cannot end with hypen.
  EXPECT_THAT(CreateDatabase(test_instance_uri_, "aaaa-aaaa-"),
              StatusIs(absl::StatusCode::kInvalidArgument));
}

TEST_F(DatabaseApiTest, LimitDatabasePerInstance) {
  for (int i = 0; i < limits::kMaxDatabasesPerInstance; ++i) {
    GOOGLESQL_EXPECT_OK(CreateDatabase(test_instance_uri_,
                                       absl::StrCat(test_database_name_, i)));
  }
  // Creating the next database fails.
  EXPECT_THAT(CreateDatabase(test_instance_uri_, test_database_name_),
              StatusIs(absl::StatusCode::kResourceExhausted));
}

TEST_F(DatabaseApiTest, OverrideMaxDatabasePerInstanceLimit) {
  int custom_max_dbs_per_instance = 150;
  absl::SetFlag(&FLAGS_override_max_databases_per_instance,
                custom_max_dbs_per_instance);
  for (int i = 0; i < custom_max_dbs_per_instance; ++i) {
    GOOGLESQL_EXPECT_OK(CreateDatabase(test_instance_uri_,
                                       absl::StrCat(test_database_name_, i)));
  }
  // Creating the next database fails.
  EXPECT_THAT(CreateDatabase(test_instance_uri_, test_database_name_),
              StatusIs(absl::StatusCode::kResourceExhausted));
}

TEST_F(DatabaseApiTest, CreateDatabaseEmptyInitialSchema) {
  GOOGLESQL_EXPECT_OK(CreateDatabase(test_instance_uri_, test_database_name_));
}

TEST_F(DatabaseApiTest, CreateDatabaseWithInitialSchema) {
  GOOGLESQL_EXPECT_OK(CreateDatabase(test_instance_uri_, test_database_name_,
                                     {
                                         R"(
                                 CREATE TABLE test_table (
                                   int64_col INT64 NOT NULL,
                                   string_col STRING(MAX)
                                 ) PRIMARY KEY(int64_col)
                               )",
                                     }));
}

TEST_F(DatabaseApiTest, CreateDatabaseWithInvalidInitialSchema) {
  EXPECT_THAT(
      CreateDatabase(test_instance_uri_, test_database_name_, {"INVALID DDL"}),
      StatusIs(absl::StatusCode::kInvalidArgument));
}

TEST_F(DatabaseApiTest, CreateDuplicateDatabaseReturnsAlreadyExists) {
  GOOGLESQL_EXPECT_OK(CreateDatabase(test_instance_uri_, test_database_name_));

  // Request to create same database.
  EXPECT_THAT(CreateDatabase(test_instance_uri_, test_database_name_),
              StatusIs(absl::StatusCode::kAlreadyExists));
}

TEST_F(DatabaseApiTest, CreateDatabaseWithoutInstanceReturnsNotFound) {
  EXPECT_THAT(CreateDatabase(
                  MakeInstanceUri(test_project_name_, "non-existent-instance"),
                  test_database_name_),
              StatusIs(absl::StatusCode::kNotFound));
}

TEST_F(DatabaseApiTest, CreateDatabaseWithGSQLDialect) {
  GOOGLESQL_EXPECT_OK(CreateDatabase(
      test_instance_uri_, absl::StrCat(test_database_name_, "_gsql"),
      /* extra_statements = */ {},
      database_api::DatabaseDialect::GOOGLE_STANDARD_SQL));
}

TEST_F(DatabaseApiTest, CreateDatabaseWithPostgresDialectDisabledByDefault) {
  GOOGLESQL_EXPECT_OK(CreateDatabase(
      test_instance_uri_, absl::StrCat(test_database_name_, "_pg_1"),
      /* extra_statements = */ {}, database_api::DatabaseDialect::POSTGRESQL));
}

TEST_F(DatabaseApiTest, CreateDatabaseWithPostgresDialect) {
  test::ScopedEmulatorFeatureFlagsSetter enabled_flags(
      {.enable_postgresql_interface = true});
  GOOGLESQL_EXPECT_OK(CreateDatabase(
      test_instance_uri_, absl::StrCat(test_database_name_, "_pg_1"),
      /* extra_statements = */ {}, database_api::DatabaseDialect::POSTGRESQL));

  GOOGLESQL_EXPECT_OK(CreateDatabase(
      test_instance_uri_, absl::StrCat(test_database_name_, "_pg_2"),
      {
          R"(
                                   CREATE TABLE test_table (
                                     int64_col bigint not null primary key,
                                     string_col varchar(1024)
                                   )
                                 )",
      },
      database_api::DatabaseDialect::POSTGRESQL));
}

// Tests for ListDatabases.
TEST_F(DatabaseApiTest, DoesNotListDatabasesForUnknownInstance) {
  database_api::ListDatabasesResponse response;
  EXPECT_THAT(ListDatabases(
                  MakeInstanceUri(test_project_name_, "non-existent-instance"),
                  0 /*page_size*/, "" /*page_token*/, &response),
              StatusIs(absl::StatusCode::kNotFound));
}

TEST_F(DatabaseApiTest, ListsEmptyDatabasesForNewInstance) {
  database_api::ListDatabasesResponse response;
  GOOGLESQL_EXPECT_OK(ListDatabases(test_instance_uri_, 0 /*page_size*/,
                                    "" /*page_token*/, &response));
  EXPECT_EQ(response.databases_size(), 0);
}

TEST_F(DatabaseApiTest, ListsCustomPageSizeDatabases) {
  // Create 5+3 = 8 databases test-database0, test-database1,...,
  // test-database7 in test-instance.
  int32_t page_size = 5;
  int32_t last_page_size = 3;
  for (int i = 0; i < page_size + last_page_size; i++) {
    GOOGLESQL_EXPECT_OK(CreateDatabase(test_instance_uri_,
                                       absl::StrCat(test_database_name_, i)));
  }

  // List databases from test-instance with page_size being 5, i.e., only 5
  // databases test-database0, test-database1,..., test-database4 should be
  // returned with next_page_token pointing to test-database5.
  database_api::ListDatabasesResponse response;
  GOOGLESQL_EXPECT_OK(ListDatabases(test_instance_uri_, page_size,
                                    "" /*page_token*/, &response));
  EXPECT_EQ(response.databases_size(), page_size);
  for (int i = 0; i < page_size; i++) {
    EXPECT_EQ(response.databases(i).name(),
              absl::StrCat(test_database_uri_, i));
  }
  EXPECT_EQ(response.next_page_token(),
            absl::StrCat(test_database_uri_, page_size));

  // Using the next_page_token pointing to test-database5, list next at most 5
  // databases test-database5, test-database6 and test-database7.
  database_api::ListDatabasesResponse response2;
  GOOGLESQL_EXPECT_OK(ListDatabases(test_instance_uri_, page_size,
                                    response.next_page_token(), &response2));
  EXPECT_EQ(response2.databases_size(), last_page_size);
  for (int i = 0; i < last_page_size; i++) {
    EXPECT_EQ(response2.databases(i).name(),
              absl::StrCat(test_database_uri_, i + page_size));
  }
  // No more databases left to be returned and thus next_page_token is not set.
  EXPECT_EQ(response2.next_page_token(), "");
}

// Tests for UpdateDatabaseDdl.

TEST_F(DatabaseApiTest, UpdateDatabaseDdlInvalidDatabaseUri) {
  GOOGLESQL_EXPECT_OK(CreateDatabase(test_instance_uri_, test_database_name_));

  // Invalid project name should return an error.
  auto instance_uri =
      MakeInstanceUri("non-existent-project", test_instance_name_);
  EXPECT_THAT(
      UpdateDatabaseDdl(MakeDatabaseUri(instance_uri, test_database_name_), {}),
      StatusIs(absl::StatusCode::kNotFound));

  // Invalid instance name should return an error.
  instance_uri = MakeInstanceUri(test_project_name_, "non-existent-instance");
  EXPECT_THAT(
      UpdateDatabaseDdl(MakeDatabaseUri(instance_uri, test_database_name_), {}),
      StatusIs(absl::StatusCode::kNotFound));

  // Invalid database name should return an error.
  EXPECT_THAT(
      UpdateDatabaseDdl(
          MakeDatabaseUri(test_instance_uri_, "non-existent-database"), {}),
      StatusIs(absl::StatusCode::kNotFound));
}

TEST_F(DatabaseApiTest, UpdateDatabaseDdlPartialSuccess) {
  GOOGLESQL_EXPECT_OK(CreateTestDatabase());
  GOOGLESQL_ASSERT_OK_AND_ASSIGN(const std::string session,
                                 CreateTestSession(/*multiplexed=*/false));

  spanner_api::CommitRequest commit_request = PARSE_TEXT_PROTO(R"pb(
    single_use_transaction { read_write {} }
    mutations {
      insert {
        table: "test_table"
        columns: "int64_col"
        columns: "string_col"
        values {
          values { string_value: "1" }
          values { string_value: "a" }
        }
        values {
          values { string_value: "2" }
          values { string_value: "a" }
        }
      }
    }
  )pb");
  *commit_request.mutable_session() = session;
  spanner_api::CommitResponse commit_response;
  GOOGLESQL_ASSERT_OK(Commit(commit_request, &commit_response));

  database_api::UpdateDatabaseDdlMetadata metadata;
  std::vector<std::string> statements = {R"(
     CREATE TABLE another_table (
       int64_col INT64 NOT NULL,
     ) PRIMARY KEY (int64_col)
  )",
                                         R"(
     CREATE UNIQUE INDEX test_index ON test_table(string_col)
  )"};

  EXPECT_THAT(UpdateDatabaseDdl(test_database_uri_, statements, &metadata),
              StatusIs(absl::StatusCode::kFailedPrecondition,
                       testing::HasSubstr(
                           "Found uniqueness violation on index test_index")));

  EXPECT_EQ(metadata.commit_timestamps_size(), 1);
  EXPECT_EQ(metadata.statements_size(), 2);
  for (int i = 0; i < metadata.statements_size(); ++i) {
    EXPECT_EQ(metadata.statements(i), statements[i]);
  }
}

TEST_F(DatabaseApiTest, GetDatabaseNonExistentDatabase) {
  database_api::Database database;
  EXPECT_THAT(GetDatabase(test_database_uri_, &database),
              StatusIs(absl::StatusCode::kNotFound));
}

TEST_F(DatabaseApiTest, GetDatabase) {
  GOOGLESQL_EXPECT_OK(CreateDatabase(test_instance_uri_, test_database_name_));

  database_api::Database database;
  GOOGLESQL_EXPECT_OK(GetDatabase(test_database_uri_, &database));
  EXPECT_EQ(database.name(), test_database_uri_);
  EXPECT_EQ(database.database_dialect(),
            database_api::DatabaseDialect::GOOGLE_STANDARD_SQL);
}

TEST_F(DatabaseApiTest, GetDatabaseWithGSQLDialect) {
  GOOGLESQL_EXPECT_OK(
      CreateDatabase(test_instance_uri_, test_database_name_, {},
                     database_api::DatabaseDialect::GOOGLE_STANDARD_SQL));

  database_api::Database database;
  GOOGLESQL_EXPECT_OK(GetDatabase(test_database_uri_, &database));
  EXPECT_EQ(database.name(), test_database_uri_);
  EXPECT_EQ(database.database_dialect(),
            database_api::DatabaseDialect::GOOGLE_STANDARD_SQL);
}

TEST_F(DatabaseApiTest, GetDatabaseWithPostgresDialect) {
  GOOGLESQL_EXPECT_OK(
      CreateDatabase(test_instance_uri_, test_database_name_, {},
                     database_api::DatabaseDialect::POSTGRESQL));

  database_api::Database database;
  GOOGLESQL_EXPECT_OK(GetDatabase(test_database_uri_, &database));
  EXPECT_EQ(database.name(), test_database_uri_);
  EXPECT_EQ(database.database_dialect(),
            database_api::DatabaseDialect::POSTGRESQL);
}

TEST_F(DatabaseApiTest, DropDatabaseInvalidInstance) {
  auto instance_uri =
      MakeInstanceUri(test_project_name_, "invalid-instance-name");
  EXPECT_THAT(DropDatabase(MakeDatabaseUri(instance_uri, test_database_name_)),
              StatusIs(absl::StatusCode::kNotFound));
}

TEST_F(DatabaseApiTest, DropDatabase) {
  GOOGLESQL_EXPECT_OK(CreateDatabase(test_instance_uri_, test_database_name_));

  database_api::Database database;
  GOOGLESQL_EXPECT_OK(GetDatabase(test_database_uri_, &database));

  GOOGLESQL_EXPECT_OK(DropDatabase(test_database_uri_));

  EXPECT_THAT(GetDatabase(test_database_uri_, &database),
              StatusIs(absl::StatusCode::kNotFound));
}

TEST_F(DatabaseApiTest, DropProtectionIsUpdatedAndEnforced) {
  GOOGLESQL_EXPECT_OK(CreateDatabase(test_instance_uri_, test_database_name_));

  database_api::UpdateDatabaseRequest enable;
  enable.mutable_database()->set_name(test_database_uri_);
  enable.mutable_database()->set_enable_drop_protection(true);
  enable.mutable_update_mask()->add_paths("enable_drop_protection");
  operations_api::Operation operation;
  grpc::ClientContext enable_context;
  GOOGLESQL_EXPECT_OK(test_env()->database_admin_client()->UpdateDatabase(
      &enable_context, enable, &operation));

  database_api::Database database;
  GOOGLESQL_EXPECT_OK(GetDatabase(test_database_uri_, &database));
  EXPECT_TRUE(database.enable_drop_protection());
  EXPECT_THAT(DropDatabase(test_database_uri_),
              StatusIs(absl::StatusCode::kFailedPrecondition));

  database_api::UpdateDatabaseRequest disable;
  disable.mutable_database()->set_name(test_database_uri_);
  disable.mutable_database()->set_enable_drop_protection(false);
  disable.mutable_update_mask()->add_paths("enable_drop_protection");
  grpc::ClientContext disable_context;
  GOOGLESQL_EXPECT_OK(test_env()->database_admin_client()->UpdateDatabase(
      &disable_context, disable, &operation));
  GOOGLESQL_EXPECT_OK(DropDatabase(test_database_uri_));
}

TEST_F(DatabaseApiTest, DropDatabaseIdempotent) {
  database_api::Database database;
  EXPECT_THAT(GetDatabase(test_database_uri_, &database),
              StatusIs(absl::StatusCode::kNotFound));

  GOOGLESQL_EXPECT_OK(DropDatabase(test_database_uri_));
}

TEST_F(DatabaseApiTest, UpdateAndGetDatabaseDDL) {
  std::vector<std::vector<std::string>> test_schemas = {
      {
          R"(CREATE TABLE test_table (
  int64_col INT64 NOT NULL,
  string_col STRING(MAX),
  ts TIMESTAMP OPTIONS (
    allow_commit_timestamp = true
  ),
) PRIMARY KEY(int64_col))"},
      {
          R"(CREATE TABLE test_table (
  int64_col INT64 NOT NULL,
  string_col STRING(MAX),
) PRIMARY KEY(int64_col))",
          R"(CREATE UNIQUE NULL_FILTERED INDEX test_index ON test_table(string_col))",
      },
      {
          R"(CREATE TABLE test_table (
  int64_col INT64 NOT NULL,
  string_col STRING(MAX),
) PRIMARY KEY(int64_col))"},
      {
          R"(CREATE TABLE test_table (
) PRIMARY KEY())"},
      {
          R"sql(CREATE PROTO BUNDLE (
  customer.app.State,
  customer.app.User,
))sql",
          R"sql(CREATE TABLE test_table (
  int64_col INT64 NOT NULL,
  string_col STRING(MAX),
  user_col `customer.app.User`,
  state_col `customer.app.State`,
) PRIMARY KEY(int64_col))sql"},
  };

  for (int i = 0; i < test_schemas.size(); ++i) {
    auto schema = test_schemas[i];
    GOOGLESQL_EXPECT_OK(
        CreateDatabase(test_instance_uri_, test_database_name_, schema));

    database_api::GetDatabaseDdlResponse response;
    GOOGLESQL_EXPECT_OK(GetDatabaseDdl(test_database_uri_, &response));

    for (int j = 0; j < schema.size(); ++j) {
      EXPECT_THAT(response.statements(j), schema[j]);
    }
    EXPECT_EQ(response.proto_descriptors(),
              i == test_schemas.size() - 1
                  ? GenerateProtoDescriptorBytesAsString()
                  : "");

    GOOGLESQL_EXPECT_OK(
        DropDatabase(MakeDatabaseUri(test_instance_uri_, test_database_name_)));
  }
}

TEST_F(DatabaseApiTest, GetDatabaseDdlIncludesIndexAddedByUpdateDdl) {
  std::vector<std::string> schema = {
      R"(CREATE TABLE test_table (
  int64_col INT64 NOT NULL,
  string_col STRING(MAX),
) PRIMARY KEY(int64_col))"};
  GOOGLESQL_EXPECT_OK(
      CreateDatabase(test_instance_uri_, test_database_name_, schema));

  GOOGLESQL_EXPECT_OK(UpdateDatabaseDdl(
      test_database_uri_,
      {R"(CREATE INDEX test_index ON test_table(string_col))"}));

  database_api::GetDatabaseDdlResponse response;
  GOOGLESQL_EXPECT_OK(GetDatabaseDdl(test_database_uri_, &response));

  ASSERT_EQ(response.statements_size(), 2);
  EXPECT_EQ(response.statements(0), schema[0]);
  EXPECT_EQ(response.statements(1),
            R"(CREATE INDEX test_index ON test_table(string_col))");
}

spanner_api::CommitRequest GenerateSequenceTableInsert(
    const std::string& session) {
  spanner_api::CommitRequest commit_request = PARSE_TEXT_PROTO(R"pb(
    single_use_transaction { read_write {} }
    mutations {
      insert {
        table: "test_table"
        columns: "col"
        values { values { string_value: "1" } }
        values { values { string_value: "2" } }
      }
    }
  )pb");
  *commit_request.mutable_session() = session;
  return commit_request;
}

TEST_F(DatabaseApiTest, CreateSameNameSequencesInTwoDatabases) {
  std::vector<std::string> schema = {
      R"(CREATE SEQUENCE test_sequence OPTIONS (
  sequence_kind = 'bit_reversed_positive'))",
      R"(CREATE TABLE test_table (
  id INT64 DEFAULT (GET_NEXT_SEQUENCE_VALUE(SEQUENCE test_sequence)),
  col INT64,
) PRIMARY KEY(id))"};
  GOOGLESQL_EXPECT_OK(CreateDatabase(test_instance_uri_, "test_db_1", schema));
  std::string database_uri_1 = MakeDatabaseUri(test_instance_uri_, "test_db_1");

  database_api::GetDatabaseDdlResponse response;
  GOOGLESQL_EXPECT_OK(GetDatabaseDdl(database_uri_1, &response));

  for (int i = 0; i < schema.size(); ++i) {
    EXPECT_THAT(response.statements(i), schema[i]);
  }

  GOOGLESQL_EXPECT_OK(CreateDatabase(test_instance_uri_, "test_db_2", schema));
  std::string database_uri_2 = MakeDatabaseUri(test_instance_uri_, "test_db_2");
  GOOGLESQL_EXPECT_OK(GetDatabaseDdl(database_uri_2, &response));

  for (int i = 0; i < schema.size(); ++i) {
    EXPECT_THAT(response.statements(i), schema[i]);
  }

  spanner_api::CommitResponse commit_response;
  GOOGLESQL_ASSERT_OK_AND_ASSIGN(
      const std::string session_1,
      CreateTestSession(/*multiplexed=*/false, database_uri_1));
  GOOGLESQL_ASSERT_OK(
      Commit(GenerateSequenceTableInsert(session_1), &commit_response));

  GOOGLESQL_ASSERT_OK_AND_ASSIGN(
      const std::string session_2,
      CreateTestSession(/*multiplexed=*/false, database_uri_2));
  GOOGLESQL_ASSERT_OK(
      Commit(GenerateSequenceTableInsert(session_2), &commit_response));

  {
    absl::MutexLock lock(Sequence::SequenceMutex);
    ASSERT_EQ(Sequence::SequenceLastValues.size(), 2);
  }
}

}  // namespace

}  // namespace frontend
}  // namespace emulator
}  // namespace spanner
}  // namespace google

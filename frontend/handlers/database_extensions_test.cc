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

#include "absl/status/status.h"
#include "absl/status/statusor.h"
#include "absl/time/clock.h"
#include "absl/time/time.h"
#include "backend/database/database.h"
#include "backend/stats/system_stats_collector.h"
#include "frontend/collections/operation_manager.h"
#include "frontend/entities/database.h"
#include "googlesql/base/testing/status_matchers.h"
#include "google/longrunning/operations.pb.h"
#include "google/protobuf/message.h"
#include "google/protobuf/text_format.h"
#include "google/spanner/admin/database/v1/backup.pb.h"
#include "google/spanner/admin/database/v1/spanner_database_admin.pb.h"
#include "grpcpp/client_context.h"
#include "gtest/gtest.h"
#include "tests/common/proto_matchers.h"
#include "tests/common/test_env.h"

namespace google {
namespace spanner {
namespace emulator {
namespace frontend {
namespace {

namespace database_api = ::google::spanner::admin::database::v1;
namespace operations_api = ::google::longrunning;

class DatabaseExtensionsTest : public test::ServerTest {
 protected:
  void SetUp() override {
    ASSERT_TRUE(CreateTestInstance().ok());
    ASSERT_TRUE(CreateTestDatabase().ok());
  }
};

TEST_F(DatabaseExtensionsTest, AddSplitPointsValidatesDatabase) {
  database_api::AddSplitPointsRequest request;
  database_api::AddSplitPointsResponse response;
  grpc::ClientContext context;
  request.set_database(test_database_uri_);
  grpc::Status status = test_env()->database_admin_client()->AddSplitPoints(
      &context, request, &response);
  EXPECT_TRUE(status.ok()) << status.error_message();

  grpc::ClientContext missing_context;
  request.set_database(test_instance_uri_ + "/databases/missing");
  status = test_env()->database_admin_client()->AddSplitPoints(
      &missing_context, request, &response);
  EXPECT_EQ(status.error_code(), grpc::StatusCode::NOT_FOUND);

  grpc::ClientContext invalid_context;
  request.set_database("not-a-database");
  status = test_env()->database_admin_client()->AddSplitPoints(
      &invalid_context, request, &response);
  EXPECT_EQ(status.error_code(), grpc::StatusCode::INVALID_ARGUMENT);
}

TEST_F(DatabaseExtensionsTest, AddSplitPointsRecordsSplitPoints) {
  GOOGLESQL_ASSERT_OK(UpdateDatabaseDdl(
      test_database_uri_,
      {"CREATE INDEX test_index ON test_table(string_col)"}));
  database_api::AddSplitPointsRequest request = PARSE_TEXT_PROTO(R"pb(
    split_points {
      table: "test_table"
      keys { key_parts { values { string_value: "10" } } }
    }
    split_points {
      table: "test_table"
      index: "test_index"
      keys {
        key_parts {
          values { string_value: "abc" }
          values { string_value: "5" }
        }
      }
      expire_time { seconds: 4102444800 }
    }
  )pb");
  request.set_database(test_database_uri_);
  database_api::AddSplitPointsResponse response;
  grpc::ClientContext context;
  grpc::Status status = test_env()->database_admin_client()->AddSplitPoints(
      &context, request, &response);
  // The expiration time is at most 30 days in the future.
  EXPECT_EQ(status.error_code(), grpc::StatusCode::INVALID_ARGUMENT)
      << status.error_message();

  request.mutable_split_points(1)->clear_expire_time();
  request.set_initiator(std::string(60, 'x'));
  grpc::ClientContext valid_context;
  status = test_env()->database_admin_client()->AddSplitPoints(
      &valid_context, request, &response);
  ASSERT_TRUE(status.ok()) << status.error_message();

  absl::StatusOr<std::shared_ptr<Database>> database =
      test_env()->server()->env()->database_manager()->GetDatabase(
          test_database_uri_);
  ASSERT_TRUE(database.ok()) << database.status();
  std::vector<backend::SpannerSysRow> rows =
      (*database)->backend()->stats_collector()->Snapshot(
          "USER_SPLIT_POINTS", absl::Now(), /*include_open_intervals=*/false,
          (*database)->backend()->GetLatestSchema());
  ASSERT_EQ(rows.size(), 2);
  // Rows are ordered by table, index and split key.
  EXPECT_EQ(rows[0].at("SPLIT_KEY").string_value(), "test_table(10)");
  EXPECT_EQ(rows[0].at("INDEX_NAME").string_value(), "");
  EXPECT_EQ(rows[1].at("SPLIT_KEY").string_value(),
            "Index: test_index on test_table, Index Key: (abc), Primary Table "
            "Key: (5)");
  // Initiators are trimmed to 50 characters, and split points expire in 10
  // days by default.
  EXPECT_EQ(rows[0].at("INITIATOR").string_value(), std::string(50, 'x'));
  EXPECT_NEAR(absl::ToDoubleHours(rows[0].at("EXPIRE_TIME").ToTime() -
                                  absl::Now()),
              240, 1);
}

TEST_F(DatabaseExtensionsTest, AddSplitPointsValidatesSplitPoints) {
  auto add = [&](const std::string& split_points) {
    database_api::AddSplitPointsRequest request;
    request.set_database(test_database_uri_);
    EXPECT_TRUE(google::protobuf::TextFormat::ParseFromString(
        split_points, request.add_split_points()));
    database_api::AddSplitPointsResponse response;
    grpc::ClientContext context;
    return test_env()->database_admin_client()->AddSplitPoints(
        &context, request, &response).error_code();
  };
  EXPECT_EQ(add(R"pb(table: "missing"
                     keys { key_parts { values { string_value: "1" } } })pb"),
            grpc::StatusCode::NOT_FOUND);
  EXPECT_EQ(add(R"pb(table: "test_table"
                     index: "missing"
                     keys { key_parts { values { string_value: "1" } } })pb"),
            grpc::StatusCode::NOT_FOUND);
  EXPECT_EQ(add(R"pb(table: "test_table")pb"),
            grpc::StatusCode::INVALID_ARGUMENT);
  // Too many key parts, and a value of the wrong type, fail like keys of
  // reads and mutations.
  EXPECT_EQ(add(R"pb(table: "test_table"
                     keys {
                       key_parts {
                         values { string_value: "1" }
                         values { string_value: "2" }
                       }
                     })pb"),
            grpc::StatusCode::FAILED_PRECONDITION);
  EXPECT_EQ(add(R"pb(table: "test_table"
                     keys { key_parts { values { bool_value: true } } })pb"),
            grpc::StatusCode::FAILED_PRECONDITION);
}

TEST_F(DatabaseExtensionsTest, InternalGraphUpdateCompletesExistingOperation) {
  auto operation = test_env()->server()->env()->operation_manager()->CreateOperation(
      test_database_uri_, "graph-update");
  ASSERT_TRUE(operation.ok()) << operation.status();

  database_api::InternalUpdateGraphOperationRequest request;
  request.set_database(test_database_uri_);
  request.set_operation_id("graph-update");
  request.mutable_status()->set_code(
      static_cast<int>(grpc::StatusCode::FAILED_PRECONDITION));
  request.mutable_status()->set_message("graph update failed");
  database_api::InternalUpdateGraphOperationResponse response;
  grpc::ClientContext context;
  grpc::Status status =
      test_env()->database_admin_client()->InternalUpdateGraphOperation(
          &context, request, &response);
  ASSERT_TRUE(status.ok()) << status.error_message();

  operations_api::Operation operation_proto;
  (*operation)->ToProto(&operation_proto);
  EXPECT_TRUE(operation_proto.done());
  EXPECT_EQ(operation_proto.error().code(),
            static_cast<int>(grpc::StatusCode::FAILED_PRECONDITION));
  EXPECT_EQ(operation_proto.error().message(), "graph update failed");

  grpc::ClientContext missing_context;
  request.set_operation_id("missing");
  status = test_env()->database_admin_client()->InternalUpdateGraphOperation(
      &missing_context, request, &response);
  EXPECT_EQ(status.error_code(), grpc::StatusCode::NOT_FOUND);
}

TEST_F(DatabaseExtensionsTest, InternalGraphUpdateSuccessCompletesOperation) {
  auto operation =
      test_env()->server()->env()->operation_manager()->CreateOperation(
          test_database_uri_, "graph-success");
  ASSERT_TRUE(operation.ok()) << operation.status();

  database_api::InternalUpdateGraphOperationRequest request;
  request.set_database(test_database_uri_);
  request.set_operation_id("graph-success");
  request.set_progress(50);
  database_api::InternalUpdateGraphOperationResponse response;
  grpc::ClientContext progress_context;
  ASSERT_TRUE(test_env()
                  ->database_admin_client()
                  ->InternalUpdateGraphOperation(&progress_context, request,
                                                 &response)
                  .ok());
  operations_api::Operation operation_proto;
  (*operation)->ToProto(&operation_proto);
  EXPECT_FALSE(operation_proto.done());

  request.mutable_status()->set_code(0);
  grpc::ClientContext success_context;
  ASSERT_TRUE(test_env()
                  ->database_admin_client()
                  ->InternalUpdateGraphOperation(&success_context, request,
                                                 &response)
                  .ok());
  (*operation)->ToProto(&operation_proto);
  EXPECT_TRUE(operation_proto.done());
  EXPECT_FALSE(operation_proto.has_error());

  request.clear_status();
  request.set_progress(101);
  grpc::ClientContext invalid_context;
  EXPECT_EQ(test_env()
                ->database_admin_client()
                ->InternalUpdateGraphOperation(&invalid_context, request,
                                               &response)
                .error_code(),
            grpc::StatusCode::INVALID_ARGUMENT);
}

TEST_F(DatabaseExtensionsTest, OperationListsFilterAndPaginate) {
  OperationManager* manager = test_env()->server()->env()->operation_manager();
  ASSERT_TRUE(manager
                  ->CreateOperation(test_instance_uri_ + "/backups/b1",
                                    "create")
                  .ok());
  ASSERT_TRUE(manager
                  ->CreateOperation(test_instance_uri_ + "/backups/b2",
                                    "create")
                  .ok());
  ASSERT_TRUE(manager
                  ->CreateOperation(test_instance_uri_ + "/databases/d1",
                                    "create")
                  .ok());
  ASSERT_TRUE(manager
                  ->CreateOperation(test_instance_uri_ + "/databases/d2",
                                    "create")
                  .ok());
  ASSERT_TRUE(manager
                  ->CreateOperation(test_instance_uri_ + "0/backups/foreign",
                                    "create")
                  .ok());
  ASSERT_TRUE(manager
                  ->CreateOperation(test_instance_uri_ + "0/databases/foreign",
                                    "create")
                  .ok());

  database_api::ListBackupOperationsRequest backup_request;
  backup_request.set_parent(test_instance_uri_);
  backup_request.set_page_size(1);
  database_api::ListBackupOperationsResponse backup_response;
  grpc::ClientContext backup_context;
  ASSERT_TRUE(test_env()
                  ->database_admin_client()
                  ->ListBackupOperations(&backup_context, backup_request,
                                         &backup_response)
                  .ok());
  ASSERT_EQ(backup_response.operations_size(), 1);
  EXPECT_NE(backup_response.operations(0).name().find("/backups/b1/"),
            std::string::npos);
  ASSERT_FALSE(backup_response.next_page_token().empty());

  backup_request.set_page_token(backup_response.next_page_token());
  backup_response.Clear();
  grpc::ClientContext backup_next_context;
  ASSERT_TRUE(test_env()
                  ->database_admin_client()
                  ->ListBackupOperations(&backup_next_context, backup_request,
                                         &backup_response)
                  .ok());
  ASSERT_EQ(backup_response.operations_size(), 1);
  EXPECT_NE(backup_response.operations(0).name().find("/backups/b2/"),
            std::string::npos);
  EXPECT_TRUE(backup_response.next_page_token().empty());

  database_api::ListDatabaseOperationsRequest database_request;
  database_request.set_parent(test_instance_uri_);
  database_request.set_page_size(1);
  database_api::ListDatabaseOperationsResponse database_response;
  grpc::ClientContext database_context;
  ASSERT_TRUE(test_env()
                  ->database_admin_client()
                  ->ListDatabaseOperations(&database_context, database_request,
                                           &database_response)
                  .ok());
  ASSERT_EQ(database_response.operations_size(), 1);
  EXPECT_NE(database_response.operations(0).name().find("/databases/d1/"),
            std::string::npos);
  ASSERT_FALSE(database_response.next_page_token().empty());

  database_request.set_page_token(database_response.next_page_token());
  database_response.Clear();
  grpc::ClientContext database_next_context;
  ASSERT_TRUE(test_env()
                  ->database_admin_client()
                  ->ListDatabaseOperations(&database_next_context,
                                           database_request,
                                           &database_response)
                  .ok());
  ASSERT_EQ(database_response.operations_size(), 1);
  EXPECT_NE(database_response.operations(0).name().find("/databases/d2/"),
            std::string::npos);
  ASSERT_FALSE(database_response.next_page_token().empty());

  database_request.set_page_token(database_response.next_page_token());
  database_response.Clear();
  grpc::ClientContext database_last_context;
  ASSERT_TRUE(test_env()
                  ->database_admin_client()
                  ->ListDatabaseOperations(&database_last_context,
                                           database_request,
                                           &database_response)
                  .ok());
  ASSERT_EQ(database_response.operations_size(), 1);
  EXPECT_NE(
      database_response.operations(0).name().find("/databases/test-database/"),
      std::string::npos);
  EXPECT_TRUE(database_response.next_page_token().empty());

  ASSERT_TRUE(manager
                  ->CreateOperation(
                      test_instance_uri_ + "/databases/backups", "create")
                  .ok());
  ASSERT_TRUE(manager
                  ->CreateOperation(
                      test_instance_uri_ + "/backups/databases", "create")
                  .ok());
  backup_request.clear_page_token();
  backup_request.set_page_size(100);
  backup_response.Clear();
  grpc::ClientContext reserved_backup_context;
  ASSERT_TRUE(test_env()
                  ->database_admin_client()
                  ->ListBackupOperations(&reserved_backup_context,
                                         backup_request, &backup_response)
                  .ok());
  EXPECT_EQ(backup_response.operations_size(), 3);
  for (const auto& operation : backup_response.operations()) {
    EXPECT_EQ(operation.name().find("/databases/backups/"),
              std::string::npos);
  }

  database_request.clear_page_token();
  database_request.set_page_size(100);
  database_response.Clear();
  grpc::ClientContext reserved_database_context;
  ASSERT_TRUE(test_env()
                  ->database_admin_client()
                  ->ListDatabaseOperations(&reserved_database_context,
                                           database_request,
                                           &database_response)
                  .ok());
  EXPECT_EQ(database_response.operations_size(), 4);
  for (const auto& operation : database_response.operations()) {
    EXPECT_EQ(operation.name().find("/backups/databases/"),
              std::string::npos);
  }
}

TEST_F(DatabaseExtensionsTest, OperationListsApplyDocumentedFilters) {
  OperationManager* manager = test_env()->server()->env()->operation_manager();
  const auto add_operation = [manager](const std::string& resource_uri,
                                       const google::protobuf::Message& metadata,
                                       bool failed) {
    auto operation = manager->CreateOperation(resource_uri, "filtered");
    ASSERT_TRUE(operation.ok()) << operation.status();
    (*operation)->SetMetadata(metadata);
    if (failed) {
      (*operation)->SetError(absl::FailedPreconditionError("failed"));
    }
  };
  database_api::CreateBackupMetadata create;
  create.set_database(test_instance_uri_ + "/databases/test_db");
  add_operation(test_instance_uri_ + "/backups/b1", create, /*failed=*/true);
  add_operation(test_instance_uri_ + "/backups/b3", create, /*failed=*/false);
  create.set_database(test_instance_uri_ + "/databases/prod");
  add_operation(test_instance_uri_ + "/backups/b4", create, /*failed=*/true);
  database_api::CopyBackupMetadata copy;
  copy.set_source_backup(test_instance_uri_ + "/backups/test_bkp");
  add_operation(test_instance_uri_ + "/backups/b2", copy, /*failed=*/true);

  // The documented example means (A AND B OR C AND D) AND error:*, so the
  // successful b3 does not match.
  database_api::ListBackupOperationsRequest backup_request;
  backup_request.set_parent(test_instance_uri_);
  backup_request.set_page_size(1);
  backup_request.set_filter(
      "((metadata.@type=type.googleapis.com/"
      "google.spanner.admin.database.v1.CreateBackupMetadata) AND "
      "(metadata.database:test_db)) OR "
      "((metadata.@type=type.googleapis.com/"
      "google.spanner.admin.database.v1.CopyBackupMetadata) AND "
      "(metadata.source_backup:test_bkp)) AND (error:*)");
  database_api::ListBackupOperationsResponse backup_response;
  grpc::ClientContext first_context;
  grpc::Status status = test_env()->database_admin_client()->ListBackupOperations(
      &first_context, backup_request, &backup_response);
  ASSERT_TRUE(status.ok()) << status.error_message();
  ASSERT_EQ(backup_response.operations_size(), 1);
  EXPECT_EQ(backup_response.operations(0).name(),
            test_instance_uri_ + "/backups/b1/operations/filtered");
  ASSERT_FALSE(backup_response.next_page_token().empty());

  backup_request.set_page_token(backup_response.next_page_token());
  backup_response.Clear();
  grpc::ClientContext second_context;
  status = test_env()->database_admin_client()->ListBackupOperations(
      &second_context, backup_request, &backup_response);
  ASSERT_TRUE(status.ok()) << status.error_message();
  ASSERT_EQ(backup_response.operations_size(), 1);
  EXPECT_EQ(backup_response.operations(0).name(),
            test_instance_uri_ + "/backups/b2/operations/filtered");
  EXPECT_TRUE(backup_response.next_page_token().empty());

  // A page token is only valid with the filter it was issued for.
  backup_request.set_filter("done:true");
  grpc::ClientContext changed_filter_context;
  EXPECT_EQ(test_env()
                ->database_admin_client()
                ->ListBackupOperations(&changed_filter_context, backup_request,
                                       &backup_response)
                .error_code(),
            grpc::StatusCode::INVALID_ARGUMENT);

  backup_request.clear_page_token();
  backup_request.set_page_size(0);
  backup_request.set_filter(
      "METADATA.@TYPE:createbackupmetadata AND NOT error:*");
  backup_response.Clear();
  grpc::ClientContext case_context;
  status = test_env()->database_admin_client()->ListBackupOperations(
      &case_context, backup_request, &backup_response);
  ASSERT_TRUE(status.ok()) << status.error_message();
  ASSERT_EQ(backup_response.operations_size(), 1);
  EXPECT_EQ(backup_response.operations(0).name(),
            test_instance_uri_ + "/backups/b3/operations/filtered");

  backup_request.set_filter("unknown:field");
  grpc::ClientContext unknown_backup_context;
  EXPECT_EQ(test_env()
                ->database_admin_client()
                ->ListBackupOperations(&unknown_backup_context, backup_request,
                                       &backup_response)
                .error_code(),
            grpc::StatusCode::INVALID_ARGUMENT);

  database_api::RestoreDatabaseMetadata restore;
  restore.set_name(test_instance_uri_ + "/databases/restored_howl");
  restore.set_source_type(database_api::BACKUP);
  restore.mutable_backup_info()->set_backup(test_instance_uri_ +
                                            "/backups/backup_howl");
  // One minute before and after 2018-03-28T14:50:00Z.
  restore.mutable_progress()->mutable_start_time()->set_seconds(1522248540);
  add_operation(test_instance_uri_ + "/databases/restored_howl", restore,
                /*failed=*/true);
  restore.mutable_progress()->mutable_start_time()->set_seconds(1522248660);
  add_operation(test_instance_uri_ + "/databases/restored_late", restore,
                /*failed=*/true);

  database_api::ListDatabaseOperationsRequest database_request;
  database_request.set_parent(test_instance_uri_);
  database_request.set_filter(
      "(metadata.@type=type.googleapis.com/"
      "google.spanner.admin.database.v1.RestoreDatabaseMetadata) AND "
      "(metadata.source_type:BACKUP) AND "
      "(metadata.backup_info.backup:backup_howl) AND "
      "(metadata.name:restored_howl) AND "
      "(metadata.progress.start_time < \"2018-03-28T14:50:00Z\") AND "
      "(error:*)");
  database_api::ListDatabaseOperationsResponse database_response;
  grpc::ClientContext restore_context;
  status = test_env()->database_admin_client()->ListDatabaseOperations(
      &restore_context, database_request, &database_response);
  ASSERT_TRUE(status.ok()) << status.error_message();
  ASSERT_EQ(database_response.operations_size(), 1);
  EXPECT_EQ(database_response.operations(0).name(),
            test_instance_uri_ + "/databases/restored_howl/operations/filtered");

  database_request.set_filter("metadata.progress.start_time < yesterday");
  grpc::ClientContext invalid_value_context;
  EXPECT_EQ(test_env()
                ->database_admin_client()
                ->ListDatabaseOperations(&invalid_value_context,
                                         database_request, &database_response)
                .error_code(),
            grpc::StatusCode::INVALID_ARGUMENT);
  database_request.set_filter("(done:true");
  grpc::ClientContext invalid_syntax_context;
  EXPECT_EQ(test_env()
                ->database_admin_client()
                ->ListDatabaseOperations(&invalid_syntax_context,
                                         database_request, &database_response)
                .error_code(),
            grpc::StatusCode::INVALID_ARGUMENT);
}

TEST_F(DatabaseExtensionsTest, DatabaseRolesIncludeSystemRoles) {
  database_api::ListDatabaseRolesRequest request;
  request.set_parent(test_database_uri_);
  database_api::ListDatabaseRolesResponse response;
  grpc::ClientContext context;
  grpc::Status status = test_env()->database_admin_client()->ListDatabaseRoles(
      &context, request, &response);
  ASSERT_TRUE(status.ok()) << status.error_message();
  ASSERT_EQ(response.database_roles_size(), 3);
  EXPECT_EQ(response.database_roles(0).name(),
            test_database_uri_ + "/databaseRoles/public");
  EXPECT_EQ(response.database_roles(1).name(),
            test_database_uri_ + "/databaseRoles/spanner_info_reader");
  EXPECT_EQ(response.database_roles(2).name(),
            test_database_uri_ + "/databaseRoles/spanner_sys_reader");
  EXPECT_TRUE(response.next_page_token().empty());
}

TEST_F(DatabaseExtensionsTest, DatabaseRolesFollowDdlAndPaginate) {
  ASSERT_TRUE(UpdateDatabaseDdl(test_database_uri_,
                                {"CREATE ROLE beta", "CREATE ROLE Alpha"})
                  .ok());

  database_api::ListDatabaseRolesRequest request;
  request.set_parent(test_database_uri_);
  request.set_page_size(1);
  database_api::ListDatabaseRolesResponse first;
  grpc::ClientContext first_context;
  grpc::Status status = test_env()->database_admin_client()->ListDatabaseRoles(
      &first_context, request, &first);
  ASSERT_TRUE(status.ok()) << status.error_message();
  ASSERT_EQ(first.database_roles_size(), 1);
  EXPECT_EQ(first.database_roles(0).name(),
            test_database_uri_ + "/databaseRoles/Alpha");
  ASSERT_EQ(first.next_page_token(),
            test_database_uri_ + "/databaseRoles/beta");

  request.set_page_token(first.next_page_token());
  database_api::ListDatabaseRolesResponse second;
  grpc::ClientContext second_context;
  status = test_env()->database_admin_client()->ListDatabaseRoles(
      &second_context, request, &second);
  ASSERT_TRUE(status.ok()) << status.error_message();
  ASSERT_EQ(second.database_roles_size(), 1);
  EXPECT_EQ(second.database_roles(0).name(),
            test_database_uri_ + "/databaseRoles/beta");
  // The system roles sort after the user roles.
  EXPECT_EQ(second.next_page_token(),
            test_database_uri_ + "/databaseRoles/public");

  database_api::GetDatabaseDdlRequest ddl_request;
  ddl_request.set_database(test_database_uri_);
  database_api::GetDatabaseDdlResponse ddl_response;
  grpc::ClientContext ddl_context;
  status = test_env()->database_admin_client()->GetDatabaseDdl(
      &ddl_context, ddl_request, &ddl_response);
  ASSERT_TRUE(status.ok()) << status.error_message();
  bool has_alpha = false;
  bool has_beta = false;
  for (const std::string& statement : ddl_response.statements()) {
    has_alpha |= statement == "CREATE ROLE Alpha";
    has_beta |= statement == "CREATE ROLE beta";
  }
  EXPECT_TRUE(has_alpha);
  EXPECT_TRUE(has_beta);

  EXPECT_EQ(UpdateDatabaseDdl(test_database_uri_, {"CREATE ROLE alpha"}).code(),
            absl::StatusCode::kFailedPrecondition);
  EXPECT_EQ(UpdateDatabaseDdl(test_database_uri_,
                              {"CREATE ROLE spanner_internal"})
                .code(),
            absl::StatusCode::kInvalidArgument);

  request.set_page_token("projects/other/instances/i/databases/d/databaseRoles/beta");
  database_api::ListDatabaseRolesResponse invalid;
  grpc::ClientContext invalid_context;
  status = test_env()->database_admin_client()->ListDatabaseRoles(
      &invalid_context, request, &invalid);
  EXPECT_EQ(status.error_code(), grpc::StatusCode::INVALID_ARGUMENT);

  ASSERT_TRUE(UpdateDatabaseDdl(test_database_uri_, {"DROP ROLE Alpha"}).ok());
  request.clear_page_token();
  request.set_page_size(10);
  database_api::ListDatabaseRolesResponse after_drop;
  grpc::ClientContext after_drop_context;
  status = test_env()->database_admin_client()->ListDatabaseRoles(
      &after_drop_context, request, &after_drop);
  ASSERT_TRUE(status.ok()) << status.error_message();
  ASSERT_EQ(after_drop.database_roles_size(), 4);
  EXPECT_EQ(after_drop.database_roles(0).name(),
            test_database_uri_ + "/databaseRoles/beta");
  EXPECT_EQ(UpdateDatabaseDdl(test_database_uri_, {"DROP ROLE Alpha"}).code(),
            absl::StatusCode::kNotFound);
}

}  // namespace
}  // namespace frontend
}  // namespace emulator
}  // namespace spanner
}  // namespace google

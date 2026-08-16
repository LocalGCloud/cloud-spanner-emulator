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

#include "frontend/collections/operation_manager.h"
#include "google/longrunning/operations.pb.h"
#include "google/spanner/admin/database/v1/spanner_database_admin.pb.h"
#include "grpcpp/client_context.h"
#include "gtest/gtest.h"
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

TEST_F(DatabaseExtensionsTest, DatabaseRolesAreEmpty) {
  database_api::ListDatabaseRolesRequest request;
  request.set_parent(test_database_uri_);
  database_api::ListDatabaseRolesResponse response;
  grpc::ClientContext context;
  grpc::Status status = test_env()->database_admin_client()->ListDatabaseRoles(
      &context, request, &response);
  ASSERT_TRUE(status.ok()) << status.error_message();
  EXPECT_EQ(response.database_roles_size(), 0);
  EXPECT_TRUE(response.next_page_token().empty());
}

}  // namespace
}  // namespace frontend
}  // namespace emulator
}  // namespace spanner
}  // namespace google

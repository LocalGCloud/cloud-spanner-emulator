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

#include <memory>

#include "gmock/gmock.h"
#include "googlesql/base/testing/status_matchers.h"
#include "grpcpp/client_context.h"
#include "gtest/gtest.h"
#include "tests/common/proto_matchers.h"
#include "tests/common/test_env.h"

namespace google {
namespace spanner {
namespace emulator {
namespace frontend {

namespace {

namespace instance_api = ::google::spanner::admin::instance::v1;
namespace operations_api = ::google::longrunning;

class OperationApiTest : public test::ServerTest {
  void SetUp() override {
    // CreateOperation is not a GRPC method; create operations directly in the
    // OperationManager.
    google::longrunning::Operation operation;
    instance_api::CreateInstanceRequest request = PARSE_TEXT_PROTO(R"pb(
      parent: "projects/123"
      instance_id: "instance-456"
      instance { config: "emulator-config" display_name: "" node_count: 3 }
    )pb");
    grpc::ClientContext ctx;
    GOOGLESQL_EXPECT_OK(test_env()->instance_admin_client()->CreateInstance(
        &ctx, request, &operation));
  }
};

TEST_F(OperationApiTest, ListsOperations) {
  operations_api::ListOperationsRequest request = PARSE_TEXT_PROTO(R"pb(
    name: "projects/123/instances/instance-456/operations"
  )pb");
  operations_api::ListOperationsResponse response;
  grpc::ClientContext context;
  GOOGLESQL_EXPECT_OK(test_env()->operations_client()->ListOperations(
      &context, request, &response));
  EXPECT_THAT(response, test::proto::Partially(test::EqualsProto(R"pb(
                operations {
                  name: "projects/123/instances/instance-456/operations/_auto0"
                }
              )pb")));
}

TEST_F(OperationApiTest, ListsOperationsInvalidUri) {
  operations_api::ListOperationsRequest request = PARSE_TEXT_PROTO(R"pb(
    name: "projects/123/instance/instance-456/operations"
  )pb");
  operations_api::ListOperationsResponse response;
  grpc::ClientContext context;
  EXPECT_THAT(test_env()->operations_client()->ListOperations(&context, request,
                                                              &response),
              googlesql_base::testing::StatusIs(
                  absl::StatusCode::kInvalidArgument,
                  testing::MatchesRegex(".*Invalid instance uri.*")));
}

TEST_F(OperationApiTest, GetOperation) {
  operations_api::GetOperationRequest request = PARSE_TEXT_PROTO(R"pb(
    name: "projects/123/instances/instance-456/operations/_auto0"
  )pb");
  operations_api::Operation operation;
  grpc::ClientContext context;
  GOOGLESQL_EXPECT_OK(test_env()->operations_client()->GetOperation(
      &context, request, &operation));
  EXPECT_THAT(
      operation,
      test::proto::Partially(test::EqualsProto(
          R"pb(name: "projects/123/instances/instance-456/operations/_auto0")pb")));
}

TEST_F(OperationApiTest, GetOperationInvalidUri) {
  operations_api::GetOperationRequest request = PARSE_TEXT_PROTO(R"pb(
    name: "projects/123/instances/instance-456/operation/_auto0"
  )pb");
  operations_api::Operation operation;
  grpc::ClientContext context;
  EXPECT_THAT(test_env()->operations_client()->GetOperation(&context, request,
                                                            &operation),
              googlesql_base::testing::StatusIs(
                  absl::StatusCode::kInvalidArgument,
                  testing::MatchesRegex(".*Invalid operation uri.*")));
}

TEST_F(OperationApiTest, WaitReturnsTerminalOperation) {
  operations_api::WaitOperationRequest request = PARSE_TEXT_PROTO(R"pb(
    name: "projects/123/instances/instance-456/operations/_auto0"
  )pb");
  operations_api::Operation operation;
  grpc::ClientContext context;
  GOOGLESQL_EXPECT_OK(test_env()->operations_client()->WaitOperation(
      &context, request, &operation));
  EXPECT_TRUE(operation.done());
}

TEST_F(OperationApiTest, CancelAcceptsCompletedOperation) {
  operations_api::CancelOperationRequest request = PARSE_TEXT_PROTO(R"pb(
    name: "projects/123/instances/instance-456/operations/_auto0"
  )pb");
  google::protobuf::Empty response;
  grpc::ClientContext context;
  GOOGLESQL_EXPECT_OK(test_env()->operations_client()->CancelOperation(
      &context, request, &response));
}

TEST_F(OperationApiTest, DeleteRemovesOperation) {
  operations_api::DeleteOperationRequest request = PARSE_TEXT_PROTO(R"pb(
    name: "projects/123/instances/instance-456/operations/_auto0"
  )pb");
  google::protobuf::Empty response;
  grpc::ClientContext delete_context;
  GOOGLESQL_EXPECT_OK(test_env()->operations_client()->DeleteOperation(
      &delete_context, request, &response));

  operations_api::GetOperationRequest get_request;
  get_request.set_name(request.name());
  operations_api::Operation operation;
  grpc::ClientContext get_context;
  EXPECT_THAT(test_env()->operations_client()->GetOperation(
                  &get_context, get_request, &operation),
              googlesql_base::testing::StatusIs(absl::StatusCode::kNotFound));
}
}  // namespace

}  // namespace frontend
}  // namespace emulator
}  // namespace spanner
}  // namespace google

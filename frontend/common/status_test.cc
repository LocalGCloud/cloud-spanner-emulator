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

#include "absl/status/status.h"

#include <string>

#include "google/rpc/error_details.pb.h"
#include "google/rpc/status.pb.h"
#include "gmock/gmock.h"
#include "gtest/gtest.h"
#include "googlesql/base/testing/status_matchers.h"
#include "tests/common/proto_matchers.h"
#include "frontend/common/status.h"
#include "googlesql/base/ret_check.h"
#include "absl/status/status.h"
#include "absl/strings/cord.h"
#include "common/constants.h"

namespace google {
namespace spanner {
namespace emulator {

namespace {

TEST(GrpcStatusConversion, MessageLength) {
  std::string error_message(2048, 'a');
  EXPECT_THAT(ToGRPCStatus(absl::Status(absl::StatusCode::kInvalidArgument,
                                        error_message))
                  .error_message(),
              testing::EndsWith("..."));
}

TEST(GrpcStatusConversion, ForwardsOnlyStandardErrorDetails) {
  google::rpc::ResourceInfo resource_info;
  resource_info.set_resource_type(kSessionResourceType);
  resource_info.set_resource_name("projects/p/instances/i/databases/d/s/s");
  absl::Status status(absl::StatusCode::kFailedPrecondition, "failed");
  status.SetPayload(kResourceInfoType,
                    absl::Cord(resource_info.SerializeAsString()));
  // Internal markers that must not reach clients.
  status.SetPayload(kConstraintError, absl::Cord(""));
  status.SetPayload("type.googleapis.com/googlesql.ErrorMessageModeForPayload",
                    absl::Cord(""));

  grpc::Status grpc_status = ToGRPCStatus(status);
  EXPECT_EQ(grpc_status.error_code(), grpc::StatusCode::FAILED_PRECONDITION);
  EXPECT_EQ(grpc_status.error_message(), "failed");

  google::rpc::Status rpc_status;
  ASSERT_TRUE(rpc_status.ParseFromString(grpc_status.error_details()));
  EXPECT_EQ(rpc_status.code(),
            static_cast<int>(absl::StatusCode::kFailedPrecondition));
  ASSERT_EQ(rpc_status.details_size(), 1);
  EXPECT_EQ(rpc_status.details(0).type_url(), kResourceInfoType);
  google::rpc::ResourceInfo forwarded_info;
  ASSERT_TRUE(rpc_status.details(0).UnpackTo(&forwarded_info));
  EXPECT_THAT(forwarded_info, test::EqualsProto(resource_info));
}

TEST(GrpcStatusConversion, DropsMarkerOnlyDetails) {
  absl::Status status(absl::StatusCode::kAlreadyExists, "duplicate key");
  status.SetPayload(kConstraintError, absl::Cord(""));

  grpc::Status grpc_status = ToGRPCStatus(status);
  EXPECT_EQ(grpc_status.error_code(), grpc::StatusCode::ALREADY_EXISTS);

  google::rpc::Status rpc_status;
  ASSERT_TRUE(rpc_status.ParseFromString(grpc_status.error_details()));
  EXPECT_EQ(rpc_status.code(),
            static_cast<int>(absl::StatusCode::kAlreadyExists));
  EXPECT_EQ(rpc_status.message(), "duplicate key");
  EXPECT_EQ(rpc_status.details_size(), 0);
}

}  // namespace
}  // namespace emulator
}  // namespace spanner
}  // namespace google

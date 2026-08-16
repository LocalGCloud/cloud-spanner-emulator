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

#include "google/spanner/v1/location.pb.h"
#include "google/spanner/v1/spanner.grpc.pb.h"
#include "google/spanner/v1/spanner.pb.h"
#include "grpcpp/client_context.h"
#include "gtest/gtest.h"
#include "tests/common/test_env.h"

namespace google {
namespace spanner {
namespace emulator {
namespace frontend {
namespace {

namespace spanner_api = ::google::spanner::v1;

class CacheUpdatesTest : public test::ServerTest {
 protected:
  void SetUp() override {
    ASSERT_TRUE(CreateTestInstance().ok());
    ASSERT_TRUE(CreateTestDatabase().ok());
  }

  grpc::Status Fetch(const std::string& database, int* response_count) {
    grpc::ClientContext context;
    spanner_api::FetchCacheUpdateRequest request;
    request.set_database(database);
    std::unique_ptr<grpc::ClientReader<spanner_api::CacheUpdate>> reader =
        test_env()->spanner_client()->FetchCacheUpdate(&context, request);
    spanner_api::CacheUpdate response;
    while (reader->Read(&response)) {
      ++*response_count;
    }
    return reader->Finish();
  }
};

TEST_F(CacheUpdatesTest, ExistingDatabaseReturnsEmptySuccessfulStream) {
  int response_count = 0;
  grpc::Status status = Fetch(test_database_uri_, &response_count);
  EXPECT_TRUE(status.ok()) << status.error_message();
  EXPECT_EQ(response_count, 0);
}

TEST_F(CacheUpdatesTest, InvalidDatabaseResourceIsRejected) {
  int response_count = 0;
  grpc::Status status = Fetch("not-a-database", &response_count);
  EXPECT_EQ(status.error_code(), grpc::StatusCode::INVALID_ARGUMENT);
  EXPECT_EQ(response_count, 0);

  status = Fetch(test_database_uri_ + "/sessions/extra", &response_count);
  EXPECT_EQ(status.error_code(), grpc::StatusCode::INVALID_ARGUMENT);
  EXPECT_EQ(response_count, 0);
}

TEST_F(CacheUpdatesTest, MissingDatabaseIsRejected) {
  int response_count = 0;
  grpc::Status status =
      Fetch(test_instance_uri_ + "/databases/missing", &response_count);
  EXPECT_EQ(status.error_code(), grpc::StatusCode::NOT_FOUND);
  EXPECT_EQ(response_count, 0);
}

}  // namespace
}  // namespace frontend
}  // namespace emulator
}  // namespace spanner
}  // namespace google

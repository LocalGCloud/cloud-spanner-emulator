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
#include <string>
#include <vector>

#include "google/spanner/admin/database/v1/common.pb.h"
#include "google/spanner/v1/result_set.pb.h"
#include "google/spanner/v1/spanner.pb.h"
#include "gmock/gmock.h"
#include "gtest/gtest.h"
#include "googlesql/base/testing/status_matchers.h"
#include "tests/common/proto_matchers.h"
#include "absl/status/status.h"
#include "absl/status/statusor.h"
#include "grpcpp/client_context.h"
#include "tests/conformance/common/database_test_base.h"

namespace google {
namespace spanner {
namespace emulator {
namespace test {

namespace {

namespace spanner_api = ::google::spanner::v1;

constexpr int64_t kNumRows = 20;
constexpr int64_t kStringSize = 409600;

class LargeReadsTest
    : public DatabaseTest,
      public testing::WithParamInterface<database_api::DatabaseDialect> {
 public:
  void SetUp() override {
    dialect_ = GetParam();
    DatabaseTest::SetUp();
  }

 public:
  absl::Status SetUpDatabase() override {
    return SetSchemaFromFile("large_reads.test");
  }

 protected:
  void PopulateDatabase() {
    // Populate the database with more than 4MB worth of data.
    for (int i = 0; i < kNumRows; ++i) {
      GOOGLESQL_EXPECT_OK(Insert("Users", {"ID", "Name"},
                       {i, std::string(kStringSize, 'a' + (i % 26))}));
    }
  }

  absl::StatusOr<std::string> CreateSession() {
    grpc::ClientContext context;
    spanner_api::CreateSessionRequest request;
    request.set_database(database()->FullName());
    spanner_api::Session response;
    GOOGLESQL_RETURN_IF_ERROR(
        raw_client()->CreateSession(&context, request, &response));
    return response.name();
  }

  // Returns the stream of responses to `request`.
  absl::StatusOr<std::vector<spanner_api::PartialResultSet>>
  ExecuteStreamingSql(const spanner_api::ExecuteSqlRequest& request) {
    grpc::ClientContext context;
    std::unique_ptr<grpc::ClientReader<spanner_api::PartialResultSet>> reader =
        raw_client()->ExecuteStreamingSql(&context, request);
    std::vector<spanner_api::PartialResultSet> responses;
    spanner_api::PartialResultSet response;
    while (reader->Read(&response)) {
      responses.push_back(response);
    }
    GOOGLESQL_RETURN_IF_ERROR(reader->Finish());
    return responses;
  }

  // Returns the IDs of the rows that `responses` complete. Values of the
  // Name column span several responses.
  static std::vector<std::string> RowIds(
      const std::vector<spanner_api::PartialResultSet>& responses) {
    std::vector<std::string> ids;
    bool continues_value = false;
    int column = 0;
    for (const spanner_api::PartialResultSet& response : responses) {
      for (int i = 0; i < response.values_size(); ++i) {
        const bool continued = i == 0 && continues_value;
        if (!continued) {
          if (column == 0) {
            ids.push_back(response.values(i).string_value());
          }
          column = (column + 1) % 2;
        }
      }
      continues_value = response.chunked_value();
    }
    return ids;
  }
};

INSTANTIATE_TEST_SUITE_P(
    PerDialectLargeReadsTest, LargeReadsTest,
    testing::Values(database_api::DatabaseDialect::GOOGLE_STANDARD_SQL,
                    database_api::DatabaseDialect::POSTGRESQL),
    [](const testing::TestParamInfo<LargeReadsTest::ParamType>& info) {
      return database_api::DatabaseDialect_Name(info.param);
    });

TEST_P(LargeReadsTest, CanPerformLargeReadWithRange) {
  PopulateDatabase();
  GOOGLESQL_ASSERT_OK_AND_ASSIGN(
      std::vector<ValueRow> rows,
      Read("Users", {"ID", "Name"}, ClosedOpen(Key(0), Key(12))));
  EXPECT_THAT(rows.size(), 12);
  for (int i = 0; i < rows.size(); ++i) {
    std::vector<google::cloud::spanner::Value> values = rows[i];
    EXPECT_THAT(values,
                testing::ElementsAre(
                    Value(i), Value(std::string(kStringSize, 'a' + (i % 26)))));
  }
}

TEST_P(LargeReadsTest, CanPerformLargeReadWithAllRows) {
  PopulateDatabase();
  GOOGLESQL_ASSERT_OK_AND_ASSIGN(std::vector<ValueRow> rows,
                       Read("Users", {"ID", "Name"}, KeySet::All()));
  EXPECT_EQ(rows.size(), kNumRows);
  for (int i = 0; i < rows.size(); ++i) {
    std::vector<google::cloud::spanner::Value> values = rows[i];
    EXPECT_THAT(values,
                testing::ElementsAre(
                    Value(i), Value(std::string(kStringSize, 'a' + (i % 26)))));
  }
}

// Resumes a stream the way client libraries do after the stream breaks: by
// resending the request with the resume token of the last response received.
TEST_P(LargeReadsTest, ResumesStreamingQueryAfterResumeToken) {
  PopulateDatabase();
  GOOGLESQL_ASSERT_OK_AND_ASSIGN(std::string session, CreateSession());
  spanner_api::ExecuteSqlRequest request;
  request.set_session(session);
  request.set_sql("SELECT ID, Name FROM Users");
  request.mutable_transaction()
      ->mutable_single_use()
      ->mutable_read_only()
      ->set_strong(true);
  GOOGLESQL_ASSERT_OK_AND_ASSIGN(
      std::vector<spanner_api::PartialResultSet> original,
      ExecuteStreamingSql(request));
  const std::vector<std::string> ids = RowIds(original);
  ASSERT_EQ(ids.size(), kNumRows);

  int resumed_streams = 0;
  for (int i = 0; i + 1 < original.size(); ++i) {
    if (original[i].resume_token().empty()) {
      continue;
    }
    ++resumed_streams;
    const std::vector<std::string> received =
        RowIds({original.begin(), original.begin() + i + 1});
    request.set_resume_token(original[i].resume_token());
    GOOGLESQL_ASSERT_OK_AND_ASSIGN(
        std::vector<spanner_api::PartialResultSet> rest,
        ExecuteStreamingSql(request));
    std::vector<std::string> resumed_ids = received;
    for (const std::string& id : RowIds(rest)) {
      resumed_ids.push_back(id);
    }
    EXPECT_EQ(resumed_ids, ids) << "after response " << i;
  }
  EXPECT_GT(resumed_streams, 1);
}

}  // namespace

}  // namespace test
}  // namespace emulator
}  // namespace spanner
}  // namespace google

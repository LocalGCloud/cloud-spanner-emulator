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

#include <set>
#include <string>

#include "google/spanner/v1/mutation.pb.h"
#include "google/spanner/v1/spanner.pb.h"
#include "google/spanner/v1/transaction.pb.h"
#include "gmock/gmock.h"
#include "gtest/gtest.h"
#include "googlesql/base/testing/status_matchers.h"
#include "tests/common/proto_matchers.h"
#include "absl/status/status.h"
#include "common/errors.h"
#include "tests/common/test_env.h"

namespace google {
namespace spanner {
namespace emulator {
namespace frontend {

namespace {

using ::googlesql_base::testing::StatusIs;

enum class SessionType {
  kRegularSession,
  kMultiplexedSession,
};

namespace spanner_api = ::google::spanner::v1;

class PartitionApiTest : public test::ServerTest,
                         public testing::WithParamInterface<SessionType> {
 protected:
  void SetUp() override {
    GOOGLESQL_ASSERT_OK(CreateTestInstance());
    GOOGLESQL_ASSERT_OK(CreateTestDatabase());
    GOOGLESQL_ASSERT_OK_AND_ASSIGN(test_session_uri_,
                         CreateTestSession(/*multiplexed=*/false));
    GOOGLESQL_ASSERT_OK_AND_ASSIGN(test_multiplexed_session_uri_,
                         CreateTestSession(/*multiplexed=*/true));
  }

  std::string GetSessionUri(bool multiplexed) {
    return multiplexed ? test_multiplexed_session_uri_ : test_session_uri_;
  }

  SessionType GetSessionType() { return GetParam(); }

  std::string test_session_uri_;
  std::string test_multiplexed_session_uri_;
};

INSTANTIATE_TEST_SUITE_P(SessionTypes, PartitionApiTest,
                         testing::Values(SessionType::kRegularSession,
                                         SessionType::kMultiplexedSession));

TEST_P(PartitionApiTest, RequiredSession) {
  spanner_api::PartitionReadRequest partition_read_request;

  spanner_api::PartitionResponse partition_read_response;
  EXPECT_THAT(PartitionRead(partition_read_request, &partition_read_response),
              StatusIs(absl::StatusCode::kInvalidArgument));
}

TEST_P(PartitionApiTest, RequiredTransaction) {
  spanner_api::PartitionReadRequest partition_read_request;
  partition_read_request.set_session(
      GetSessionUri(GetSessionType() == SessionType::kMultiplexedSession));

  spanner_api::PartitionResponse partition_read_response;
  EXPECT_THAT(PartitionRead(partition_read_request, &partition_read_response),
              StatusIs(absl::StatusCode::kInvalidArgument));
}

TEST_P(PartitionApiTest, CannotReadUsingSingleUseTransaction) {
  spanner_api::PartitionReadRequest partition_read_request = PARSE_TEXT_PROTO(
      R"(
        transaction { single_use { read_only {} } }
      )");
  partition_read_request.set_session(
      GetSessionUri(GetSessionType() == SessionType::kMultiplexedSession));

  spanner_api::PartitionResponse partition_read_response;
  EXPECT_THAT(PartitionRead(partition_read_request, &partition_read_response),
              StatusIs(absl::StatusCode::kInvalidArgument));
}

TEST_P(PartitionApiTest, CannotReadUsingBeginReadWriteTransaction) {
  // Begin a new read only transaction.
  spanner_api::PartitionReadRequest partition_read_request = PARSE_TEXT_PROTO(
      R"(
        transaction { begin { read_write {} } }
      )");
  partition_read_request.set_session(
      GetSessionUri(GetSessionType() == SessionType::kMultiplexedSession));

  spanner_api::PartitionResponse partition_read_response;
  EXPECT_THAT(PartitionRead(partition_read_request, &partition_read_response),
              StatusIs(absl::StatusCode::kInvalidArgument));
}

TEST_P(PartitionApiTest, CannotReadUsingExistingReadWriteTransaction) {
  spanner_api::BeginTransactionRequest txn_request = PARSE_TEXT_PROTO(R"(
    options { read_write {} }
  )");
  txn_request.set_session(
      GetSessionUri(GetSessionType() == SessionType::kMultiplexedSession));

  spanner_api::Transaction txn_response;
  GOOGLESQL_ASSERT_OK(BeginTransaction(txn_request, &txn_response));

  // Perform read using the transaction that was started above.
  spanner_api::TransactionSelector selector;
  selector.set_id(txn_response.id());

  spanner_api::PartitionReadRequest partition_read_request =
      PARSE_TEXT_PROTO(R"(
        table: "test_table"
      )");
  partition_read_request.set_session(
      GetSessionUri(GetSessionType() == SessionType::kMultiplexedSession));
  *partition_read_request.mutable_transaction() = selector;

  spanner_api::PartitionResponse partition_read_response;
  EXPECT_THAT(PartitionRead(partition_read_request, &partition_read_response),
              StatusIs(absl::StatusCode::kInvalidArgument));
}

TEST_P(PartitionApiTest, CannotReadUsingInvalidPartitionOptions) {
  // Test that negative partition_size_bytes is not allowed.
  spanner_api::PartitionReadRequest partition_read_request = PARSE_TEXT_PROTO(
      R"(
        transaction { begin { read_only {} } }
        partition_options { partition_size_bytes: -1 max_partitions: 100 }
      )");
  partition_read_request.set_session(
      GetSessionUri(GetSessionType() == SessionType::kMultiplexedSession));

  spanner_api::PartitionResponse partition_read_response;
  EXPECT_THAT(PartitionRead(partition_read_request, &partition_read_response),
              StatusIs(absl::StatusCode::kInvalidArgument));

  // Test that negative max_partitions is not allowed.
  partition_read_request = PARSE_TEXT_PROTO(
      R"(
        transaction { begin { read_only {} } }
        partition_options { partition_size_bytes: 10000 max_partitions: -1 }
      )");
  partition_read_request.set_session(
      GetSessionUri(GetSessionType() == SessionType::kMultiplexedSession));

  EXPECT_THAT(PartitionRead(partition_read_request, &partition_read_response),
              StatusIs(absl::StatusCode::kInvalidArgument));
}

TEST_P(PartitionApiTest, MultiPartitionQueryExecution) {
  std::string session_uri =
      GetSessionUri(GetSessionType() == SessionType::kMultiplexedSession);

  // Populate test_table with 9 rows: 1..9.
  spanner_api::CommitRequest commit_request;
  commit_request.set_session(session_uri);
  commit_request.mutable_single_use_transaction()->mutable_read_write();
  auto* mutation = commit_request.add_mutations()->mutable_insert();
  mutation->set_table("test_table");
  mutation->add_columns("int64_col");
  mutation->add_columns("string_col");
  for (int i = 1; i <= 9; ++i) {
    auto* val_list = mutation->add_values();
    val_list->add_values()->set_string_value(std::to_string(i));
    val_list->add_values()->set_string_value("val_" + std::to_string(i));
  }
  spanner_api::CommitResponse commit_response;
  GOOGLESQL_ASSERT_OK(Commit(commit_request, &commit_response));

  // Begin a read-only transaction.
  spanner_api::BeginTransactionRequest begin_req;
  begin_req.set_session(session_uri);
  begin_req.mutable_options()->mutable_read_only();
  spanner_api::Transaction txn_response;
  GOOGLESQL_ASSERT_OK(BeginTransaction(begin_req, &txn_response));

  spanner_api::TransactionSelector selector;
  selector.set_id(txn_response.id());

  // Request PartitionQuery with max_partitions = 3.
  spanner_api::PartitionQueryRequest query_req;
  query_req.set_session(session_uri);
  *query_req.mutable_transaction() = selector;
  query_req.set_sql("SELECT int64_col, string_col FROM test_table");
  query_req.mutable_partition_options()->set_max_partitions(3);

  spanner_api::PartitionResponse query_resp;
  grpc::ClientContext ctx;
  GOOGLESQL_ASSERT_OK(test_env()->spanner_client()->PartitionQuery(&ctx, query_req, &query_resp));
  EXPECT_EQ(query_resp.partitions_size(), 3);

  // Execute each partition via ExecuteSql and verify all 9 rows are covered with no duplicates.
  std::set<int64_t> received_keys;
  for (int i = 0; i < query_resp.partitions_size(); ++i) {
    spanner_api::ExecuteSqlRequest exec_req;
    exec_req.set_session(session_uri);
    *exec_req.mutable_transaction() = selector;
    exec_req.set_sql("SELECT int64_col, string_col FROM test_table");
    exec_req.set_partition_token(query_resp.partitions(i).partition_token());

    spanner_api::ResultSet exec_resp;
    GOOGLESQL_ASSERT_OK(ExecuteSql(exec_req, &exec_resp));
    EXPECT_EQ(exec_resp.rows_size(), 3);
    for (const auto& row : exec_resp.rows()) {
      int64_t key = std::stoll(row.values(0).string_value());
      EXPECT_TRUE(received_keys.insert(key).second) << "Duplicate key: " << key;
    }
  }
  EXPECT_EQ(received_keys.size(), 9);

  // Also execute each partition via ExecuteStreamingSql.
  std::set<int64_t> streaming_keys;
  for (int i = 0; i < query_resp.partitions_size(); ++i) {
    spanner_api::ExecuteSqlRequest exec_req;
    exec_req.set_session(session_uri);
    *exec_req.mutable_transaction() = selector;
    exec_req.set_sql("SELECT int64_col, string_col FROM test_table");
    exec_req.set_partition_token(query_resp.partitions(i).partition_token());

    std::vector<spanner_api::PartialResultSet> streaming_resp;
    GOOGLESQL_ASSERT_OK(ExecuteStreamingSql(exec_req, &streaming_resp));
    for (const auto& partial_rs : streaming_resp) {
      for (int v = 0; v < partial_rs.values_size(); v += 2) {
        int64_t key = std::stoll(partial_rs.values(v).string_value());
        EXPECT_TRUE(streaming_keys.insert(key).second) << "Duplicate key: " << key;
      }
    }
  }
  EXPECT_EQ(streaming_keys.size(), 9);
}

TEST_P(PartitionApiTest, MultiPartitionReadExecution) {
  std::string session_uri =
      GetSessionUri(GetSessionType() == SessionType::kMultiplexedSession);

  // Populate test_table with 9 rows: 1..9.
  spanner_api::CommitRequest commit_request;
  commit_request.set_session(session_uri);
  commit_request.mutable_single_use_transaction()->mutable_read_write();
  auto* mutation = commit_request.add_mutations()->mutable_insert();
  mutation->set_table("test_table");
  mutation->add_columns("int64_col");
  mutation->add_columns("string_col");
  for (int i = 1; i <= 9; ++i) {
    auto* val_list = mutation->add_values();
    val_list->add_values()->set_string_value(std::to_string(i));
    val_list->add_values()->set_string_value("val_" + std::to_string(i));
  }
  spanner_api::CommitResponse commit_response;
  GOOGLESQL_ASSERT_OK(Commit(commit_request, &commit_response));

  // Begin a read-only transaction.
  spanner_api::BeginTransactionRequest begin_req;
  begin_req.set_session(session_uri);
  begin_req.mutable_options()->mutable_read_only();
  spanner_api::Transaction txn_response;
  GOOGLESQL_ASSERT_OK(BeginTransaction(begin_req, &txn_response));

  spanner_api::TransactionSelector selector;
  selector.set_id(txn_response.id());

  // Test 1: PartitionRead with key_set { all: true } and max_partitions = 3.
  {
    spanner_api::PartitionReadRequest read_req;
    read_req.set_session(session_uri);
    *read_req.mutable_transaction() = selector;
    read_req.set_table("test_table");
    read_req.add_columns("int64_col");
    read_req.add_columns("string_col");
    read_req.mutable_key_set()->set_all(true);
    read_req.mutable_partition_options()->set_max_partitions(3);

    spanner_api::PartitionResponse read_resp;
    GOOGLESQL_ASSERT_OK(PartitionRead(read_req, &read_resp));
    EXPECT_EQ(read_resp.partitions_size(), 3);

    std::set<int64_t> received_keys;
    for (int i = 0; i < read_resp.partitions_size(); ++i) {
      spanner_api::ReadRequest data_req;
      data_req.set_session(session_uri);
      *data_req.mutable_transaction() = selector;
      data_req.set_table("test_table");
      data_req.add_columns("int64_col");
      data_req.add_columns("string_col");
      data_req.mutable_key_set()->set_all(true);
      data_req.set_partition_token(read_resp.partitions(i).partition_token());

      spanner_api::ResultSet data_resp;
      GOOGLESQL_ASSERT_OK(Read(data_req, &data_resp));
      EXPECT_EQ(data_resp.rows_size(), 3);
      for (const auto& row : data_resp.rows()) {
        int64_t key = std::stoll(row.values(0).string_value());
        EXPECT_TRUE(received_keys.insert(key).second) << "Duplicate key: " << key;
      }
    }
    EXPECT_EQ(received_keys.size(), 9);
  }

  // Test 2: PartitionRead with discrete keys and max_partitions = 2.
  {
    spanner_api::PartitionReadRequest read_req;
    read_req.set_session(session_uri);
    *read_req.mutable_transaction() = selector;
    read_req.set_table("test_table");
    read_req.add_columns("int64_col");
    read_req.add_columns("string_col");
    for (int k : {1, 3, 5, 7}) {
      auto* list_val = read_req.mutable_key_set()->add_keys();
      list_val->add_values()->set_string_value(std::to_string(k));
    }
    read_req.mutable_partition_options()->set_max_partitions(2);

    spanner_api::PartitionResponse read_resp;
    GOOGLESQL_ASSERT_OK(PartitionRead(read_req, &read_resp));
    EXPECT_EQ(read_resp.partitions_size(), 2);

    std::set<int64_t> received_keys;
    for (int i = 0; i < read_resp.partitions_size(); ++i) {
      spanner_api::ReadRequest data_req;
      data_req.set_session(session_uri);
      *data_req.mutable_transaction() = selector;
      data_req.set_table("test_table");
      data_req.add_columns("int64_col");
      data_req.add_columns("string_col");
      for (int k : {1, 3, 5, 7}) {
        auto* list_val = data_req.mutable_key_set()->add_keys();
        list_val->add_values()->set_string_value(std::to_string(k));
      }
      data_req.set_partition_token(read_resp.partitions(i).partition_token());

      spanner_api::ResultSet data_resp;
      GOOGLESQL_ASSERT_OK(Read(data_req, &data_resp));
      EXPECT_EQ(data_resp.rows_size(), 2);
      for (const auto& row : data_resp.rows()) {
        int64_t key = std::stoll(row.values(0).string_value());
        EXPECT_TRUE(received_keys.insert(key).second) << "Duplicate key: " << key;
      }
    }
    EXPECT_EQ(received_keys.size(), 4);
    EXPECT_TRUE(received_keys.count(1));
    EXPECT_TRUE(received_keys.count(3));
    EXPECT_TRUE(received_keys.count(5));
    EXPECT_TRUE(received_keys.count(7));
  }
}

}  // namespace

}  // namespace frontend
}  // namespace emulator
}  // namespace spanner
}  // namespace google

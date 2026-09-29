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

#include <cstdint>
#include <set>
#include <string>

#include "google/protobuf/empty.pb.h"
#include "google/spanner/admin/database/v1/spanner_database_admin.pb.h"
#include "google/spanner/v1/spanner.pb.h"
#include "gmock/gmock.h"
#include "gtest/gtest.h"
#include "googlesql/base/testing/status_matchers.h"
#include "tests/common/proto_matchers.h"
#include "absl/container/flat_hash_set.h"
#include "absl/status/status.h"
#include "googlesql/base/status_macros.h"
#include "frontend/common/protos.h"
#include "frontend/common/uris.h"
#include "tests/common/test_env.h"

namespace google {
namespace spanner {
namespace emulator {
namespace frontend {

namespace {

using ::googlesql_base::testing::StatusIs;

namespace spanner_api = google::spanner::v1;

class SessionApiTest : public test::ServerTest {
 protected:
  void SetUp() override {
    GOOGLESQL_EXPECT_OK(CreateTestInstance());
    GOOGLESQL_EXPECT_OK(CreateTestDatabase());
  }

  spanner_api::Session response_;
  grpc::ClientContext context_;
};

TEST_F(SessionApiTest, CannotReadUsingExpiredTransactions) {
  // Create a test session.
  spanner_api::CreateSessionRequest request;
  request.set_database(test_database_uri_);
  GOOGLESQL_EXPECT_OK(test_env()->spanner_client()->CreateSession(&context_, request,
                                                        &response_));
  std::string test_sessions_uri = response_.name();
  EXPECT_THAT(test_sessions_uri,
              testing::HasSubstr(
                  "projects/test-project/instances/test-instance/databases/"
                  "test-database/sessions/"));

  // Begin 50 transactions in the same session. Don't use any of the
  // transactions started yet.
  backend::TransactionID id;
  for (int i = 0; i < 50; ++i) {
    spanner_api::BeginTransactionRequest txn_request = PARSE_TEXT_PROTO(R"(
      options { read_only {} }
    )");
    txn_request.set_session(test_sessions_uri);

    spanner_api::Transaction txn_response;
    GOOGLESQL_ASSERT_OK(BeginTransaction(txn_request, &txn_response));
    id = TransactionIDFromProto(txn_response.id());
  }

  // Build a read request to use transactions created above.
  spanner_api::ReadRequest read_request = PARSE_TEXT_PROTO(R"(
    table: "test_table"
    columns: "int64_col"
    columns: "string_col"
    key_set { all: true }
  )");
  *read_request.mutable_session() = test_sessions_uri;

  // A given session only tracks last 32 created transactions. Any older
  // transactions are deleted from the session object on the server. Thus,
  // attempt to use these older transactions will result in transaction not
  // found errors.
  spanner_api::TransactionSelector selector;
  spanner_api::ResultSet read_response;
  for (int i = 49; i >= 32; --i) {
    *selector.mutable_id() = std::to_string(id - i);
    *read_request.mutable_transaction() = selector;
    EXPECT_THAT(Read(read_request, &read_response),
                StatusIs(absl::StatusCode::kNotFound));
  }
}

TEST_F(SessionApiTest, CanBeginAndUseMultipleTransactionsInSameSession) {
  // Create a test session.
  spanner_api::CreateSessionRequest request;
  request.set_database(test_database_uri_);
  GOOGLESQL_EXPECT_OK(test_env()->spanner_client()->CreateSession(&context_, request,
                                                        &response_));
  std::string test_sessions_uri = response_.name();
  EXPECT_THAT(test_sessions_uri,
              testing::HasSubstr(
                  "projects/test-project/instances/test-instance/databases/"
                  "test-database/sessions/"));

  // Create 50 transactions in the test session created above, note that though
  // only the last 32 transactions are tracked by a single session. First 18
  // transactions will be invalidated.
  backend::TransactionID id;
  for (int i = 0; i < 50; ++i) {
    spanner_api::BeginTransactionRequest txn_request = PARSE_TEXT_PROTO(R"(
      options { read_only {} }
    )");
    txn_request.set_session(test_sessions_uri);

    spanner_api::Transaction txn_response;
    GOOGLESQL_ASSERT_OK(BeginTransaction(txn_request, &txn_response));
    id = TransactionIDFromProto(txn_response.id());
  }

  spanner_api::ReadRequest read_request = PARSE_TEXT_PROTO(R"(
    table: "test_table"
    columns: "int64_col"
    key_set {}
  )");
  read_request.set_session(test_sessions_uri);

  // Check that the last 32 created transactions exist in oldest to newest
  // order. There can only be one active transaction at a given time in a
  // given session, thus sequential read using same session for different
  // transactions should succeed.
  spanner_api::TransactionSelector selector;
  spanner_api::ResultSet read_response;
  for (int i = 31; i >= 0; --i) {
    *selector.mutable_id() = std::to_string(id - i);
    *read_request.mutable_transaction() = selector;
    GOOGLESQL_EXPECT_OK(Read(read_request, &read_response));
    EXPECT_THAT(read_response, test::EqualsProto(
                                   R"(metadata {
                                        row_type {
                                          fields {
                                            name: "int64_col"
                                            type { code: INT64 }
                                          }
                                        }
                                      })"));
  }

  // Trying to read a transaction older than the most recent 32 transactions
  // should return a failed precondition error since min transaction id that
  // can be a valid transaction id for the given session has moved to 50 with
  // the last read performed above.
  *selector.mutable_id() = std::to_string(id - 40);
  *read_request.mutable_transaction() = selector;
  EXPECT_THAT(Read(read_request, &read_response),
              StatusIs(absl::StatusCode::kFailedPrecondition));
}

TEST_F(SessionApiTest, SessionsUseExistingDatabaseRoles) {
  GOOGLESQL_ASSERT_OK(UpdateDatabaseDdl(test_database_uri_, {"CREATE ROLE reader"}));

  spanner_api::CreateSessionRequest request;
  request.set_database(test_database_uri_);
  request.mutable_session()->set_creator_role("reader");
  GOOGLESQL_ASSERT_OK(test_env()->spanner_client()->CreateSession(&context_, request,
                                                        &response_));
  EXPECT_EQ(response_.creator_role(), "reader");

  request.mutable_session()->set_creator_role("spanner_info_reader");
  grpc::ClientContext system_role_context;
  GOOGLESQL_EXPECT_OK(test_env()->spanner_client()->CreateSession(
      &system_role_context, request, &response_));

  request.mutable_session()->set_creator_role("nobody");
  grpc::ClientContext unknown_role_context;
  grpc::Status status = test_env()->spanner_client()->CreateSession(
      &unknown_role_context, request, &response_);
  EXPECT_EQ(status.error_code(), grpc::StatusCode::PERMISSION_DENIED);
  EXPECT_EQ(status.error_message(), "Role not found: nobody.");

  spanner_api::BatchCreateSessionsRequest batch_request;
  batch_request.set_database(test_database_uri_);
  batch_request.set_session_count(1);
  batch_request.mutable_session_template()->set_creator_role("nobody");
  spanner_api::BatchCreateSessionsResponse batch_response;
  grpc::ClientContext batch_context;
  status = test_env()->spanner_client()->BatchCreateSessions(
      &batch_context, batch_request, &batch_response);
  EXPECT_EQ(status.error_code(), grpc::StatusCode::PERMISSION_DENIED);
}

TEST_F(SessionApiTest, ListSessionsFiltersByLabels) {
  const auto create = [this](const std::string& env) -> std::string {
    spanner_api::CreateSessionRequest request;
    request.set_database(test_database_uri_);
    if (!env.empty()) {
      (*request.mutable_session()->mutable_labels())["env"] = env;
    }
    spanner_api::Session session;
    grpc::ClientContext context;
    EXPECT_TRUE(test_env()
                    ->spanner_client()
                    ->CreateSession(&context, request, &session)
                    .ok());
    return session.name();
  };
  const std::string dev = create("dev");
  const std::string dev2 = create("dev2");
  const std::string prod = create("prod");
  const std::string unlabeled = create("");

  const auto list = [this](const std::string& filter, int32_t page_size,
                           const std::string& page_token,
                           std::set<std::string>* names,
                           std::string* next_page_token) -> absl::Status {
    spanner_api::ListSessionsRequest request;
    request.set_database(test_database_uri_);
    request.set_filter(filter);
    request.set_page_size(page_size);
    request.set_page_token(page_token);
    spanner_api::ListSessionsResponse response;
    grpc::ClientContext context;
    GOOGLESQL_RETURN_IF_ERROR(test_env()->spanner_client()->ListSessions(
        &context, request, &response));
    names->clear();
    for (const auto& session : response.sessions()) {
      names->insert(session.name());
    }
    *next_page_token = response.next_page_token();
    return absl::OkStatus();
  };
  std::set<std::string> names;
  std::string next_page_token;
  GOOGLESQL_ASSERT_OK(list("labels.env:*", 0, "", &names, &next_page_token));
  EXPECT_EQ(names, (std::set<std::string>{dev, dev2, prod}));
  GOOGLESQL_ASSERT_OK(list("LABELS.ENV:DEV", 0, "", &names, &next_page_token));
  EXPECT_EQ(names, (std::set<std::string>{dev, dev2}));
  GOOGLESQL_ASSERT_OK(list("labels.env = dev OR labels.env = prod", 0, "",
                           &names, &next_page_token));
  EXPECT_EQ(names, (std::set<std::string>{dev, prod}));
  GOOGLESQL_ASSERT_OK(
      list("NOT labels.env:*", 0, "", &names, &next_page_token));
  EXPECT_EQ(names, (std::set<std::string>{unlabeled}));

  // Filtering happens before pagination, and the token keeps the filter.
  std::set<std::string> all;
  std::string page_token;
  do {
    GOOGLESQL_ASSERT_OK(
        list("labels.env:dev", 1, page_token, &names, &next_page_token));
    ASSERT_EQ(names.size(), 1);
    all.insert(names.begin(), names.end());
    page_token = next_page_token;
  } while (!page_token.empty());
  EXPECT_EQ(all, (std::set<std::string>{dev, dev2}));
  GOOGLESQL_ASSERT_OK(list("labels.env:dev", 1, "", &names, &next_page_token));
  EXPECT_THAT(list("labels.env:prod", 1, next_page_token, &names, &page_token),
              StatusIs(absl::StatusCode::kInvalidArgument));

  EXPECT_THAT(list("name:x", 0, "", &names, &next_page_token),
              StatusIs(absl::StatusCode::kInvalidArgument));
}

}  // namespace

}  // namespace frontend
}  // namespace emulator
}  // namespace spanner
}  // namespace google

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
#include <string>
#include <utility>
#include <vector>

#include "google/spanner/admin/database/v1/spanner_database_admin.pb.h"
#include "google/spanner/v1/commit_response.pb.h"
#include "google/spanner/v1/spanner.pb.h"
#include "gmock/gmock.h"
#include "gtest/gtest.h"
#include "googlesql/base/testing/status_matchers.h"
#include "tests/common/proto_matchers.h"
#include "absl/status/status.h"
#include "absl/status/statusor.h"
#include "absl/strings/str_cat.h"
#include "absl/strings/str_format.h"
#include "absl/strings/str_replace.h"
#include "absl/strings/string_view.h"
#include "absl/time/clock.h"
#include "absl/time/time.h"
#include "backend/datamodel/types.h"
#include "common/config.h"
#include "common/errors.h"
#include "frontend/converters/partition.h"
#include "frontend/converters/time.h"
#include "frontend/converters/types.h"
#include "frontend/converters/values.h"
#include "frontend/proto/partition_token.pb.h"
#include "frontend/proto/resume_token.pb.h"
#include "tests/common/chunking.h"
#include "tests/common/proto_matchers.h"
#include "tests/common/test_env.h"
#include "googlesql/base/status_macros.h"
#include "grpcpp/client_context.h"

namespace google {
namespace spanner {
namespace emulator {
namespace frontend {

namespace {

namespace spanner_api = ::google::spanner::v1;
namespace database_api = ::google::spanner::admin::database::v1;
namespace instance_api = ::google::spanner::admin::instance::v1;
namespace operations_api = ::google::longrunning;

using testing::ElementsAre;
using test::EqualsProto;
using testing::HasSubstr;
using test::proto::Partially;
using googlesql_base::testing::StatusIs;

enum class SessionType {
  kRegularSession,
  kMultiplexedSession,
};

class QueryApiTest : public test::ServerTest,
                     public testing::WithParamInterface<SessionType> {
 protected:
  void SetUp() override {
    GOOGLESQL_ASSERT_OK(CreateTestInstance());
    GOOGLESQL_ASSERT_OK(CreateTestDatabase());
    GOOGLESQL_ASSERT_OK_AND_ASSIGN(test_session_uri_,
                         CreateTestSession(/*multiplexed=*/false));
    GOOGLESQL_ASSERT_OK_AND_ASSIGN(test_multiplexed_session_uri_,
                         CreateTestSession(/*multiplexed=*/true));
    GOOGLESQL_ASSERT_OK(PopulateTestTable());
  }

  std::string GetSessionUri(bool multiplexed) {
    return multiplexed ? test_multiplexed_session_uri_ : test_session_uri_;
  }

  absl::Status PopulateTestTable() {
    spanner_api::CommitRequest commit_request = PARSE_TEXT_PROTO(R"(
      single_use_transaction { read_write {} }
      mutations {
        insert {
          table: "test_table"
          columns: "int64_col"
          columns: "string_col"
          values {
            values { string_value: "1" }
            values { string_value: "row_1" }
          }
          values {
            values { string_value: "2" }
            values { string_value: "row_2" }
          }
          values {
            values { string_value: "3" }
            values { string_value: "row_3" }
          }
        }
      }
    )");
    *commit_request.mutable_session() = test_session_uri_;

    spanner_api::CommitResponse commit_response;
    return Commit(commit_request, &commit_response);
  }

  absl::Status AddProtoTables() {
    grpc::ClientContext context;
    database_api::UpdateDatabaseDdlRequest request;
    request.set_database(test_database_uri_);
    request.add_statements(R"sql(
      CREATE PROTO BUNDLE (
        customer.app.User,
      )
    )sql");
    request.add_statements(R"sql(
      CREATE TABLE proto_table(
        int64_col INT64 NOT NULL,
        proto_col customer.app.User,
      ) PRIMARY KEY(int64_col)
    )sql");
    request.set_proto_descriptors(GenerateProtoDescriptorBytesAsString());
    operations_api::Operation operation;
    GOOGLESQL_RETURN_IF_ERROR(test_env()->database_admin_client()->UpdateDatabaseDdl(
        &context, request, &operation));
    GOOGLESQL_RETURN_IF_ERROR(WaitForOperation(operation.name(), &operation));
    google::rpc::Status status = operation.error();
    return absl::Status(static_cast<absl::StatusCode>(status.code()),
                        status.message());
  }

  absl::Status PopulateProtoTable() {
    // `int_field: 314` is encoded as CLoC
    // `int_field: 271` is encoded as CI8C
    spanner_api::CommitRequest commit_request = PARSE_TEXT_PROTO(R"pb(
      single_use_transaction { read_write {} }
      mutations {
        insert {
          table: "proto_table"
          columns: "int64_col"
          columns: "proto_col"
          values {
            values { string_value: "1" }
            values { string_value: "CLoC" }
          }
          values {
            values { string_value: "2" }
            values { string_value: "CI8C" }
          }
        }
      }
    )pb");
    *commit_request.mutable_session() = test_session_uri_;

    spanner_api::CommitResponse commit_response;
    return Commit(commit_request, &commit_response);
  }

  SessionType GetSessionType() { return GetParam(); }

  // Sends `request` with ExecuteSql and with ExecuteStreamingSql, each with a
  // call deadline `timeout` from now, and returns their statuses.
  std::vector<absl::Status> ExecuteSqlWithTimeout(
      const spanner_api::ExecuteSqlRequest& request, absl::Duration timeout) {
    std::vector<absl::Status> statuses;
    {
      grpc::ClientContext context;
      context.set_deadline(absl::ToChronoTime(absl::Now() + timeout));
      spanner_api::ResultSet response;
      statuses.push_back(test_env()->spanner_client()->ExecuteSql(
          &context, request, &response));
    }
    {
      grpc::ClientContext context;
      context.set_deadline(absl::ToChronoTime(absl::Now() + timeout));
      auto reader =
          test_env()->spanner_client()->ExecuteStreamingSql(&context, request);
      spanner_api::PartialResultSet response;
      while (reader->Read(&response)) {
      }
      statuses.push_back(reader->Finish());
    }
    return statuses;
  }

  std::string test_session_uri_;
  std::string test_multiplexed_session_uri_;

 private:
  std::string GenerateProtoDescriptorBytesAsString() {
    const google::protobuf::FileDescriptorProto file_descriptor = PARSE_TEXT_PROTO(R"pb(
      syntax: "proto2"
      name: "0"
      package: "customer.app"
      message_type {
        name: "User"
        field {
          name: "int_field"
          type: TYPE_INT64
          number: 1
          label: LABEL_OPTIONAL
        }
      }
      enum_type {
        name: "State"
        value { name: "UNSPECIFIED" number: 0 }
      }
    )pb");
    google::protobuf::FileDescriptorSet file_descriptor_set;
    *file_descriptor_set.add_file() = file_descriptor;
    return file_descriptor_set.SerializeAsString();
  }
};

INSTANTIATE_TEST_SUITE_P(SessionTypes, QueryApiTest,
                         testing::Values(SessionType::kRegularSession,
                                         SessionType::kMultiplexedSession));

TEST_P(QueryApiTest, ExecuteBatchDml) {
  spanner_api::BeginTransactionRequest begin_request = PARSE_TEXT_PROTO(R"(
    options { read_write {} }
  )");
  begin_request.set_session(
      GetSessionUri(GetSessionType() == SessionType::kMultiplexedSession));

  spanner_api::Transaction transaction_response;
  GOOGLESQL_EXPECT_OK(BeginTransaction(begin_request, &transaction_response));

  spanner_api::ExecuteBatchDmlRequest request = PARSE_TEXT_PROTO(
      R"""(
        statements {
          sql: "insert into test_table(int64_col, string_col) "
               "values (10, 'row_10')"
        }
        statements {
          sql: "insert into test_table(int64_col, string_col) "
               "values (11, 'row_11')"
        }
      )""");
  request.set_session(
      GetSessionUri(GetSessionType() == SessionType::kMultiplexedSession));
  request.mutable_transaction()->set_id(transaction_response.id());

  spanner_api::ExecuteBatchDmlResponse response;
  GOOGLESQL_ASSERT_OK(ExecuteBatchDml(request, &response));
  EXPECT_THAT(response, Partially(EqualsProto(
                            R"pb(
                              result_sets {
                                metadata { row_type {} }
                                stats { row_count_exact: 1 }
                              }
                              result_sets { stats { row_count_exact: 1 } }
                              status { code: 0 }
                            )pb")));
  if (GetSessionType() == SessionType::kMultiplexedSession) {
    ASSERT_TRUE(response.has_precommit_token());
  }

  spanner_api::CommitRequest commit_request;
  commit_request.set_transaction_id(transaction_response.id());
  commit_request.set_session(
      GetSessionUri(GetSessionType() == SessionType::kMultiplexedSession));
  if (GetSessionType() == SessionType::kMultiplexedSession) {
    *commit_request.mutable_precommit_token() = response.precommit_token();
  }

  spanner_api::CommitResponse commit_response1;
  GOOGLESQL_EXPECT_OK(Commit(commit_request, &commit_response1));
}

TEST_P(QueryApiTest, ExecuteBatchDmlWithProtos) {
  GOOGLESQL_ASSERT_OK(AddProtoTables());

  spanner_api::BeginTransactionRequest begin_request = PARSE_TEXT_PROTO(R"pb(
    options { read_write {} }
  )pb");
  begin_request.set_session(
      GetSessionUri(GetSessionType() == SessionType::kMultiplexedSession));

  spanner_api::Transaction transaction_response;
  GOOGLESQL_EXPECT_OK(BeginTransaction(begin_request, &transaction_response));

  spanner_api::ExecuteBatchDmlRequest request = PARSE_TEXT_PROTO(
      R"""(
        statements {
          sql: "insert into proto_table(int64_col, proto_col) "
               "values (10, 'int_field: 314')"
        }
        statements {
          sql: "insert into proto_table(int64_col, proto_col) "
               "values (11, 'int_field: 271')"
        }
      )""");
  request.set_session(
      GetSessionUri(GetSessionType() == SessionType::kMultiplexedSession));
  request.mutable_transaction()->set_id(transaction_response.id());

  spanner_api::ExecuteBatchDmlResponse response;
  GOOGLESQL_ASSERT_OK(ExecuteBatchDml(request, &response));
  if (GetSessionType() == SessionType::kMultiplexedSession) {
    ASSERT_TRUE(response.has_precommit_token());
  }
  ASSERT_THAT(response, Partially(EqualsProto(
                            R"pb(
                              result_sets {
                                metadata { row_type {} }
                                stats { row_count_exact: 1 }
                              }
                              result_sets { stats { row_count_exact: 1 } }
                              status { code: 0 }
                            )pb")));
}

TEST_P(QueryApiTest, ExecuteBatchDmlFailsOnInvalidDmlStatement) {
  spanner_api::BeginTransactionRequest begin_request = PARSE_TEXT_PROTO(R"(
    options { read_write {} }
  )");
  begin_request.set_session(
      GetSessionUri(GetSessionType() == SessionType::kMultiplexedSession));

  spanner_api::Transaction transaction_response;
  GOOGLESQL_EXPECT_OK(BeginTransaction(begin_request, &transaction_response));

  spanner_api::ExecuteBatchDmlRequest request = PARSE_TEXT_PROTO(
      R"""(
        statements {
          sql: "insert into test_table(int64_col, string_col) "
               "values (10, 'row_10')"
        }
        statements {
          sql: "insert into test_table(int64_t, string) "
               "values (11, 'row_11')"
        }
      )""");
  request.set_session(
      GetSessionUri(GetSessionType() == SessionType::kMultiplexedSession));
  request.mutable_transaction()->set_id(transaction_response.id());

  spanner_api::ExecuteBatchDmlResponse response;
  GOOGLESQL_ASSERT_OK(ExecuteBatchDml(request, &response));
  // Ignoring the status.message field to avoid brittle tests.
  EXPECT_THAT(response, Partially(EqualsProto(
                            R"(
                              result_sets { stats { row_count_exact: 1 } }
                              status { code: 3 }
                            )")));
}

TEST_P(QueryApiTest, ExecuteSql) {
  spanner_api::ExecuteSqlRequest request = PARSE_TEXT_PROTO(
      R"(
        transaction { single_use { read_only { strong: true } } }
        sql: "SELECT int64_col, string_col FROM test_table "
             "ORDER BY int64_col ASC, string_col DESC"
      )");
  request.set_session(
      GetSessionUri(GetSessionType() == SessionType::kMultiplexedSession));

  spanner_api::ResultSet response;
  GOOGLESQL_ASSERT_OK(ExecuteSql(request, &response));
  if (GetSessionType() == SessionType::kMultiplexedSession) {
    // No precommit token for single use transactions.
    ASSERT_FALSE(response.has_precommit_token());
  }
  EXPECT_THAT(response, Partially(EqualsProto(
                            R"pb(
                              metadata {
                                row_type {
                                  fields {
                                    name: "int64_col"
                                    type { code: INT64 }
                                  }
                                  fields {
                                    name: "string_col"
                                    type { code: STRING }
                                  }
                                }
                              }
                              rows {
                                values { string_value: "1" }
                                values { string_value: "row_1" }
                              }
                              rows {
                                values { string_value: "2" }
                                values { string_value: "row_2" }
                              }
                              rows {
                                values { string_value: "3" }
                                values { string_value: "row_3" }
                              }
                            )pb")));
}

TEST_P(QueryApiTest, FutureReadTimestampPastCallDeadlineFailsAtOnce) {
  // A query at a future timestamp waits for it. When the call's deadline comes
  // first, the query fails right away instead of waiting past the deadline.
  const std::string session_uri =
      GetSessionUri(GetSessionType() == SessionType::kMultiplexedSession);
  GOOGLESQL_ASSERT_OK_AND_ASSIGN(auto read_timestamp,
                       TimestampToProto(absl::Now() + absl::Minutes(1)));
  spanner_api::BeginTransactionRequest begin_request;
  begin_request.set_session(session_uri);
  *begin_request.mutable_options()
       ->mutable_read_only()
       ->mutable_read_timestamp() = read_timestamp;
  spanner_api::Transaction txn;
  GOOGLESQL_ASSERT_OK(BeginTransaction(begin_request, &txn));

  // The begun transaction comes first: beginning another one in a regular
  // session ends it.
  std::vector<spanner_api::TransactionSelector> selectors(3);
  selectors[0].set_id(txn.id());
  *selectors[1].mutable_single_use()->mutable_read_only()
       ->mutable_read_timestamp() = read_timestamp;
  *selectors[2].mutable_begin()->mutable_read_only()
       ->mutable_read_timestamp() = read_timestamp;
  for (const spanner_api::TransactionSelector& selector : selectors) {
    SCOPED_TRACE(selector.DebugString());
    spanner_api::ExecuteSqlRequest request;
    request.set_session(session_uri);
    request.set_sql("SELECT int64_col FROM test_table");
    *request.mutable_transaction() = selector;

    const absl::Time start = absl::Now();
    EXPECT_THAT(ExecuteSqlWithTimeout(request, absl::Seconds(10)),
                testing::Each(StatusIs(absl::StatusCode::kDeadlineExceeded,
                                       HasSubstr("request deadline"))));
    EXPECT_LT(absl::Now() - start, absl::Seconds(10));
  }
}

TEST_P(QueryApiTest, FutureReadTimestampBeforeCallDeadlineWaits) {
  const absl::Time read_time = absl::Now() + absl::Milliseconds(200);
  GOOGLESQL_ASSERT_OK_AND_ASSIGN(auto read_timestamp,
                                 TimestampToProto(read_time));
  spanner_api::ExecuteSqlRequest request;
  request.set_session(
      GetSessionUri(GetSessionType() == SessionType::kMultiplexedSession));
  request.set_sql("SELECT int64_col FROM test_table");
  *request.mutable_transaction()->mutable_single_use()->mutable_read_only()
       ->mutable_read_timestamp() = read_timestamp;

  EXPECT_THAT(ExecuteSqlWithTimeout(request, absl::Seconds(30)),
              testing::Each(StatusIs(absl::StatusCode::kOk)));
  EXPECT_GE(absl::Now(), read_time);
}

TEST_P(QueryApiTest, ExecuteSqlDataBoostEnabledMissingPartitionTokenFails) {
  spanner_api::ExecuteSqlRequest request = PARSE_TEXT_PROTO(
      R"pb(
        transaction { single_use { read_only { strong: true } } }
        sql: "SELECT int64_col, string_col FROM test_table"
        data_boost_enabled: true
      )pb");
  request.set_session(
      GetSessionUri(GetSessionType() == SessionType::kMultiplexedSession));

  spanner_api::ResultSet response;
  EXPECT_THAT(ExecuteSql(request, &response),
              StatusIs(absl::StatusCode::kInvalidArgument,
                       "Data Boost is only valid for partitioned queries or "
                       "reads, and requires a partition token."));
}

TEST_P(QueryApiTest, ExecuteSqlDataBoostEnabledWithPartitionTokenSucceeds) {
  spanner_api::PartitionQueryRequest partition_request;
  partition_request.set_session(
      GetSessionUri(GetSessionType() == SessionType::kMultiplexedSession));
  partition_request.mutable_transaction()
      ->mutable_begin()
      ->mutable_read_only()
      ->set_strong(true);
  partition_request.set_sql("SELECT int64_col, string_col FROM test_table");

  spanner_api::PartitionResponse partition_response;
  grpc::ClientContext context;
  GOOGLESQL_ASSERT_OK(test_env()->spanner_client()->PartitionQuery(
      &context, partition_request, &partition_response));
  ASSERT_GT(partition_response.partitions().size(), 0);

  // The emulator returns an empty partition and a full partition. To be robust
  // against future changes in the number or order of partitions, we iterate
  // over all partitions and use the first one that returns rows when executed.
  std::string valid_token;
  for (const auto& partition : partition_response.partitions()) {
    spanner_api::ExecuteSqlRequest test_request;
    test_request.set_session(
        GetSessionUri(GetSessionType() == SessionType::kMultiplexedSession));
    test_request.mutable_transaction()->set_id(
        partition_response.transaction().id());
    test_request.set_sql("SELECT int64_col, string_col FROM test_table");
    test_request.set_partition_token(partition.partition_token());

    spanner_api::ResultSet test_response;
    if (ExecuteSql(test_request, &test_response).ok() &&
        test_response.rows_size() > 0) {
      valid_token = partition.partition_token();
      break;
    }
  }
  ASSERT_FALSE(valid_token.empty()) << "No non-empty partition found";

  spanner_api::ExecuteSqlRequest request;
  request.set_session(
      GetSessionUri(GetSessionType() == SessionType::kMultiplexedSession));
  request.mutable_transaction()->set_id(partition_response.transaction().id());
  request.set_sql("SELECT int64_col, string_col FROM test_table");
  request.set_partition_token(valid_token);
  request.set_data_boost_enabled(true);

  spanner_api::ResultSet response;
  GOOGLESQL_EXPECT_OK(ExecuteSql(request, &response));
  EXPECT_THAT(response, Partially(EqualsProto(
                            R"pb(
                              rows {
                                values { string_value: "2" }
                                values { string_value: "row_2" }
                              }
                              rows {
                                values { string_value: "1" }
                                values { string_value: "row_1" }
                              }
                              rows {
                                values { string_value: "3" }
                                values { string_value: "row_3" }
                              }
                            )pb")));
}

TEST_P(QueryApiTest, ExecuteSqlWithParameters) {
  spanner_api::ExecuteSqlRequest request = PARSE_TEXT_PROTO(
      R"(
        transaction { single_use { read_only { strong: true } } }
        sql: "SELECT @param AS param FROM test_table"
        params {
          fields {
            key: "param"
            value { string_value: "value" }
          }
        }
        param_types {
          key: "param"
          value { code: STRING }
        }
      )");
  request.set_session(
      GetSessionUri(GetSessionType() == SessionType::kMultiplexedSession));

  spanner_api::ResultSet response;
  GOOGLESQL_ASSERT_OK(ExecuteSql(request, &response));
  if (GetSessionType() == SessionType::kMultiplexedSession) {
    // no precommit token for single use transactions.
    ASSERT_FALSE(response.has_precommit_token());
  }
  EXPECT_THAT(response, Partially(EqualsProto(
                            R"pb(
                              metadata {
                                row_type {
                                  fields {
                                    name: "param"
                                    type { code: STRING }
                                  }
                                }
                                undeclared_parameters {
                                  fields {
                                    name: "param"
                                    type { code: STRING }
                                  }
                                }
                              }
                              rows { values { string_value: "value" } }
                              rows { values { string_value: "value" } }
                              rows { values { string_value: "value" } }
                            )pb")));
}

TEST_P(QueryApiTest, ExecuteSqlWithProtoParameters) {
  GOOGLESQL_ASSERT_OK(AddProtoTables());

  spanner_api::ExecuteSqlRequest request = PARSE_TEXT_PROTO(
      R"pb(
        transaction { single_use { read_only { strong: true } } }
        sql: "SELECT @param.int_field AS intval FROM test_table"
        params {
          fields {
            key: "param"
            value { string_value: "CI8C" }
          }
        }
        param_types {
          key: "param"
          value { code: PROTO proto_type_fqn: "customer.app.User" }
        }
      )pb");
  request.set_session(
      GetSessionUri(GetSessionType() == SessionType::kMultiplexedSession));

  spanner_api::ResultSet response;
  GOOGLESQL_ASSERT_OK(ExecuteSql(request, &response));
  if (GetSessionType() == SessionType::kMultiplexedSession) {
    // no precommit token for single use transactions.
    ASSERT_FALSE(response.has_precommit_token());
  }
  EXPECT_THAT(
      response,
      EqualsProto(
          R"pb(
            metadata {
              row_type {
                fields {
                  name: "intval"
                  type { code: INT64 }
                }
              }
              undeclared_parameters {
                fields {
                  name: "param"
                  type { code: PROTO proto_type_fqn: "customer.app.User" }
                }
              }
            }
            rows { values { string_value: "271" } }
            rows { values { string_value: "271" } }
            rows { values { string_value: "271" } }
          )pb"));
}

TEST_P(QueryApiTest, ExecuteSqlWithDmlAndParameters) {
  spanner_api::BeginTransactionRequest begin_request = PARSE_TEXT_PROTO(R"pb(
    options { read_write {} }
  )pb");
  begin_request.set_session(
      GetSessionUri(GetSessionType() == SessionType::kMultiplexedSession));

  spanner_api::Transaction transaction_response;
  GOOGLESQL_EXPECT_OK(BeginTransaction(begin_request, &transaction_response));

  spanner_api::ExecuteSqlRequest request = PARSE_TEXT_PROTO(
      R"""(
        sql: "INSERT INTO test_table (int64_col, string_col) "
             "VALUES (@p1, @p2)"
      )""");
  request.set_session(
      GetSessionUri(GetSessionType() == SessionType::kMultiplexedSession));
  request.set_query_mode(spanner_api::ExecuteSqlRequest::PLAN);
  request.mutable_transaction()->set_id(transaction_response.id());

  spanner_api::ResultSet response;
  GOOGLESQL_ASSERT_OK(ExecuteSql(request, &response));
  if (GetSessionType() == SessionType::kMultiplexedSession) {
    ASSERT_TRUE(response.has_precommit_token());
  }
  EXPECT_THAT(
      response,
      Partially(EqualsProto(
          R"pb(
            metadata {
              row_type {}
              undeclared_parameters {
                fields {
                  name: "p1"
                  type { code: INT64 }
                }
                fields {
                  name: "p2"
                  type { code: STRING }
                }
              }
            }
            stats {
              row_count_exact: 0
            }
          )pb")));
  EXPECT_EQ(response.stats().query_plan().plan_nodes(0).display_name(),
            "Apply Mutations");
}

TEST_P(QueryApiTest, ExecuteSqlWithDmlReturningAndParameters) {
  spanner_api::BeginTransactionRequest begin_request = PARSE_TEXT_PROTO(R"pb(
    options { read_write {} }
  )pb");
  begin_request.set_session(
      GetSessionUri(GetSessionType() == SessionType::kMultiplexedSession));

  spanner_api::Transaction transaction_response;
  GOOGLESQL_EXPECT_OK(BeginTransaction(begin_request, &transaction_response));

  spanner_api::ExecuteSqlRequest request = PARSE_TEXT_PROTO(
      R"""(
        sql: "INSERT INTO test_table (int64_col, string_col) "
             "VALUES (@p1, @p2) THEN RETURN int64_col, string_col"
      )""");
  request.set_session(
      GetSessionUri(GetSessionType() == SessionType::kMultiplexedSession));
  request.set_query_mode(spanner_api::ExecuteSqlRequest::PLAN);
  request.mutable_transaction()->set_id(transaction_response.id());

  spanner_api::ResultSet response;
  GOOGLESQL_ASSERT_OK(ExecuteSql(request, &response));
  if (GetSessionType() == SessionType::kMultiplexedSession) {
    ASSERT_TRUE(response.has_precommit_token());
  }
  EXPECT_THAT(
      response,
      Partially(EqualsProto(
          R"pb(
            metadata {
              row_type {
                fields {
                  name: "int64_col"
                  type { code: INT64 }
                }
                fields {
                  name: "string_col"
                  type { code: STRING }
                }
              }
              undeclared_parameters {
                fields {
                  name: "p1"
                  type { code: INT64 }
                }
                fields {
                  name: "p2"
                  type { code: STRING }
                }
              }
            }
            stats {
              row_count_exact: 0
            }
          )pb")));
  EXPECT_EQ(response.stats().query_plan().plan_nodes(0).display_name(),
            "Apply Mutations");
}

TEST_P(QueryApiTest, ExecuteSqlWithDmlReturningReturnsStats) {
  spanner_api::BeginTransactionRequest begin_request = PARSE_TEXT_PROTO(R"pb(
    options { read_write {} }
  )pb");
  begin_request.set_session(
      GetSessionUri(GetSessionType() == SessionType::kMultiplexedSession));

  spanner_api::Transaction transaction_response;
  GOOGLESQL_EXPECT_OK(BeginTransaction(begin_request, &transaction_response));

  spanner_api::ExecuteSqlRequest request = PARSE_TEXT_PROTO(
      R"""(
        sql: "INSERT INTO test_table (int64_col, string_col) "
             "VALUES (10, 'row_10') THEN RETURN int64_col, string_col"
      )""");
  request.set_session(
      GetSessionUri(GetSessionType() == SessionType::kMultiplexedSession));
  request.mutable_transaction()->set_id(transaction_response.id());

  spanner_api::ResultSet response;
  GOOGLESQL_ASSERT_OK(ExecuteSql(request, &response));
  if (GetSessionType() == SessionType::kMultiplexedSession) {
    ASSERT_TRUE(response.has_precommit_token());
  }
  EXPECT_THAT(response, Partially(EqualsProto(
                            R"pb(
                              metadata {
                                row_type {
                                  fields {
                                    name: "int64_col"
                                    type { code: INT64 }
                                  }
                                  fields {
                                    name: "string_col"
                                    type { code: STRING }
                                  }
                                }
                              }
                              rows {
                                values { string_value: "10" }
                                values { string_value: "row_10" }
                              }
                              stats { row_count_exact: 1 }
                            )pb")));
}

TEST_P(QueryApiTest, ExecuteStreamingSqlWithDmlReturningReturnsStats) {
  spanner_api::BeginTransactionRequest begin_request = PARSE_TEXT_PROTO(R"pb(
    options { read_write {} }
  )pb");
  begin_request.set_session(
      GetSessionUri(GetSessionType() == SessionType::kMultiplexedSession));

  spanner_api::Transaction transaction_response;
  GOOGLESQL_EXPECT_OK(BeginTransaction(begin_request, &transaction_response));

  spanner_api::ExecuteSqlRequest request = PARSE_TEXT_PROTO(
      R"""(
        sql: "INSERT INTO test_table (int64_col, string_col) "
             "VALUES (10, 'row_10') THEN RETURN int64_col, string_col"
      )""");
  request.set_session(
      GetSessionUri(GetSessionType() == SessionType::kMultiplexedSession));
  request.mutable_transaction()->set_id(transaction_response.id());

  std::vector<spanner_api::PartialResultSet> response;
  GOOGLESQL_EXPECT_OK(ExecuteStreamingSql(request, &response));
  if (GetSessionType() == SessionType::kMultiplexedSession) {
    ASSERT_TRUE(response[0].has_precommit_token());
  }
  EXPECT_THAT(response, ElementsAre(Partially(EqualsProto(
                            R"pb(metadata {
                                   row_type {
                                     fields {
                                       name: "int64_col"
                                       type { code: INT64 }
                                     }
                                     fields {
                                       name: "string_col"
                                       type { code: STRING }
                                     }
                                   }
                                 }
                                 values { string_value: "10" }
                                 values { string_value: "row_10" }
                                 chunked_value: false
                                 stats { row_count_exact: 1 }
                            )pb"))));
}

TEST_P(QueryApiTest,
       ExecuteStreamingSqlWithDmlReturningInPlanModeReturnsEmptyStats) {
  spanner_api::BeginTransactionRequest begin_request = PARSE_TEXT_PROTO(R"pb(
    options { read_write {} }
  )pb");
  begin_request.set_session(
      GetSessionUri(GetSessionType() == SessionType::kMultiplexedSession));

  spanner_api::Transaction transaction_response;
  GOOGLESQL_EXPECT_OK(BeginTransaction(begin_request, &transaction_response));

  spanner_api::ExecuteSqlRequest request = PARSE_TEXT_PROTO(
      R"""(
        sql: "INSERT INTO test_table (int64_col, string_col) "
             "VALUES (10, 'row_10') THEN RETURN int64_col, string_col"
      )""");
  request.set_session(
      GetSessionUri(GetSessionType() == SessionType::kMultiplexedSession));
  request.set_query_mode(spanner_api::ExecuteSqlRequest::PLAN);
  request.mutable_transaction()->set_id(transaction_response.id());

  std::vector<spanner_api::PartialResultSet> response;
  GOOGLESQL_EXPECT_OK(ExecuteStreamingSql(request, &response));
  if (GetSessionType() == SessionType::kMultiplexedSession) {
    ASSERT_TRUE(response[0].has_precommit_token());
  }
  EXPECT_THAT(response, ElementsAre(Partially(EqualsProto(
                            R"pb(metadata {
                                   row_type {
                                     fields {
                                       name: "int64_col"
                                       type { code: INT64 }
                                     }
                                     fields {
                                       name: "string_col"
                                       type { code: STRING }
                                     }
                                   }
                                 }
                                 chunked_value: false
                                 stats { row_count_exact: 0 }
                            )pb"))));
}

TEST_P(QueryApiTest, ExecuteSqlWithDmlReturningStar) {
  spanner_api::BeginTransactionRequest begin_request = PARSE_TEXT_PROTO(R"pb(
    options { read_write {} }
  )pb");
  begin_request.set_session(
      GetSessionUri(GetSessionType() == SessionType::kMultiplexedSession));

  spanner_api::Transaction transaction_response;
  GOOGLESQL_EXPECT_OK(BeginTransaction(begin_request, &transaction_response));

  spanner_api::ExecuteSqlRequest request = PARSE_TEXT_PROTO(
      R"""(
        sql: "DELETE test_table WHERE TRUE THEN RETURN *"
      )""");
  request.set_session(
      GetSessionUri(GetSessionType() == SessionType::kMultiplexedSession));
  request.set_query_mode(spanner_api::ExecuteSqlRequest::PLAN);
  request.mutable_transaction()->set_id(transaction_response.id());

  spanner_api::ResultSet response;
  GOOGLESQL_ASSERT_OK(ExecuteSql(request, &response));
  if (GetSessionType() == SessionType::kMultiplexedSession) {
    ASSERT_TRUE(response.has_precommit_token());
  }
  EXPECT_THAT(
      response,
      Partially(EqualsProto(
          R"pb(
            metadata {
              row_type {
                fields {
                  name: "int64_col"
                  type { code: INT64 }
                }
                fields {
                  name: "string_col"
                  type { code: STRING }
                }
              }
            }
            stats {
              row_count_exact: 0
            }
          )pb")));
  EXPECT_EQ(response.stats().query_plan().plan_nodes(0).display_name(),
            "Apply Mutations");
}

TEST_P(QueryApiTest, ExecuteSqlUpdateReturning) {
  spanner_api::BeginTransactionRequest begin_request = PARSE_TEXT_PROTO(R"pb(
    options { read_write {} }
  )pb");
  begin_request.set_session(
      GetSessionUri(GetSessionType() == SessionType::kMultiplexedSession));

  spanner_api::Transaction transaction_response;
  GOOGLESQL_EXPECT_OK(BeginTransaction(begin_request, &transaction_response));

  spanner_api::ExecuteSqlRequest request = PARSE_TEXT_PROTO(
      R"""(
        sql: "UPDATE test_table SET string_col=@p1 "
             "WHERE int64_col=@p2 THEN RETURN string_col"
      )""");
  request.set_session(
      GetSessionUri(GetSessionType() == SessionType::kMultiplexedSession));
  request.set_query_mode(spanner_api::ExecuteSqlRequest::PLAN);
  request.mutable_transaction()->set_id(transaction_response.id());

  spanner_api::ResultSet response;
  GOOGLESQL_ASSERT_OK(ExecuteSql(request, &response));
  if (GetSessionType() == SessionType::kMultiplexedSession) {
    ASSERT_TRUE(response.has_precommit_token());
  }
  EXPECT_THAT(
      response,
      Partially(EqualsProto(
          R"pb(
            metadata {
              row_type {
                fields {
                  name: "string_col"
                  type { code: STRING }
                }
              }
              undeclared_parameters {
                fields {
                  name: "p1"
                  type { code: STRING }
                }
                fields {
                  name: "p2"
                  type { code: INT64 }
                }
              }
            }
            stats {
              row_count_exact: 0
            }
          )pb")));
  EXPECT_EQ(response.stats().query_plan().plan_nodes(0).display_name(),
            "Apply Mutations");
}

TEST_P(QueryApiTest, ExecuteSqlDmlPlanWithoutReturning) {
  spanner_api::BeginTransactionRequest begin_request = PARSE_TEXT_PROTO(R"pb(
    options { read_write {} }
  )pb");
  begin_request.set_session(
      GetSessionUri(GetSessionType() == SessionType::kMultiplexedSession));

  spanner_api::Transaction transaction_response;
  GOOGLESQL_EXPECT_OK(BeginTransaction(begin_request, &transaction_response));

  spanner_api::ExecuteSqlRequest request = PARSE_TEXT_PROTO(
      R"""(
        sql: "UPDATE test_table SET string_col=@p1 "
             "WHERE int64_col=@p2"
      )""");
  request.set_session(
      GetSessionUri(GetSessionType() == SessionType::kMultiplexedSession));
  request.set_query_mode(spanner_api::ExecuteSqlRequest::PLAN);
  request.mutable_transaction()->set_id(transaction_response.id());

  spanner_api::ResultSet response;
  GOOGLESQL_ASSERT_OK(ExecuteSql(request, &response));
  if (GetSessionType() == SessionType::kMultiplexedSession) {
    ASSERT_TRUE(response.has_precommit_token());
  }
  EXPECT_THAT(
      response,
      Partially(EqualsProto(
          R"pb(
            metadata {
              row_type {}
              undeclared_parameters {
                fields {
                  name: "p1"
                  type { code: STRING }
                }
                fields {
                  name: "p2"
                  type { code: INT64 }
                }
              }
            }
            stats {
              row_count_exact: 0
            }
          )pb")));
  EXPECT_EQ(response.stats().query_plan().plan_nodes(0).display_name(),
            "Apply Mutations");
}

TEST_P(QueryApiTest, ExecuteSqlWithDmlAndProtoParameters) {
  GOOGLESQL_ASSERT_OK(AddProtoTables());

  spanner_api::BeginTransactionRequest begin_request = PARSE_TEXT_PROTO(R"pb(
    options { read_write {} }
  )pb");
  begin_request.set_session(
      GetSessionUri(GetSessionType() == SessionType::kMultiplexedSession));

  spanner_api::Transaction transaction_response;
  GOOGLESQL_EXPECT_OK(BeginTransaction(begin_request, &transaction_response));

  spanner_api::ExecuteSqlRequest request = PARSE_TEXT_PROTO(
      R"""(
        sql: "INSERT INTO proto_table (int64_col, proto_col) "
             "VALUES (@p1, @p2)"
      )""");
  request.set_session(
      GetSessionUri(GetSessionType() == SessionType::kMultiplexedSession));
  request.set_query_mode(spanner_api::ExecuteSqlRequest::PLAN);
  request.mutable_transaction()->set_id(transaction_response.id());

  spanner_api::ResultSet response;
  GOOGLESQL_ASSERT_OK(ExecuteSql(request, &response));
  if (GetSessionType() == SessionType::kMultiplexedSession) {
    ASSERT_TRUE(response.has_precommit_token());
  }
  EXPECT_THAT(
      response,
      Partially(EqualsProto(
          R"pb(
            metadata {
              row_type {}
              undeclared_parameters {
                fields {
                  name: "p1"
                  type { code: INT64 }
                }
                fields {
                  name: "p2"
                  type { code: PROTO proto_type_fqn: "customer.app.User" }
                }
              }
            }
            stats {
              row_count_exact: 0
            }
          )pb")));
  EXPECT_EQ(response.stats().query_plan().plan_nodes(0).display_name(),
            "Apply Mutations");
}

TEST_P(QueryApiTest, ExecuteStreamingSql) {
  spanner_api::ExecuteSqlRequest request = PARSE_TEXT_PROTO(
      R"(
        transaction { single_use { read_only { strong: true } } }
        sql: "SELECT int64_col, string_col FROM test_table "
             "ORDER BY int64_col ASC, string_col DESC"
      )");
  request.set_session(
      GetSessionUri(GetSessionType() == SessionType::kMultiplexedSession));

  std::vector<spanner_api::PartialResultSet> response;
  GOOGLESQL_EXPECT_OK(ExecuteStreamingSql(request, &response));
  if (GetSessionType() == SessionType::kMultiplexedSession) {
    ASSERT_FALSE(response.back().has_precommit_token());
  }
  EXPECT_THAT(response, ElementsAre(Partially(EqualsProto(
                            R"pb(metadata {
                                   row_type {
                                     fields {
                                       name: "int64_col"
                                       type { code: INT64 }
                                     }
                                     fields {
                                       name: "string_col"
                                       type { code: STRING }
                                     }
                                   }
                                 }
                                 values { string_value: "1" }
                                 values { string_value: "row_1" }
                                 values { string_value: "2" }
                                 values { string_value: "row_2" }
                                 values { string_value: "3" }
                                 values { string_value: "row_3" }
                                 chunked_value: false
                            )pb"))));
}

TEST_P(QueryApiTest,
       ExecuteStreamingSqlDataBoostEnabledMissingPartitionTokenFails) {
  spanner_api::ExecuteSqlRequest request = PARSE_TEXT_PROTO(
      R"pb(
        transaction { single_use { read_only { strong: true } } }
        sql: "SELECT int64_col, string_col FROM test_table"
        data_boost_enabled: true
      )pb");
  request.set_session(
      GetSessionUri(GetSessionType() == SessionType::kMultiplexedSession));

  std::vector<spanner_api::PartialResultSet> response;
  EXPECT_THAT(ExecuteStreamingSql(request, &response),
              StatusIs(absl::StatusCode::kInvalidArgument,
                       "Data Boost is only valid for partitioned queries or "
                       "reads, and requires a partition token."));
}

TEST_P(QueryApiTest,
       ExecuteStreamingSqlDataBoostEnabledWithPartitionTokenSucceeds) {
  spanner_api::PartitionQueryRequest partition_request;
  partition_request.set_session(
      GetSessionUri(GetSessionType() == SessionType::kMultiplexedSession));
  partition_request.mutable_transaction()
      ->mutable_begin()
      ->mutable_read_only()
      ->set_strong(true);
  partition_request.set_sql("SELECT int64_col, string_col FROM test_table");

  spanner_api::PartitionResponse partition_response;
  grpc::ClientContext context;
  GOOGLESQL_ASSERT_OK(test_env()->spanner_client()->PartitionQuery(
      &context, partition_request, &partition_response));
  ASSERT_GT(partition_response.partitions().size(), 0);

  // The emulator returns an empty partition and a full partition. To be robust
  // against future changes in the number or order of partitions, we iterate
  // over all partitions and use the first one that returns rows when executed.
  std::string valid_token;
  for (const auto& partition : partition_response.partitions()) {
    spanner_api::ExecuteSqlRequest test_request;
    test_request.set_session(
        GetSessionUri(GetSessionType() == SessionType::kMultiplexedSession));
    test_request.mutable_transaction()->set_id(
        partition_response.transaction().id());
    test_request.set_sql("SELECT int64_col, string_col FROM test_table");
    test_request.set_partition_token(partition.partition_token());

    std::vector<spanner_api::PartialResultSet> test_response;
    if (ExecuteStreamingSql(test_request, &test_response).ok()) {
      bool has_rows = false;
      for (const auto& r : test_response) {
        if (r.values_size() > 0) {
          has_rows = true;
          break;
        }
      }
      if (has_rows) {
        valid_token = partition.partition_token();
        break;
      }
    }
  }
  ASSERT_FALSE(valid_token.empty()) << "No non-empty partition found";

  spanner_api::ExecuteSqlRequest request;
  request.set_session(
      GetSessionUri(GetSessionType() == SessionType::kMultiplexedSession));
  request.mutable_transaction()->set_id(partition_response.transaction().id());
  request.set_sql("SELECT int64_col, string_col FROM test_table");
  request.set_partition_token(valid_token);
  request.set_data_boost_enabled(true);

  std::vector<spanner_api::PartialResultSet> response;
  GOOGLESQL_EXPECT_OK(ExecuteStreamingSql(request, &response));
  EXPECT_THAT(response, ElementsAre(Partially(EqualsProto(
                            R"pb(
                              values { string_value: "2" }
                              values { string_value: "row_2" }
                              values { string_value: "1" }
                              values { string_value: "row_1" }
                              values { string_value: "3" }
                              values { string_value: "row_3" }
                            )pb"))));
}

TEST_P(QueryApiTest, ExecuteStreamingSqlWithParameters) {
  spanner_api::ExecuteSqlRequest request = PARSE_TEXT_PROTO(
      R"(
        transaction { single_use { read_only { strong: true } } }
        sql: "SELECT @param AS param FROM test_table"
        params {
          fields {
            key: "param"
            value { string_value: "value" }
          }
        }
        param_types {
          key: "param"
          value { code: STRING }
        }
      )");
  request.set_session(
      GetSessionUri(GetSessionType() == SessionType::kMultiplexedSession));

  std::vector<spanner_api::PartialResultSet> response;
  GOOGLESQL_EXPECT_OK(ExecuteStreamingSql(request, &response));
  if (GetSessionType() == SessionType::kMultiplexedSession) {
    ASSERT_FALSE(response.back().has_precommit_token());
  }
  EXPECT_THAT(response, ElementsAre(Partially(EqualsProto(
                            R"pb(
                              metadata {
                                row_type {
                                  fields {
                                    name: "param"
                                    type { code: STRING }
                                  }
                                }
                                undeclared_parameters {
                                  fields {
                                    name: "param"
                                    type { code: STRING }
                                  }
                                }
                              }
                              values { string_value: "value" }
                              values { string_value: "value" }
                              values { string_value: "value" }
                            )pb"))));
}

TEST_P(QueryApiTest, ExecuteStreamingSqlWithProtoParameters) {
  GOOGLESQL_ASSERT_OK(AddProtoTables());
  GOOGLESQL_ASSERT_OK(PopulateProtoTable());

  spanner_api::ExecuteSqlRequest request = PARSE_TEXT_PROTO(
      R"pb(
        transaction { single_use { read_only { strong: true } } }
        sql: "SELECT @param.int_field AS intval FROM test_table"
        params {
          fields {
            key: "param"
            value { string_value: "CLoC" }
          }
        }
        param_types {
          key: "param"
          value { code: PROTO proto_type_fqn: "customer.app.User" }
        }
      )pb");
  request.set_session(
      GetSessionUri(GetSessionType() == SessionType::kMultiplexedSession));

  std::vector<spanner_api::PartialResultSet> response;
  GOOGLESQL_EXPECT_OK(ExecuteStreamingSql(request, &response));
  if (GetSessionType() == SessionType::kMultiplexedSession) {
    ASSERT_FALSE(response.back().has_precommit_token());
  }
  // The response ends on a row boundary, where the stream can resume.
  ASSERT_EQ(response.size(), 1);
  EXPECT_FALSE(response.front().resume_token().empty());
  response.front().clear_resume_token();
  EXPECT_THAT(
      response,
      ElementsAre(EqualsProto(
          R"pb(
            metadata {
              row_type {
                fields {
                  name: "intval"
                  type { code: INT64 }
                }
              }
              undeclared_parameters {
                fields {
                  name: "param"
                  type { code: PROTO proto_type_fqn: "customer.app.User" }
                }
              }
            }
            values { string_value: "314" }
            values { string_value: "314" }
            values { string_value: "314" }
          )pb")));
}

TEST_P(QueryApiTest, ExecuteStreamingSqlWithDmlAndParameters) {
  spanner_api::BeginTransactionRequest begin_request = PARSE_TEXT_PROTO(R"pb(
    options { read_write {} }
  )pb");
  begin_request.set_session(
      GetSessionUri(GetSessionType() == SessionType::kMultiplexedSession));

  spanner_api::Transaction transaction_response;
  GOOGLESQL_EXPECT_OK(BeginTransaction(begin_request, &transaction_response));

  spanner_api::ExecuteSqlRequest request = PARSE_TEXT_PROTO(
      R"""(
        sql: "INSERT INTO test_table (int64_col, string_col) "
             "VALUES (@p1, @p2)"
      )""");
  request.set_session(
      GetSessionUri(GetSessionType() == SessionType::kMultiplexedSession));
  request.set_query_mode(spanner_api::ExecuteSqlRequest::PLAN);
  request.mutable_transaction()->set_id(transaction_response.id());

  std::vector<spanner_api::PartialResultSet> response;
  GOOGLESQL_EXPECT_OK(ExecuteStreamingSql(request, &response));
  if (GetSessionType() == SessionType::kMultiplexedSession) {
    ASSERT_TRUE(response.back().has_precommit_token());
  }
  EXPECT_THAT(response, ElementsAre(Partially(EqualsProto(
                            R"pb(
                              metadata {
                                row_type {}
                                undeclared_parameters {
                                  fields {
                                    name: "p1"
                                    type { code: INT64 }
                                  }
                                  fields {
                                    name: "p2"
                                    type { code: STRING }
                                  }
                                }
                              }
                              stats { row_count_exact: 0 }
                            )pb"))));
}

TEST_P(QueryApiTest, AcceptsPlanMode) {
  spanner_api::ExecuteSqlRequest request = PARSE_TEXT_PROTO(
      R"pb(
        transaction { single_use { read_only { strong: true } } }
        query_mode: PLAN
        sql: "SELECT * FROM test_table"
      )pb");
  request.set_session(
      GetSessionUri(GetSessionType() == SessionType::kMultiplexedSession));

  // PLAN mode accepted in non-streaming case.
  {
    spanner_api::ResultSet response;
    EXPECT_THAT(ExecuteSql(request, &response),
                StatusIs(absl::StatusCode::kOk));
    if (GetSessionType() == SessionType::kMultiplexedSession) {
      ASSERT_FALSE(response.has_precommit_token());
    }
  }

  // PLAN mode accepted in streaming case.
  {
    std::vector<spanner_api::PartialResultSet> response;
    EXPECT_THAT(ExecuteStreamingSql(request, &response),
                StatusIs(absl::StatusCode::kOk));
    if (GetSessionType() == SessionType::kMultiplexedSession) {
      ASSERT_FALSE(response.back().has_precommit_token());
    }
  }
}

TEST_P(QueryApiTest, ExecuteStreamingSqlReturnsStatsWithTheLastResponse) {
  // The result is larger than a streaming chunk, so it takes two responses.
  spanner_api::ExecuteSqlRequest request = PARSE_TEXT_PROTO(
      R"pb(
        transaction { single_use { read_only { strong: true } } }
        query_mode: PROFILE
        sql: "SELECT REPEAT('x', 700000) UNION ALL SELECT REPEAT('y', 700000)"
      )pb");
  request.set_session(
      GetSessionUri(GetSessionType() == SessionType::kMultiplexedSession));

  std::vector<spanner_api::PartialResultSet> response;
  GOOGLESQL_ASSERT_OK(ExecuteStreamingSql(request, &response));
  ASSERT_GE(response.size(), 2);
  for (int i = 0; i + 1 < response.size(); ++i) {
    EXPECT_FALSE(response[i].has_stats());
  }
  const spanner_api::ResultSetStats& stats = response.back().stats();
  ASSERT_GT(stats.query_plan().plan_nodes_size(), 0);
  EXPECT_EQ(stats.query_plan().plan_nodes(0).display_name(),
            "Serialize Result");
  EXPECT_TRUE(stats.query_plan().plan_nodes(0).has_execution_stats());
  EXPECT_EQ(stats.query_stats().fields().at("rows_returned").string_value(),
            "2");
  EXPECT_THAT(stats.query_stats().fields().at("elapsed_time").string_value(),
              testing::MatchesRegex("[0-9]+\\.[0-9]{2} m?secs"));
}

TEST_P(QueryApiTest, ReturnsWhatTheQueryModeAsksFor) {
  spanner_api::ExecuteSqlRequest request = PARSE_TEXT_PROTO(
      R"pb(
        transaction { single_use { read_only { strong: true } } }
        sql: "SELECT * FROM test_table"
      )pb");
  request.set_session(
      GetSessionUri(GetSessionType() == SessionType::kMultiplexedSession));

  spanner_api::ResultSet response;
  request.set_query_mode(spanner_api::ExecuteSqlRequest::NORMAL);
  GOOGLESQL_ASSERT_OK(ExecuteSql(request, &response));
  EXPECT_FALSE(response.stats().has_query_plan());
  EXPECT_FALSE(response.stats().has_query_stats());

  response.Clear();
  request.set_query_mode(spanner_api::ExecuteSqlRequest::PLAN);
  GOOGLESQL_ASSERT_OK(ExecuteSql(request, &response));
  EXPECT_EQ(response.rows_size(), 0);
  EXPECT_TRUE(response.stats().has_query_plan());
  EXPECT_FALSE(response.stats().has_query_stats());
  EXPECT_FALSE(
      response.stats().query_plan().plan_nodes(0).has_execution_stats());

  response.Clear();
  request.set_query_mode(spanner_api::ExecuteSqlRequest::WITH_STATS);
  GOOGLESQL_ASSERT_OK(ExecuteSql(request, &response));
  EXPECT_FALSE(response.stats().has_query_plan());
  EXPECT_TRUE(response.stats().query_stats().fields().contains("cpu_time"));

  response.Clear();
  request.set_query_mode(spanner_api::ExecuteSqlRequest::WITH_PLAN_AND_STATS);
  GOOGLESQL_ASSERT_OK(ExecuteSql(request, &response));
  EXPECT_TRUE(response.stats().has_query_plan());
  EXPECT_FALSE(
      response.stats().query_plan().plan_nodes(0).has_execution_stats());
  EXPECT_TRUE(response.stats().query_stats().fields().contains("cpu_time"));
}

TEST_P(QueryApiTest, DirectedReadsWithROTxnSucceeds) {
  spanner_api::ExecuteSqlRequest request = PARSE_TEXT_PROTO(
      R"pb(
        transaction { single_use { read_only { strong: true } } }
        sql: "SELECT int64_col, string_col FROM test_table "
             "ORDER BY int64_col ASC, string_col DESC"
        directed_read_options {
          include_replicas { replica_selections { type: READ_ONLY } }
        }
      )pb");
  request.set_session(
      GetSessionUri(GetSessionType() == SessionType::kMultiplexedSession));

  // Directed Reads accepted in non-streaming case.
  {
    spanner_api::ResultSet unused_response;
    EXPECT_THAT(ExecuteSql(request, &unused_response),
                StatusIs(absl::StatusCode::kOk));
    if (GetSessionType() == SessionType::kMultiplexedSession) {
      ASSERT_FALSE(unused_response.has_precommit_token());
    }
  }

  // Directed Reads accepted in streaming case.
  {
    std::vector<spanner_api::PartialResultSet> unused_response;
    EXPECT_THAT(ExecuteStreamingSql(request, &unused_response),
                StatusIs(absl::StatusCode::kOk));
    if (GetSessionType() == SessionType::kMultiplexedSession) {
      ASSERT_FALSE(unused_response.back().has_precommit_token());
    }
  }
}

TEST_P(QueryApiTest, DirectedReadsWithRWTxnFails) {
  spanner_api::ExecuteSqlRequest request = PARSE_TEXT_PROTO(
      R"pb(
        transaction { begin { read_write {} } }
        sql: "SELECT int64_col, string_col FROM test_table "
             "ORDER BY int64_col ASC, string_col DESC"
        directed_read_options {
          include_replicas { replica_selections { type: READ_ONLY } }
        }
      )pb");
  request.set_session(
      GetSessionUri(GetSessionType() == SessionType::kMultiplexedSession));

  // Directed Reads rejected in non-streaming case.
  {
    spanner_api::ResultSet unused_response;
    EXPECT_THAT(ExecuteSql(request, &unused_response),
                StatusIs(absl::StatusCode::kFailedPrecondition));
  }

  // Directed Reads rejected in streaming case.
  {
    std::vector<spanner_api::PartialResultSet> unused_response;
    EXPECT_THAT(ExecuteStreamingSql(request, &unused_response),
                StatusIs(absl::StatusCode::kFailedPrecondition));
  }
}

// Geo-partitioning (placement) limits on statements in read-write
// transactions.
class PlacementQueryApiTest : public QueryApiTest {
 protected:
  void SetUp() override {
    QueryApiTest::SetUp();
    for (absl::string_view partition : {"europe-partition", "asia-partition"}) {
      GOOGLESQL_ASSERT_OK(CreateInstancePartition(partition));
    }
    GOOGLESQL_ASSERT_OK(UpdateDdl({
        "CREATE PLACEMENT europe OPTIONS "
        "(instance_partition = 'europe-partition')",
        "CREATE PLACEMENT asia OPTIONS (instance_partition = 'asia-partition')",
        "CREATE TABLE Singers (SingerId INT64 NOT NULL, Name STRING(MAX), "
        "Location STRING(MAX) NOT NULL PLACEMENT KEY) PRIMARY KEY (SingerId)",
    }));
    spanner_api::CommitRequest commit_request = PARSE_TEXT_PROTO(R"pb(
      single_use_transaction { read_write {} }
      mutations {
        insert {
          table: "Singers"
          columns: [ "SingerId", "Name", "Location" ]
          values {
            values { string_value: "1" }
            values { string_value: "Marc" }
            values { string_value: "europe" }
          }
          values {
            values { string_value: "2" }
            values { string_value: "Ana" }
            values { string_value: "europe" }
          }
        }
      }
    )pb");
    commit_request.set_session(test_session_uri_);
    spanner_api::CommitResponse commit_response;
    GOOGLESQL_ASSERT_OK(Commit(commit_request, &commit_response));
  }

  void TearDown() override {
    config::set_enforce_placement_dml_restrictions(true);
    QueryApiTest::TearDown();
  }

  absl::Status CreateInstancePartition(absl::string_view partition_id) {
    instance_api::CreateInstancePartitionRequest request;
    request.set_parent(test_instance_uri_);
    request.set_instance_partition_id(std::string(partition_id));
    request.mutable_instance_partition()->set_config(absl::StrCat(
        "projects/", test_project_name_, "/instanceConfigs/emulator-config"));
    request.mutable_instance_partition()->set_node_count(1);
    grpc::ClientContext context;
    operations_api::Operation operation;
    GOOGLESQL_RETURN_IF_ERROR(
        test_env()->instance_admin_client()->CreateInstancePartition(
            &context, request, &operation));
    return WaitForOperation(operation.name(), &operation);
  }

  absl::Status UpdateDdl(const std::vector<std::string>& statements) {
    database_api::UpdateDatabaseDdlRequest request;
    request.set_database(test_database_uri_);
    for (const std::string& statement : statements) {
      request.add_statements(statement);
    }
    grpc::ClientContext context;
    operations_api::Operation operation;
    GOOGLESQL_RETURN_IF_ERROR(test_env()->database_admin_client()->UpdateDatabaseDdl(
        &context, request, &operation));
    GOOGLESQL_RETURN_IF_ERROR(WaitForOperation(operation.name(), &operation));
    return absl::Status(static_cast<absl::StatusCode>(operation.error().code()),
                        operation.error().message());
  }

  absl::StatusOr<std::string> Begin(bool partitioned_dml = false) {
    spanner_api::BeginTransactionRequest request;
    request.set_session(test_session_uri_);
    if (partitioned_dml) {
      request.mutable_options()->mutable_partitioned_dml();
    } else {
      request.mutable_options()->mutable_read_write();
    }
    spanner_api::Transaction transaction;
    GOOGLESQL_RETURN_IF_ERROR(BeginTransaction(request, &transaction));
    return transaction.id();
  }

  absl::StatusOr<spanner_api::ResultSet> Execute(const std::string& txn_id,
                                                 const std::string& sql,
                                                 int64_t seqno) {
    spanner_api::ExecuteSqlRequest request;
    request.set_session(test_session_uri_);
    request.mutable_transaction()->set_id(txn_id);
    request.set_sql(sql);
    request.set_seqno(seqno);
    spanner_api::ResultSet response;
    GOOGLESQL_RETURN_IF_ERROR(ExecuteSql(request, &response));
    return response;
  }

  absl::StatusOr<spanner_api::ExecuteBatchDmlResponse> ExecuteBatch(
      const std::string& txn_id, const std::vector<std::string>& statements,
      int64_t seqno) {
    spanner_api::ExecuteBatchDmlRequest request;
    request.set_session(test_session_uri_);
    request.mutable_transaction()->set_id(txn_id);
    for (const std::string& sql : statements) {
      request.add_statements()->set_sql(sql);
    }
    request.set_seqno(seqno);
    spanner_api::ExecuteBatchDmlResponse response;
    GOOGLESQL_RETURN_IF_ERROR(ExecuteBatchDml(request, &response));
    return response;
  }

  absl::Status CommitTransaction(const std::string& txn_id) {
    spanner_api::CommitRequest request;
    request.set_session(test_session_uri_);
    request.set_transaction_id(txn_id);
    spanner_api::CommitResponse response;
    return Commit(request, &response);
  }

  absl::StatusOr<spanner_api::ResultSet> QueryReadOnly(const std::string& sql) {
    spanner_api::ExecuteSqlRequest request = PARSE_TEXT_PROTO(R"pb(
      transaction { single_use { read_only { strong: true } } }
    )pb");
    request.set_session(test_session_uri_);
    request.set_sql(sql);
    spanner_api::ResultSet response;
    GOOGLESQL_RETURN_IF_ERROR(ExecuteSql(request, &response));
    return response;
  }
};

INSTANTIATE_TEST_SUITE_P(RegularSession, PlacementQueryApiTest,
                         testing::Values(SessionType::kRegularSession));

TEST_P(PlacementQueryApiTest, PlacementInsertMustBeOnlyStatementInTransaction) {
  GOOGLESQL_ASSERT_OK_AND_ASSIGN(std::string txn, Begin());
  GOOGLESQL_EXPECT_OK(Execute(txn, "SELECT Name FROM Singers WHERE SingerId = 1", 1));
  EXPECT_THAT(Execute(txn,
                      "INSERT INTO Singers (SingerId, Name, Location) "
                      "VALUES (3, 'Lea', 'asia')",
                      2),
              StatusIs(absl::StatusCode::kFailedPrecondition,
                       HasSubstr("must be the only statement")));
}

TEST_P(PlacementQueryApiTest, NoStatementMayFollowPlacementDelete) {
  GOOGLESQL_ASSERT_OK_AND_ASSIGN(std::string txn, Begin());
  GOOGLESQL_EXPECT_OK(Execute(txn, "DELETE FROM Singers WHERE SingerId = 1", 1));
  // Replaying the same request returns its saved outcome.
  GOOGLESQL_EXPECT_OK(Execute(txn, "DELETE FROM Singers WHERE SingerId = 1", 1));
  EXPECT_THAT(Execute(txn, "SELECT Name FROM Singers WHERE SingerId = 2", 2),
              StatusIs(absl::StatusCode::kFailedPrecondition,
                       HasSubstr("Singers")));
  EXPECT_THAT(
      Execute(txn, "UPDATE test_table SET string_col = 'x' WHERE int64_col = 1",
              3),
      StatusIs(absl::StatusCode::kFailedPrecondition,
               HasSubstr("must be the only statement")));
  GOOGLESQL_EXPECT_OK(CommitTransaction(txn));

  GOOGLESQL_ASSERT_OK_AND_ASSIGN(spanner_api::ResultSet result,
                       QueryReadOnly("SELECT SingerId FROM Singers"));
  EXPECT_THAT(result, Partially(EqualsProto(R"pb(
                rows { values { string_value: "2" } }
              )pb")));
}

TEST_P(PlacementQueryApiTest, PlacementInsertAloneCommits) {
  GOOGLESQL_ASSERT_OK_AND_ASSIGN(std::string txn, Begin());
  GOOGLESQL_EXPECT_OK(Execute(txn,
                    "INSERT INTO Singers (SingerId, Name, Location) "
                    "VALUES (3, 'Lea', 'asia')",
                    1));
  GOOGLESQL_EXPECT_OK(CommitTransaction(txn));

  // Read-only transactions may filter on any column.
  GOOGLESQL_ASSERT_OK_AND_ASSIGN(
      spanner_api::ResultSet result,
      QueryReadOnly("SELECT SingerId FROM Singers WHERE Location = 'asia'"));
  EXPECT_THAT(result, Partially(EqualsProto(R"pb(
                rows { values { string_value: "3" } }
              )pb")));
}

TEST_P(PlacementQueryApiTest, WhereClauseLimitedToPrimaryKeyInReadWriteTxn) {
  GOOGLESQL_ASSERT_OK_AND_ASSIGN(std::string txn, Begin());
  EXPECT_THAT(
      Execute(txn, "UPDATE Singers SET Name = 'x' WHERE Location = 'europe'", 1),
      StatusIs(absl::StatusCode::kInvalidArgument,
               HasSubstr("primary key columns of placement table Singers")));

  GOOGLESQL_ASSERT_OK_AND_ASSIGN(txn, Begin());
  EXPECT_THAT(Execute(txn, "SELECT SingerId FROM Singers WHERE Name = 'Marc'", 1),
              StatusIs(absl::StatusCode::kInvalidArgument,
                       HasSubstr("references Name")));

  // Updating a row by primary key, including moving it to another placement,
  // is allowed.
  GOOGLESQL_ASSERT_OK_AND_ASSIGN(txn, Begin());
  GOOGLESQL_EXPECT_OK(Execute(
      txn, "UPDATE Singers SET Location = 'asia' WHERE SingerId = 1", 1));
  GOOGLESQL_EXPECT_OK(
      Execute(txn, "UPDATE Singers SET Name = 'y' WHERE SingerId = 2", 2));
  GOOGLESQL_EXPECT_OK(CommitTransaction(txn));
}

TEST_P(PlacementQueryApiTest, PartitionedDmlMayFilterOnPlacementKey) {
  GOOGLESQL_ASSERT_OK_AND_ASSIGN(std::string txn, Begin(/*partitioned_dml=*/true));
  GOOGLESQL_ASSERT_OK_AND_ASSIGN(
      spanner_api::ResultSet result,
      Execute(txn,
              "UPDATE Singers SET Location = 'asia' WHERE Location = 'europe'",
              1));
  EXPECT_EQ(result.stats().row_count_lower_bound(), 2);
}

TEST_P(PlacementQueryApiTest, BatchWithPlacementInsertFailsBeforeRunning) {
  GOOGLESQL_ASSERT_OK_AND_ASSIGN(std::string txn, Begin());
  GOOGLESQL_ASSERT_OK_AND_ASSIGN(
      spanner_api::ExecuteBatchDmlResponse response,
      ExecuteBatch(txn,
                   {"UPDATE test_table SET string_col = 'x' "
                    "WHERE int64_col = 1",
                    "INSERT INTO Singers (SingerId, Name, Location) "
                    "VALUES (3, 'Lea', 'asia')"},
                   1));
  EXPECT_EQ(response.result_sets_size(), 0);
  EXPECT_EQ(response.status().code(),
            static_cast<int>(absl::StatusCode::kFailedPrecondition));
  EXPECT_THAT(response.status().message(),
              HasSubstr("must be the only statement"));

  // A batch holding just the placement insert runs.
  GOOGLESQL_ASSERT_OK_AND_ASSIGN(txn, Begin());
  GOOGLESQL_ASSERT_OK_AND_ASSIGN(response,
                       ExecuteBatch(txn,
                                    {"INSERT INTO Singers (SingerId, Name, "
                                     "Location) VALUES (3, 'Lea', 'asia')"},
                                    1));
  EXPECT_EQ(response.result_sets_size(), 1);
  EXPECT_EQ(response.status().code(), 0);
  GOOGLESQL_EXPECT_OK(CommitTransaction(txn));
}

TEST_P(PlacementQueryApiTest, RestrictionsCanBeDisabled) {
  config::set_enforce_placement_dml_restrictions(false);
  GOOGLESQL_ASSERT_OK_AND_ASSIGN(std::string txn, Begin());
  GOOGLESQL_EXPECT_OK(
      Execute(txn, "SELECT SingerId FROM Singers WHERE Location = 'europe'", 1));
  GOOGLESQL_EXPECT_OK(Execute(txn,
                    "INSERT INTO Singers (SingerId, Name, Location) "
                    "VALUES (3, 'Lea', 'asia')",
                    2));
  GOOGLESQL_EXPECT_OK(Execute(txn, "DELETE FROM Singers WHERE SingerId = 1", 3));
  GOOGLESQL_EXPECT_OK(CommitTransaction(txn));
}

// Streams queries and reads whose results span several PartialResultSets, and
// resumes them the way client libraries do after a broken stream: by resending
// the request with the resume token of the last response received.
class ResumeTokenApiTest : public test::ServerTest {
 protected:
  // Enough rows of this size that results span several PartialResultSets, and
  // several batches of the scrambled order of a query without ORDER BY.
  static constexpr int kNumRows = 200;
  static constexpr int kValueSize = 16 * 1024;

  void SetUp() override {
    GOOGLESQL_ASSERT_OK(CreateTestInstance());
    GOOGLESQL_ASSERT_OK(CreateTestDatabase());
    GOOGLESQL_ASSERT_OK_AND_ASSIGN(session_,
                                   CreateTestSession(/*multiplexed=*/false));
    GOOGLESQL_ASSERT_OK(InsertRows(1, kNumRows));
  }

  absl::Status InsertRows(int64_t first, int64_t last) {
    spanner_api::CommitRequest request;
    request.set_session(session_);
    request.mutable_single_use_transaction()->mutable_read_write();
    auto* insert = request.add_mutations()->mutable_insert();
    insert->set_table("test_table");
    insert->add_columns("int64_col");
    insert->add_columns("string_col");
    for (int64_t key = first; key <= last; ++key) {
      auto* row = insert->add_values();
      row->add_values()->set_string_value(absl::StrCat(key));
      row->add_values()->set_string_value(
          std::string(kValueSize, 'a' + key % 26));
    }
    spanner_api::CommitResponse response;
    return Commit(request, &response);
  }

  // Resends `request` with the resume token of each response of `original`,
  // its complete stream, and expects the resumed stream to return the rows
  // that follow the response. Returns the number of resumed streams.
  template <typename Request, typename Execute>
  int ExpectResumesAfterEveryToken(
      Request request,
      const std::vector<spanner_api::PartialResultSet>& original,
      const Execute& execute) {
    auto all = backend::test::MergePartialResultSets(original, 2);
    EXPECT_TRUE(all.ok()) << all.status();
    int resumed_streams = 0;
    for (int i = 0; i < original.size(); ++i) {
      if (original[i].resume_token().empty()) {
        continue;
      }
      ++resumed_streams;
      auto before = backend::test::MergePartialResultSets(
          {original.begin(), original.begin() + i + 1}, 2);
      request.set_resume_token(original[i].resume_token());
      std::vector<spanner_api::PartialResultSet> resumed;
      GOOGLESQL_EXPECT_OK(execute(request, &resumed));
      auto after = backend::test::MergePartialResultSets(resumed, 2);
      if (!before.ok() || !after.ok()) {
        ADD_FAILURE() << before.status() << after.status();
        continue;
      }
      EXPECT_TRUE(resumed.front().has_metadata());
      EXPECT_EQ(before->rows_size() + after->rows_size(), all->rows_size());
      for (int j = 0; j < after->rows_size() &&
                      before->rows_size() + j < all->rows_size();
           ++j) {
        EXPECT_THAT(after->rows(j),
                    EqualsProto(all->rows(before->rows_size() + j)));
      }
    }
    return resumed_streams;
  }

  std::string session_;
};

TEST_F(ResumeTokenApiTest, ResumesSingleUseQueryAtItsReadTimestamp) {
  spanner_api::ExecuteSqlRequest request = PARSE_TEXT_PROTO(R"pb(
    transaction { single_use { read_only { strong: true } } }
    sql: "SELECT int64_col, string_col FROM test_table"
  )pb");
  request.set_session(session_);
  std::vector<spanner_api::PartialResultSet> original;
  GOOGLESQL_ASSERT_OK(ExecuteStreamingSql(request, &original));
  ASSERT_GT(original.size(), 2);

  // Rows committed later are not visible to a resumed stream.
  GOOGLESQL_ASSERT_OK(InsertRows(kNumRows + 1, kNumRows + 10));
  EXPECT_GT(ExpectResumesAfterEveryToken(
                request, original,
                [this](const auto& request, auto* response) {
                  return ExecuteStreamingSql(request, response);
                }),
            1);
}

TEST_F(ResumeTokenApiTest, ResumesQueryInTheSameTransaction) {
  spanner_api::ExecuteSqlRequest request = PARSE_TEXT_PROTO(R"pb(
    transaction { begin { read_write {} } }
    sql: "SELECT int64_col, string_col FROM test_table"
  )pb");
  request.set_session(session_);
  std::vector<spanner_api::PartialResultSet> original;
  GOOGLESQL_ASSERT_OK(ExecuteStreamingSql(request, &original));
  ASSERT_GT(original.size(), 2);
  const std::string transaction_id =
      original.front().metadata().transaction().id();

  // A resumed stream must not begin another transaction.
  request.set_resume_token(original.front().resume_token());
  std::vector<spanner_api::PartialResultSet> resumed;
  EXPECT_THAT(ExecuteStreamingSql(request, &resumed),
              StatusIs(absl::StatusCode::kInvalidArgument));

  request.mutable_transaction()->set_id(transaction_id);
  EXPECT_GT(ExpectResumesAfterEveryToken(
                request, original,
                [this](const auto& request, auto* response) {
                  return ExecuteStreamingSql(request, response);
                }),
            1);
}

TEST_F(ResumeTokenApiTest, ResumesStreamingReadWithLimit) {
  spanner_api::ReadRequest request = PARSE_TEXT_PROTO(R"pb(
    transaction { single_use { read_only { strong: true } } }
    table: "test_table"
    columns: "int64_col"
    columns: "string_col"
    key_set { all: true }
    limit: 150
  )pb");
  request.set_session(session_);
  std::vector<spanner_api::PartialResultSet> original;
  GOOGLESQL_ASSERT_OK(StreamingRead(request, &original));
  GOOGLESQL_ASSERT_OK_AND_ASSIGN(
      spanner_api::ResultSet all,
      backend::test::MergePartialResultSets(original, 2));
  EXPECT_EQ(all.rows_size(), 150);
  EXPECT_GT(ExpectResumesAfterEveryToken(
                request, original,
                [this](const auto& request, auto* response) {
                  return StreamingRead(request, response);
                }),
            1);
}

TEST_F(ResumeTokenApiTest, RejectsTokensThatDoNotResumeTheRequest) {
  spanner_api::ExecuteSqlRequest request = PARSE_TEXT_PROTO(R"pb(
    transaction { single_use { read_only { strong: true } } }
    sql: "SELECT int64_col, string_col FROM test_table"
  )pb");
  request.set_session(session_);
  std::vector<spanner_api::PartialResultSet> original;
  GOOGLESQL_ASSERT_OK(ExecuteStreamingSql(request, &original));
  const std::string token = original.front().resume_token();
  ASSERT_FALSE(token.empty());
  std::vector<spanner_api::PartialResultSet> resumed;

  request.set_resume_token("not a token");
  EXPECT_THAT(ExecuteStreamingSql(request, &resumed),
              StatusIs(absl::StatusCode::kInvalidArgument));

  // The token of another request.
  spanner_api::ExecuteSqlRequest other_request = request;
  other_request.set_sql(
      "SELECT int64_col, string_col FROM test_table WHERE int64_col > 1");
  other_request.set_resume_token(token);
  EXPECT_THAT(ExecuteStreamingSql(other_request, &resumed),
              StatusIs(absl::StatusCode::kInvalidArgument));

  // A token whose rows differ from those that the request returns.
  ResumeToken changed;
  ASSERT_TRUE(changed.ParseFromString(token));
  changed.mutable_rows()->set_rows_fingerprint(
      changed.rows().rows_fingerprint() + 1);
  request.set_resume_token(changed.SerializeAsString());
  EXPECT_THAT(ExecuteStreamingSql(request, &resumed),
              StatusIs(absl::StatusCode::kFailedPrecondition));
}

}  // namespace

}  // namespace frontend
}  // namespace emulator
}  // namespace spanner
}  // namespace google

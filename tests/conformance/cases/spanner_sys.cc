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
#include <vector>

#include "google/spanner/admin/database/v1/common.pb.h"
#include "google/spanner/admin/database/v1/spanner_database_admin.pb.h"
#include "google/spanner/v1/spanner.pb.h"
#include "gmock/gmock.h"
#include "gtest/gtest.h"
#include "googlesql/base/testing/status_matchers.h"
#include "tests/common/proto_matchers.h"
#include "absl/log/log.h"
#include "absl/status/status.h"
#include "absl/strings/ascii.h"
#include "absl/strings/match.h"
#include "absl/time/time.h"
#include "absl/types/span.h"
#include "common/config.h"
#include "tests/conformance/common/database_test_base.h"
#include "googlesql/base/status_macros.h"

namespace google {
namespace spanner {
namespace emulator {
namespace test {

namespace {

using ::googlesql_base::testing::StatusIs;
using ::testing::Contains;
using ::testing::ElementsAre;
using ::testing::IsEmpty;
using ::testing::Not;
using ::testing::SizeIs;

class SpannerSysTest
    : public DatabaseTest,
      public testing::WithParamInterface<database_api::DatabaseDialect> {
 public:
  void SetUp() override {
    dialect_ = GetParam();
    DatabaseTest::SetUp();
  }
  absl::Status SetUpDatabase() override { return absl::OkStatus(); }
};

TEST_P(SpannerSysTest, SupportedOptimizerVersionsTableIsValid) {

  absl::StatusOr<std::vector<ValueRow>> results;
  if (dialect_ == database_api::DatabaseDialect::POSTGRESQL) {
    results = Query(R"(
      SELECT is_default, to_char(release_date, 'YYYY-MM-dd'), version
      FROM spanner_sys.SUPPORTED_OPTIMIZER_VERSIONS
    )");
  } else {
    results = Query(R"(
      SELECT is_default, FORMAT_DATE('%F', release_date), version
      FROM spanner_sys.SUPPORTED_OPTIMIZER_VERSIONS
    )");
  }

  EXPECT_THAT(results, IsOkAndHoldsRow({true, "2023-09-19", 42}));
}

INSTANTIATE_TEST_SUITE_P(
    PerDialectSpannerSysTest, SpannerSysTest,
    testing::Values(database_api::DatabaseDialect::GOOGLE_STANDARD_SQL,
                    database_api::DatabaseDialect::POSTGRESQL),
    [](const testing::TestParamInfo<SpannerSysTest::ParamType>& info) {
      return database_api::DatabaseDialect_Name(info.param);
    });

// Tests the statistics tables, which report the emulator's own measurements.
// Production Cloud Spanner reports an interval only after it ends, so these
// tests make the emulator report the current interval too.
class SpannerSysStatisticsTest : public SpannerSysTest {
 public:
  void SetUp() override {
    if (in_prod_env()) {
      GTEST_SKIP() << "Statistics appear in production only after a minute.";
    }
    config::set_spanner_sys_expose_open_interval(true);
    SpannerSysTest::SetUp();
  }

  void TearDown() override {
    config::set_spanner_sys_expose_open_interval(false);
    SpannerSysTest::TearDown();
  }

  absl::Status SetUpDatabase() override {
    if (dialect_ == database_api::DatabaseDialect::POSTGRESQL) {
      GOOGLESQL_RETURN_IF_ERROR(SetSchema({R"(
        CREATE TABLE users (
          id bigint NOT NULL PRIMARY KEY,
          name varchar
        ))"}));
    } else {
      GOOGLESQL_RETURN_IF_ERROR(SetSchema({R"(
        CREATE TABLE users (
          id INT64 NOT NULL,
          name STRING(MAX),
        ) PRIMARY KEY (id))"}));
    }
    return CreateSession(&session_);
  }

 protected:
  absl::Status CreateSession(std::string* session) {
    grpc::ClientContext context;
    spanner_api::CreateSessionRequest request;
    request.set_database(database()->FullName());
    spanner_api::Session response;
    GOOGLESQL_RETURN_IF_ERROR(
        raw_client()->CreateSession(&context, request, &response));
    *session = response.name();
    return absl::OkStatus();
  }

  // Returns the INT64 values of the first column of `sql`'s rows.
  std::vector<int64_t> Int64Column(const std::string& sql) {
    std::vector<int64_t> values;
    absl::StatusOr<std::vector<ValueRow>> rows = Query(sql);
    EXPECT_TRUE(rows.ok()) << rows.status();
    if (rows.ok()) {
      for (const ValueRow& row : *rows) {
        values.push_back(*row.values()[0].get<std::int64_t>());
      }
    }
    return values;
  }

  int64_t Sum(const std::vector<int64_t>& values) {
    int64_t sum = 0;
    for (int64_t value : values) {
      sum += value;
    }
    return sum;
  }

  // Returns the upper-case names of the columns of `table`.
  std::vector<std::string> ColumnNames(const std::string& table) {
    spanner_api::ExecuteSqlRequest request = PARSE_TEXT_PROTO(R"pb(
      transaction { single_use { read_only { strong: true } } }
    )pb");
    request.set_session(session_);
    request.set_sql("SELECT * FROM spanner_sys." + table);
    spanner_api::ResultSet response;
    grpc::ClientContext context;
    EXPECT_TRUE(raw_client()->ExecuteSql(&context, request, &response).ok());
    std::vector<std::string> names;
    for (const auto& field : response.metadata().row_type().fields()) {
      names.push_back(absl::AsciiStrToUpper(field.name()));
    }
    return names;
  }

  absl::Status AddSplitPoints(
      const database_api::AddSplitPointsRequest& request) {
    database_api::AddSplitPointsResponse response;
    grpc::ClientContext context;
    grpc::Status status =
        raw_database_client()->AddSplitPoints(&context, request, &response);
    return absl::Status(static_cast<absl::StatusCode>(status.error_code()),
                        status.error_message());
  }

  std::string session_;
};

INSTANTIATE_TEST_SUITE_P(
    PerDialectSpannerSysStatisticsTest, SpannerSysStatisticsTest,
    testing::Values(database_api::DatabaseDialect::GOOGLE_STANDARD_SQL,
                    database_api::DatabaseDialect::POSTGRESQL),
    [](const testing::TestParamInfo<SpannerSysStatisticsTest::ParamType>&
           info) { return database_api::DatabaseDialect_Name(info.param); });

TEST_P(SpannerSysStatisticsTest, TablesAreEmptyBeforeAnyActivity) {
  // A statement is recorded once it completes, so the first query sees no
  // queries.
  EXPECT_THAT(
      Query("SELECT interval_end FROM spanner_sys.query_stats_total_minute"),
      IsOkAndHoldsRows({}));
  for (const std::string table :
       {"read_stats_top_minute", "txn_stats_total_10minute",
        "lock_stats_top_hour", "split_stats_top_minute"}) {
    EXPECT_THAT(Int64Column("SELECT COUNT(*) FROM spanner_sys." + table),
                ElementsAre(0))
        << table;
  }
}

TEST_P(SpannerSysStatisticsTest, ColumnsAreInProductionOrder) {
  std::vector<std::string> columns = ColumnNames("query_stats_top_minute");
  ASSERT_THAT(columns,
              SizeIs(dialect_ == database_api::DatabaseDialect::POSTGRESQL
                         ? 30
                         : 31));
  EXPECT_THAT(std::vector<std::string>(columns.begin(), columns.begin() + 6),
              ElementsAre("INTERVAL_END", "TEXT", "TEXT_TRUNCATED",
                          "TEXT_FINGERPRINT", "EXECUTION_COUNT",
                          "AVG_LATENCY_SECONDS"));
  EXPECT_THAT(std::vector<std::string>(columns.end() - 3, columns.end()),
              ElementsAre("LATENCY_DISTRIBUTION_JSON_STRING",
                          "QUERY_OPTIMIZER_VERSIONS",
                          "STATISTICS_PACKAGE_NAMES"));
  // PostgreSQL databases have the JSON string instead of the struct column.
  if (dialect_ == database_api::DatabaseDialect::POSTGRESQL) {
    EXPECT_THAT(columns, Not(Contains("LATENCY_DISTRIBUTION")));
  } else {
    EXPECT_THAT(columns, Contains("LATENCY_DISTRIBUTION"));
  }

  EXPECT_THAT(ColumnNames("supported_optimizer_versions"),
              ElementsAre("VERSION", "RELEASE_DATE", "IS_DEFAULT"));
  EXPECT_THAT(ColumnNames("lock_stats_total_minute"),
              ElementsAre("INTERVAL_END", "TOTAL_LOCK_WAIT_SECONDS"));
}

TEST_P(SpannerSysStatisticsTest, RecordsQueries) {
  GOOGLESQL_ASSERT_OK(Insert("users", {"id", "name"}, {1, "a"}));
  GOOGLESQL_ASSERT_OK(Insert("users", {"id", "name"}, {2, "b"}));
  const std::string query = "SELECT name FROM users WHERE id = 1";
  for (int i = 0; i < 3; ++i) {
    EXPECT_THAT(Query(query), IsOkAndHoldsRows({{"a"}}));
  }
  EXPECT_THAT(Query("SELECT id / 0 FROM users"),
              StatusIs(absl::StatusCode::kOutOfRange));

  const std::string stats_filter = " WHERE text = '" + query + "'";
  EXPECT_EQ(Sum(Int64Column("SELECT execution_count FROM "
                            "spanner_sys.query_stats_top_minute" +
                            stats_filter)),
            3);
  // The statement has one fingerprint, in every interval table.
  std::vector<int64_t> fingerprints = Int64Column(
      "SELECT text_fingerprint FROM spanner_sys.query_stats_top_hour" +
      stats_filter);
  ASSERT_THAT(fingerprints, SizeIs(1));
  EXPECT_THAT(Int64Column("SELECT text_fingerprint FROM "
                          "spanner_sys.query_stats_top_10minute" +
                          stats_filter),
              Contains(fingerprints[0]));

  absl::StatusOr<std::vector<ValueRow>> rows = Query(
      "SELECT avg_rows, avg_rows_scanned, avg_latency_seconds, "
      "avg_cpu_seconds, query_type, request_tag, all_failed_execution_count "
      "FROM spanner_sys.query_stats_top_hour" +
      stats_filter);
  GOOGLESQL_ASSERT_OK(rows);
  ASSERT_THAT(*rows, SizeIs(1));
  absl::Span<const Value> row = (*rows)[0].values();
  EXPECT_EQ(*row[0].get<double>(), 1);
  // The emulator scans the whole table for a key lookup.
  EXPECT_EQ(*row[1].get<double>(), 2);
  EXPECT_GT(*row[2].get<double>(), 0);
  EXPECT_GE(*row[3].get<double>(), 0);
  EXPECT_EQ(*row[4].get<std::string>(), "QUERY");
  EXPECT_EQ(*row[5].get<std::string>(), "");
  EXPECT_EQ(*row[6].get<std::int64_t>(), 0);

  // Failed statements count as failed executions.
  EXPECT_THAT(Int64Column("SELECT all_failed_execution_count FROM "
                          "spanner_sys.query_stats_top_hour WHERE text = "
                          "'SELECT id / 0 FROM users'"),
              ElementsAre(1));
  EXPECT_GE(Sum(Int64Column("SELECT execution_count FROM "
                            "spanner_sys.query_stats_total_hour")),
            3);
}

TEST_P(SpannerSysStatisticsTest, GroupsTaggedQueriesByRequestTag) {
  for (const std::string query : {"SELECT 1", "SELECT 2"}) {
    auto rows = client().ExecuteQuery(
        SqlStatement(query),
        cloud::spanner::QueryOptions().set_request_tag("app=report"));
    for (const auto& row : rows) {
      GOOGLESQL_ASSERT_OK(ToUtilStatusOr(row));
    }
  }
  EXPECT_THAT(Query("SELECT text, execution_count FROM "
                    "spanner_sys.query_stats_top_hour "
                    "WHERE request_tag = 'app=report'"),
              IsOkAndHoldsRows({{"SELECT 1", 2}}));
}

TEST_P(SpannerSysStatisticsTest, RecordsReadsAndOperations) {
  GOOGLESQL_ASSERT_OK(Insert("users", {"id", "name"}, {1, "a"}));
  GOOGLESQL_ASSERT_OK(Insert("users", {"id", "name"}, {2, "b"}));
  for (int i = 0; i < 2; ++i) {
    EXPECT_THAT(ReadAll("users", {"name", "id"}),
                IsOkAndHoldsRows({{"a", 1}, {"b", 2}}));
  }

  EXPECT_THAT(Query("SELECT read_columns, execution_count, avg_rows, "
                    "read_type FROM spanner_sys.read_stats_top_hour"),
              IsOkAndHoldsRows({{std::vector<std::string>{"users.id",
                                                          "users.name"},
                                 2, 2.0, "READ"}}));

  // Two inserts, two reads and the queries above.
  EXPECT_THAT(Query("SELECT write_count, delete_count FROM "
                    "spanner_sys.table_operations_stats_hour "
                    "WHERE table_name = 'users'"),
              IsOkAndHoldsRows({{2, 0}}));
  EXPECT_THAT(
      Int64Column("SELECT read_count FROM "
                  "spanner_sys.column_operations_stats_hour "
                  "WHERE table_name = 'users' AND column_name = 'name'"),
      ElementsAre(2));
}

TEST_P(SpannerSysStatisticsTest, RecordsTransactionsAndLockConflicts) {
  GOOGLESQL_ASSERT_OK(Insert("users", {"id", "name"}, {1, "a"}));
  // Younger transactions wait for older ones' locks, briefly.
  const int abort_probability = config::abort_current_transaction_probability();
  const absl::Duration lock_wait_timeout = config::lock_wait_timeout();
  config::set_abort_current_transaction_probability(0);
  config::set_lock_wait_timeout_ms(300);

  // A reader locks a row...
  spanner_api::BeginTransactionRequest begin = PARSE_TEXT_PROTO(R"pb(
    options { read_write {} }
    request_options { transaction_tag: "reader" }
  )pb");
  begin.set_session(session_);
  spanner_api::Transaction reader;
  {
    grpc::ClientContext context;
    GOOGLESQL_ASSERT_OK(
        raw_client()->BeginTransaction(&context, begin, &reader));
  }
  spanner_api::ReadRequest read = PARSE_TEXT_PROTO(R"pb(
    table: "users"
    columns: "name"
    key_set { keys { values { string_value: "1" } } }
  )pb");
  read.set_session(session_);
  read.mutable_transaction()->set_id(reader.id());
  {
    spanner_api::ResultSet response;
    grpc::ClientContext context;
    GOOGLESQL_ASSERT_OK(raw_client()->Read(&context, read, &response));
  }

  // ...that a younger writer then updates. The writer waits for the reader's
  // lock until the wait times out.
  std::string writer_session;
  GOOGLESQL_ASSERT_OK(CreateSession(&writer_session));
  spanner_api::CommitRequest commit = PARSE_TEXT_PROTO(R"pb(
    single_use_transaction { read_write {} }
    mutations {
      update {
        table: "users"
        columns: "id"
        columns: "name"
        values { values { string_value: "1" } values { string_value: "b" } }
      }
    }
    request_options { transaction_tag: "writer" }
  )pb");
  commit.set_session(writer_session);
  {
    spanner_api::CommitResponse response;
    grpc::ClientContext context;
    EXPECT_THAT(raw_client()->Commit(&context, commit, &response),
                StatusIs(absl::StatusCode::kAborted));
  }
  // Once the reader commits, the writer's retry commits too.
  spanner_api::CommitRequest reader_commit;
  reader_commit.set_session(session_);
  reader_commit.set_transaction_id(reader.id());
  {
    spanner_api::CommitResponse response;
    grpc::ClientContext context;
    GOOGLESQL_ASSERT_OK(
        raw_client()->Commit(&context, reader_commit, &response));
  }
  {
    spanner_api::CommitResponse response;
    grpc::ClientContext context;
    GOOGLESQL_ASSERT_OK(raw_client()->Commit(&context, commit, &response));
  }
  config::set_abort_current_transaction_probability(abort_probability);
  config::set_lock_wait_timeout_ms(
      absl::ToInt64Milliseconds(lock_wait_timeout));

  EXPECT_THAT(
      Query("SELECT attempt_count, commit_attempt_count, commit_abort_count, "
            "commit_failed_precondition_count, write_constructive_columns "
            "FROM spanner_sys.txn_stats_top_hour "
            "WHERE transaction_tag = 'writer'"),
      IsOkAndHoldsRows({{2, 2, 1, 0,
                         std::vector<std::string>{"users.id", "users.name"}}}));
  EXPECT_THAT(
      Query("SELECT attempt_count, commit_abort_count, read_columns "
            "FROM spanner_sys.txn_stats_top_hour "
            "WHERE transaction_tag = 'reader'"),
      IsOkAndHoldsRows({{1, 0, std::vector<std::string>{"users.name"}}}));
  EXPECT_GE(Sum(Int64Column("SELECT attempt_count FROM "
                            "spanner_sys.txn_stats_total_hour")),
            3);

  absl::StatusOr<std::vector<ValueRow>> locks =
      Query("SELECT row_range_start_key, lock_wait_seconds, "
            "sample_lock_requests_json_string "
            "FROM spanner_sys.lock_stats_top_hour");
  GOOGLESQL_ASSERT_OK(locks);
  ASSERT_THAT(*locks, SizeIs(1));
  absl::Span<const Value> lock = (*locks)[0].values();
  EXPECT_EQ(*lock[0].get<Bytes>(), Bytes(std::string("users(1)")));
  // The writer waited for the reader's lock.
  EXPECT_GE(*lock[1].get<double>(), 0.3);
  const std::string samples = *lock[2].get<std::string>();
  EXPECT_TRUE(absl::StrContains(
      samples,
      R"({"COLUMN":"users.name","LOCK_MODE":"ReaderShared",)"
      R"("TRANSACTION_TAG":"reader"})"))
      << samples;
  EXPECT_TRUE(absl::StrContains(
      samples, R"("LOCK_MODE":"Exclusive","TRANSACTION_TAG":"writer"})"))
      << samples;
}

TEST_P(SpannerSysStatisticsTest, ListsActiveQueries) {
  // The query that reads the table is active.
  EXPECT_THAT(Query("SELECT text, transaction_type FROM "
                    "spanner_sys.oldest_active_queries"),
              IsOkAndHoldsRows({{"SELECT text, transaction_type FROM "
                                 "spanner_sys.oldest_active_queries",
                                 "NONE"}}));
  EXPECT_THAT(Int64Column("SELECT active_count FROM "
                          "spanner_sys.active_queries_summary"),
              ElementsAre(1));

  // Active queries can't be read in a read-write transaction.
  auto rows = client().ExecuteQuery(
      Transaction(Transaction::ReadWriteOptions()),
      SqlStatement("SELECT text FROM spanner_sys.oldest_active_queries"));
  for (const auto& row : rows) {
    EXPECT_THAT(ToUtilStatusOr(row),
                StatusIs(absl::StatusCode::kInvalidArgument));
  }
}

TEST_P(SpannerSysStatisticsTest, StateTablesHaveProductionColumns) {
  EXPECT_THAT(ColumnNames("active_partitioned_dmls"),
              ElementsAre("TEXT", "TEXT_FINGERPRINT", "SESSION_ID",
                          "NUM_PARTITIONS_TOTAL", "NUM_PARTITIONS_COMPLETE",
                          "NUM_TRIVIAL_PARTITIONS_COMPLETE", "PROGRESS",
                          "ROWS_PROCESSED", "START_TIMESTAMP",
                          "LAST_UPDATE_TIMESTAMP"));
  EXPECT_THAT(ColumnNames("table_sizes_stats_1hour"),
              ElementsAre("INTERVAL_END", "TABLE_NAME", "USED_BYTES",
                          "USED_SSD_BYTES", "USED_HDD_BYTES"));
  EXPECT_THAT(ColumnNames("row_deletion_policies"),
              ElementsAre("TABLE_NAME", "PROCESSED_WATERMARK",
                          "UNDELETABLE_ROWS", "MIN_UNDELETABLE_TIMESTAMP"));
  EXPECT_THAT(ColumnNames("user_split_points"),
              ElementsAre("TABLE_NAME", "INDEX_NAME", "INITIATOR", "SPLIT_KEY",
                          "EXPIRE_TIME"));
  // No partitioned DML is running.
  EXPECT_THAT(
      Int64Column("SELECT COUNT(*) FROM spanner_sys.active_partitioned_dmls"),
      ElementsAre(0));
}

TEST_P(SpannerSysStatisticsTest, ListsRowDeletionPolicies) {
  GOOGLESQL_ASSERT_OK(UpdateSchema(
      {dialect_ == database_api::DatabaseDialect::POSTGRESQL
           ? "CREATE TABLE events (id bigint NOT NULL PRIMARY KEY, "
             "ts timestamptz) TTL INTERVAL '1 day' ON ts"
           : "CREATE TABLE events (id INT64 NOT NULL, ts TIMESTAMP) "
             "PRIMARY KEY (id), "
             "ROW DELETION POLICY (OLDER_THAN(ts, INTERVAL 1 DAY))"}));
  // The policy lists the table, which the background sweep has not
  // processed yet.
  EXPECT_THAT(Query("SELECT table_name, undeletable_rows, "
                    "processed_watermark IS NULL "
                    "FROM spanner_sys.row_deletion_policies"),
              IsOkAndHoldsRows({{"events", 0, true}}));
}

TEST_P(SpannerSysStatisticsTest, ListsUserSplitPoints) {
  GOOGLESQL_ASSERT_OK(
      UpdateSchema({"CREATE INDEX users_by_name ON users(name)"}));
  database_api::AddSplitPointsRequest request;
  request.set_database(database()->FullName());
  request.set_initiator("bulk_load");
  database_api::SplitPoints* table_split = request.add_split_points();
  table_split->set_table("users");
  table_split->add_keys()->mutable_key_parts()->add_values()->set_string_value(
      "10");
  database_api::SplitPoints* index_split = request.add_split_points();
  index_split->set_table("users");
  index_split->set_index("users_by_name");
  index_split->add_keys()->mutable_key_parts()->add_values()->set_string_value(
      "m");
  // An expired split point is not listed.
  database_api::SplitPoints* expired_split = request.add_split_points();
  expired_split->set_table("users");
  expired_split->add_keys()
      ->mutable_key_parts()
      ->add_values()
      ->set_string_value("20");
  expired_split->mutable_expire_time()->set_seconds(1);
  GOOGLESQL_ASSERT_OK(AddSplitPoints(request));

  EXPECT_THAT(
      Query("SELECT table_name, index_name, initiator, split_key FROM "
            "spanner_sys.user_split_points ORDER BY split_key"),
      IsOkAndHoldsRows(
          {{"users", "users_by_name", "bulk_load",
            "Index: users_by_name on users, Index Key: (m), Primary Table "
            "Key: (<begin>)"},
           {"users", "", "bulk_load", "users(10)"}}));

  // Split points must name an existing table.
  request.clear_split_points();
  request.add_split_points()->set_table("missing");
  request.mutable_split_points(0)
      ->add_keys()
      ->mutable_key_parts()
      ->add_values()
      ->set_string_value("1");
  EXPECT_THAT(AddSplitPoints(request), StatusIs(absl::StatusCode::kNotFound));
}

}  // namespace

}  // namespace test
}  // namespace emulator
}  // namespace spanner
}  // namespace google

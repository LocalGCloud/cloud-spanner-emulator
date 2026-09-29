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

#include "backend/stats/system_stats_collector.h"

#include <cstdint>
#include <filesystem>
#include <string>
#include <vector>

#include "gmock/gmock.h"
#include "gtest/gtest.h"
#include "googlesql/public/value.h"
#include "absl/status/status.h"
#include "absl/strings/str_cat.h"
#include "absl/time/civil_time.h"
#include "absl/time/time.h"
#include "backend/access/write.h"
#include "backend/datamodel/key.h"
#include "backend/datamodel/key_set.h"
#include "backend/stats/operation_stats.h"

namespace google {
namespace spanner {
namespace emulator {
namespace backend {

namespace {

using ::testing::ElementsAre;
using ::testing::IsEmpty;
using ::testing::Not;
using ::testing::SizeIs;

absl::Time At(int hour, int minute, int second) {
  return absl::FromCivil(absl::CivilSecond(2026, 9, 27, hour, minute, second),
                         absl::UTCTimeZone());
}

QueryExecution Query(std::string text, absl::Duration latency,
                     int64_t rows = 1) {
  QueryExecution query;
  query.text = std::move(text);
  query.latency = latency;
  query.cpu_time = latency / 2;
  query.rows_returned = rows;
  return query;
}

googlesql::Value StringArray(std::vector<std::string> values) {
  return googlesql::values::StringArray(values);
}

// Returns the values of `column` in `rows`.
std::vector<googlesql::Value> Column(const std::vector<SpannerSysRow>& rows,
                                     const std::string& column) {
  std::vector<googlesql::Value> values;
  for (const SpannerSysRow& row : rows) {
    values.push_back(row.at(column));
  }
  return values;
}

TEST(IntervalEndTest, AlignsIntervalsToTheClock) {
  const absl::Time time = At(11, 59, 30);
  EXPECT_EQ(IntervalEnd(time, StatsInterval::kMinute), At(12, 0, 0));
  EXPECT_EQ(IntervalEnd(time, StatsInterval::kTenMinutes), At(12, 0, 0));
  EXPECT_EQ(IntervalEnd(time, StatsInterval::kHour), At(12, 0, 0));

  // Intervals include their start.
  EXPECT_EQ(IntervalEnd(At(11, 40, 0), StatsInterval::kMinute), At(11, 41, 0));
  EXPECT_EQ(IntervalEnd(At(11, 40, 0), StatsInterval::kTenMinutes),
            At(11, 50, 0));
  EXPECT_EQ(IntervalEnd(At(11, 0, 0), StatsInterval::kHour), At(12, 0, 0));
}

TEST(NormalizeTagTest, KeepsPrintableCharactersWithoutLeadingUnderscores) {
  EXPECT_EQ(NormalizeTag("__app=a\tb"), "app=ab");
  EXPECT_EQ(NormalizeTag(std::string(60, 'x')), std::string(50, 'x'));
  EXPECT_EQ(NormalizeTag(""), "");
}

TEST(SystemStatsCollectorTest, ServesStatisticsTables) {
  EXPECT_TRUE(SystemStatsCollector::ServesTable("QUERY_STATS_TOP_MINUTE"));
  EXPECT_TRUE(SystemStatsCollector::ServesTable("TXN_STATS_TOTAL_10MINUTE"));
  EXPECT_TRUE(SystemStatsCollector::ServesTable("LOCK_STATS_TOP_HOUR"));
  EXPECT_TRUE(
      SystemStatsCollector::ServesTable("COLUMN_OPERATIONS_STATS_MINUTE"));
  EXPECT_TRUE(SystemStatsCollector::ServesTable("OLDEST_ACTIVE_QUERIES"));
  EXPECT_TRUE(SystemStatsCollector::ServesTable("ACTIVE_PARTITIONED_DMLS"));
  EXPECT_TRUE(SystemStatsCollector::ServesTable("TABLE_SIZES_STATS_1HOUR"));
  EXPECT_TRUE(SystemStatsCollector::ServesTable("ROW_DELETION_POLICIES"));
  EXPECT_TRUE(SystemStatsCollector::ServesTable("USER_SPLIT_POINTS"));
  EXPECT_FALSE(SystemStatsCollector::ServesTable("QUERY_STATS_TOP_DAY"));
  EXPECT_FALSE(
      SystemStatsCollector::ServesTable("SUPPORTED_OPTIMIZER_VERSIONS"));
}

TEST(SystemStatsCollectorTest, ShowsOnlyClosedIntervalsByDefault) {
  SystemStatsCollector collector;
  collector.RecordQuery(At(11, 59, 30), Query("SELECT 1", absl::Seconds(1)),
                        {});

  const absl::Time before_end = At(11, 59, 59);
  EXPECT_THAT(collector.Snapshot("QUERY_STATS_TOP_MINUTE", before_end,
                                 /*include_open_intervals=*/false, nullptr),
              IsEmpty());
  EXPECT_THAT(collector.Snapshot("QUERY_STATS_TOP_MINUTE", before_end,
                                 /*include_open_intervals=*/true, nullptr),
              SizeIs(1));

  std::vector<SpannerSysRow> rows =
      collector.Snapshot("QUERY_STATS_TOP_MINUTE", At(12, 0, 0),
                         /*include_open_intervals=*/false, nullptr);
  ASSERT_THAT(rows, SizeIs(1));
  EXPECT_EQ(rows[0].at("INTERVAL_END"),
            googlesql::values::Timestamp(At(12, 0, 0)));

  // The 10-minute and hour intervals close later.
  EXPECT_THAT(collector.Snapshot("QUERY_STATS_TOP_10MINUTE", At(12, 0, 0),
                                 false, nullptr),
              SizeIs(1));
  EXPECT_THAT(collector.Snapshot("QUERY_STATS_TOP_HOUR", At(11, 59, 59), false,
                                 nullptr),
              IsEmpty());
}

TEST(SystemStatsCollectorTest, DropsIntervalsAfterTheirRetention) {
  SystemStatsCollector collector;
  collector.RecordQuery(At(1, 0, 0), Query("SELECT 1", absl::Seconds(1)), {});

  // Minute intervals are kept for 6 hours, 10-minute intervals for 4 days.
  const absl::Time later = At(7, 5, 0);
  EXPECT_THAT(
      collector.Snapshot("QUERY_STATS_TOTAL_MINUTE", later, false, nullptr),
      IsEmpty());
  EXPECT_THAT(
      collector.Snapshot("QUERY_STATS_TOTAL_10MINUTE", later, false, nullptr),
      SizeIs(1));
  EXPECT_THAT(
      collector.Snapshot("QUERY_STATS_TOTAL_HOUR", later, false, nullptr),
      SizeIs(1));

  // Recording prunes the expired intervals.
  collector.RecordQuery(later, Query("SELECT 2", absl::Seconds(1)), {});
  std::vector<SpannerSysRow> rows = collector.Snapshot(
      "QUERY_STATS_TOP_MINUTE", later, /*include_open_intervals=*/true,
      nullptr);
  EXPECT_THAT(Column(rows, "TEXT"),
              ElementsAre(googlesql::values::String("SELECT 2")));
  EXPECT_THAT(collector.Snapshot("QUERY_STATS_TOTAL_HOUR",
                                 At(1, 0, 0) + absl::Hours(24 * 31), false,
                                 nullptr),
              IsEmpty());
}

TEST(SystemStatsCollectorTest, AggregatesExecutionsOfAStatement) {
  SystemStatsCollector collector;
  const absl::Time time = At(11, 0, 10);
  collector.RecordQuery(time, Query("SELECT a FROM T", absl::Seconds(1), 2),
                        {});
  QueryExecution rw_query = Query("SELECT a FROM T", absl::Seconds(3), 4);
  rw_query.in_read_write_transaction = true;
  rw_query.rows_scanned = 10;
  collector.RecordQuery(time, rw_query, {});
  QueryExecution failed = Query("SELECT a FROM T", absl::Seconds(5));
  failed.status = absl::StatusCode::kDeadlineExceeded;
  collector.RecordQuery(time, failed, {});

  std::vector<SpannerSysRow> rows =
      collector.Snapshot("QUERY_STATS_TOP_MINUTE", At(11, 1, 0), false,
                         nullptr);
  ASSERT_THAT(rows, SizeIs(1));
  const SpannerSysRow& row = rows[0];
  EXPECT_EQ(row.at("TEXT").string_value(), "SELECT a FROM T");
  EXPECT_FALSE(row.at("TEXT_TRUNCATED").bool_value());
  EXPECT_EQ(row.at("EXECUTION_COUNT").int64_value(), 2);
  EXPECT_EQ(row.at("STATEMENT_COUNT").int64_value(), 2);
  EXPECT_DOUBLE_EQ(row.at("AVG_LATENCY_SECONDS").double_value(), 2);
  EXPECT_DOUBLE_EQ(row.at("AVG_CPU_SECONDS").double_value(), 1);
  EXPECT_DOUBLE_EQ(row.at("AVG_ROWS").double_value(), 3);
  EXPECT_DOUBLE_EQ(row.at("AVG_ROWS_SCANNED").double_value(), 5);
  EXPECT_EQ(row.at("RUN_IN_RW_TRANSACTION_EXECUTION_COUNT").int64_value(), 1);
  EXPECT_EQ(row.at("ALL_FAILED_EXECUTION_COUNT").int64_value(), 1);
  EXPECT_EQ(row.at("TIMED_OUT_EXECUTION_COUNT").int64_value(), 1);
  EXPECT_EQ(row.at("CANCELLED_OR_DISCONNECTED_EXECUTION_COUNT").int64_value(),
            0);
  EXPECT_DOUBLE_EQ(row.at("ALL_FAILED_AVG_LATENCY_SECONDS").double_value(), 5);
  EXPECT_EQ(row.at("QUERY_TYPE").string_value(), "QUERY");
  EXPECT_EQ(row.at("REQUEST_TAG").string_value(), "");
  EXPECT_EQ(row.at("LATENCY_DISTRIBUTION").elements()[0].field(0).int64_value(),
            2);

  // The fingerprint is stable for the same text.
  SystemStatsCollector other;
  other.RecordQuery(time, Query("SELECT a FROM T", absl::Seconds(1)), {});
  EXPECT_EQ(other.Snapshot("QUERY_STATS_TOP_MINUTE", At(11, 1, 0), false,
                           nullptr)[0]
                .at("TEXT_FINGERPRINT"),
            row.at("TEXT_FINGERPRINT"));

  rows = collector.Snapshot("QUERY_STATS_TOTAL_MINUTE", At(11, 1, 0), false,
                            nullptr);
  ASSERT_THAT(rows, SizeIs(1));
  EXPECT_EQ(rows[0].at("EXECUTION_COUNT").int64_value(), 2);
  EXPECT_FALSE(rows[0].contains("TEXT"));
}

TEST(SystemStatsCollectorTest, GroupsTaggedStatementsByTag) {
  SystemStatsCollector collector;
  const absl::Time time = At(11, 0, 10);
  QueryExecution first = Query("SELECT 1", absl::Seconds(1));
  first.request_tag = "_report";
  QueryExecution second = Query("SELECT 2", absl::Seconds(1));
  second.request_tag = "report";
  collector.RecordQuery(time, first, {});
  collector.RecordQuery(time, second, {});

  std::vector<SpannerSysRow> rows =
      collector.Snapshot("QUERY_STATS_TOP_MINUTE", At(11, 1, 0), false,
                         nullptr);
  ASSERT_THAT(rows, SizeIs(1));
  EXPECT_EQ(rows[0].at("REQUEST_TAG").string_value(), "report");
  EXPECT_EQ(rows[0].at("TEXT").string_value(), "SELECT 1");
  EXPECT_EQ(rows[0].at("EXECUTION_COUNT").int64_value(), 2);
}

TEST(SystemStatsCollectorTest, TruncatesLongText) {
  SystemStatsCollector collector;
  const std::string text =
      absl::StrCat("SELECT '", std::string(SystemStatsCollector::kMaxTextBytes,
                                           'x'),
                   "'");
  collector.RecordQuery(At(11, 0, 10), Query(text, absl::Seconds(1)), {});
  std::vector<SpannerSysRow> rows =
      collector.Snapshot("QUERY_STATS_TOP_MINUTE", At(11, 1, 0), false,
                         nullptr);
  ASSERT_THAT(rows, SizeIs(1));
  EXPECT_TRUE(rows[0].at("TEXT_TRUNCATED").bool_value());
  EXPECT_EQ(rows[0].at("TEXT").string_value().size(),
            SystemStatsCollector::kMaxTextBytes);
}

TEST(SystemStatsCollectorTest, KeepsTheTopStatementsByCpuTime) {
  SystemStatsCollector collector;
  const absl::Time time = At(11, 0, 10);
  const int statements = SystemStatsCollector::kTopN + 1;
  for (int i = 0; i < statements; ++i) {
    collector.RecordQuery(
        time, Query(absl::StrCat("SELECT ", i), absl::Milliseconds(i + 1)),
        {});
  }

  std::vector<SpannerSysRow> rows =
      collector.Snapshot("QUERY_STATS_TOP_MINUTE", At(11, 1, 0), false,
                         nullptr);
  ASSERT_THAT(rows, SizeIs(SystemStatsCollector::kTopN));
  // Rows are ranked by CPU time; the cheapest statement was evicted.
  EXPECT_EQ(rows.front().at("TEXT").string_value(),
            absl::StrCat("SELECT ", statements - 1));
  EXPECT_EQ(rows.back().at("TEXT").string_value(), "SELECT 1");

  // TOTAL includes the evicted statement.
  rows = collector.Snapshot("QUERY_STATS_TOTAL_MINUTE", At(11, 1, 0), false,
                            nullptr);
  ASSERT_THAT(rows, SizeIs(1));
  EXPECT_EQ(rows[0].at("EXECUTION_COUNT").int64_value(), statements);
}

TEST(SystemStatsCollectorTest, RecordsReadsAndOperations) {
  SystemStatsCollector collector;
  const absl::Time time = At(11, 0, 10);
  ReadExecution read;
  read.table = "Singers";
  read.columns = {"Name", "Id"};
  read.rows = 2;
  read.bytes = 20;
  collector.RecordRead(time, read);
  collector.RecordRead(time, read);

  std::vector<SpannerSysRow> rows =
      collector.Snapshot("READ_STATS_TOP_MINUTE", At(11, 1, 0), false, nullptr);
  ASSERT_THAT(rows, SizeIs(1));
  EXPECT_EQ(rows[0].at("READ_COLUMNS"),
            StringArray({"Singers.Id", "Singers.Name"}));
  EXPECT_EQ(rows[0].at("EXECUTION_COUNT").int64_value(), 2);
  EXPECT_DOUBLE_EQ(rows[0].at("AVG_BYTES").double_value(), 20);
  EXPECT_EQ(rows[0].at("READ_TYPE").string_value(), "READ");

  AccessFootprint query_footprint;
  query_footprint.AddRead("Singers", {"Id"});
  collector.RecordQuery(time, Query("SELECT Id FROM Singers", absl::Seconds(1)),
                        query_footprint);
  Mutation mutation;
  mutation.AddWriteOp(MutationOpType::kInsert, "Albums", {"Id", "Title"},
                      {{googlesql::values::Int64(1),
                        googlesql::values::String("x")}});
  AccessFootprint write_footprint;
  write_footprint.AddMutation(mutation);
  collector.RecordMutations(time, write_footprint);

  rows = collector.Snapshot("TABLE_OPERATIONS_STATS_MINUTE", At(11, 1, 0),
                            false, nullptr);
  ASSERT_THAT(rows, SizeIs(2));
  EXPECT_EQ(rows[0].at("TABLE_NAME").string_value(), "Albums");
  EXPECT_EQ(rows[0].at("WRITE_COUNT").int64_value(), 1);
  EXPECT_EQ(rows[1].at("TABLE_NAME").string_value(), "Singers");
  EXPECT_EQ(rows[1].at("READ_QUERY_COUNT").int64_value(), 3);

  rows = collector.Snapshot("COLUMN_OPERATIONS_STATS_MINUTE", At(11, 1, 0),
                            false, nullptr);
  ASSERT_THAT(rows, SizeIs(4));
  // Singers.Id was read twice and queried once.
  EXPECT_EQ(rows[2].at("COLUMN_NAME").string_value(), "Id");
  EXPECT_EQ(rows[2].at("READ_COUNT").int64_value(), 2);
  EXPECT_EQ(rows[2].at("QUERY_COUNT").int64_value(), 1);
  EXPECT_FALSE(rows[2].at("IS_QUERY_CACHE_MEMORY_CAPPED").bool_value());
}

TEST(SystemStatsCollectorTest, RecordsTransactionAttempts) {
  SystemStatsCollector collector;
  const absl::Time time = At(11, 0, 10);
  Mutation mutation;
  mutation.AddWriteOp(MutationOpType::kInsert, "Singers", {"Id", "Name"},
                      {{googlesql::values::Int64(1),
                        googlesql::values::String("abc")}});
  mutation.AddDeleteOp("Albums", KeySet::All());

  TransactionAttempt aborted;
  aborted.transaction_tag = "app";
  aborted.outcome = TransactionAttempt::Outcome::kAborted;
  aborted.total_latency = absl::Seconds(1);
  aborted.footprint.AddRead("Singers", {"Name"});
  collector.RecordTransactionAttempt(time, aborted);

  TransactionAttempt committed;
  committed.transaction_tag = "app";
  committed.retry = true;
  committed.commit_attempted = true;
  committed.total_latency = absl::Seconds(3);
  committed.commit_latency = absl::Seconds(2);
  committed.footprint.AddMutation(mutation);
  collector.RecordTransactionAttempt(time, committed);

  std::vector<SpannerSysRow> rows =
      collector.Snapshot("TXN_STATS_TOP_MINUTE", At(11, 1, 0), false, nullptr);
  ASSERT_THAT(rows, SizeIs(1));
  const SpannerSysRow& row = rows[0];
  EXPECT_EQ(row.at("TRANSACTION_TAG").string_value(), "app");
  EXPECT_EQ(row.at("ATTEMPT_COUNT").int64_value(), 2);
  EXPECT_EQ(row.at("COMMIT_ATTEMPT_COUNT").int64_value(), 1);
  EXPECT_EQ(row.at("COMMIT_ABORT_COUNT").int64_value(), 1);
  EXPECT_EQ(row.at("COMMIT_RETRY_COUNT").int64_value(), 1);
  EXPECT_EQ(row.at("SERIALIZABLE_PESSIMISTIC_TXN_COUNT").int64_value(), 2);
  EXPECT_DOUBLE_EQ(row.at("AVG_TOTAL_LATENCY_SECONDS").double_value(), 2);
  EXPECT_DOUBLE_EQ(row.at("AVG_COMMIT_LATENCY_SECONDS").double_value(), 2);
  EXPECT_DOUBLE_EQ(row.at("AVG_PARTICIPANTS").double_value(), 1);
  EXPECT_EQ(row.at("READ_COLUMNS"),
            StringArray({"Singers.Name"}));
  EXPECT_EQ(row.at("WRITE_CONSTRUCTIVE_COLUMNS"),
            StringArray({"Singers.Id", "Singers.Name"}));
  EXPECT_EQ(row.at("WRITE_DELETE_TABLES"),
            StringArray({"Albums"}));
  const googlesql::Value& operations = row.at("OPERATIONS_BY_TABLE");
  ASSERT_EQ(operations.num_elements(), 1);
  EXPECT_EQ(operations.element(0).field(0).string_value(), "Singers");
  EXPECT_EQ(operations.element(0).field(1).int64_value(), 1);
  EXPECT_EQ(operations.element(0).field(2).int64_value(), 11);
  EXPECT_EQ(row.at("OPERATIONS_BY_TABLE_JSON_STRING").string_value(),
            R"([{"INSERT_OR_UPDATE_BYTES":11,"INSERT_OR_UPDATE_COUNT":1,)"
            R"("TABLE_NAME":"Singers"}])");
}

TEST(SystemStatsCollectorTest, RecordsConflictsOfRegisteredTransactions) {
  SystemStatsCollector collector;
  const absl::Time time = At(11, 0, 10);
  LockConflict conflict;
  conflict.table_id = "t1";
  conflict.start_key = Key({googlesql::values::Int64(2)});
  conflict.requests = {{/*transaction_id=*/1, /*exclusive=*/true, {"c1"}},
                       {/*transaction_id=*/2, /*exclusive=*/false, {}}};

  // Conflicts of internal transactions are not recorded.
  collector.RecordLockConflict(time, conflict);
  EXPECT_THAT(
      collector.Snapshot("LOCK_STATS_TOP_MINUTE", At(11, 1, 0), false, nullptr),
      IsEmpty());

  collector.RegisterTransaction(1, "writer");
  collector.RecordLockConflict(time, conflict);
  collector.RecordLockConflict(time, conflict);
  collector.UnregisterTransaction(1);

  std::vector<SpannerSysRow> rows =
      collector.Snapshot("LOCK_STATS_TOP_MINUTE", At(11, 1, 0), false, nullptr);
  ASSERT_THAT(rows, SizeIs(1));
  EXPECT_EQ(rows[0].at("ROW_RANGE_START_KEY").bytes_value(), "t1(2)");
  EXPECT_EQ(rows[0].at("LOCK_WAIT_SECONDS").double_value(), 0);
  EXPECT_EQ(rows[0].at("SAMPLE_LOCK_REQUESTS_JSON_STRING").string_value(),
            R"([{"COLUMN":"t1.c1","LOCK_MODE":"Exclusive",)"
            R"("TRANSACTION_TAG":"writer"},{"COLUMN":"t1._exists",)"
            R"("LOCK_MODE":"ReaderShared","TRANSACTION_TAG":""}])");
  rows = collector.Snapshot("LOCK_STATS_TOTAL_MINUTE", At(11, 1, 0), false,
                            nullptr);
  ASSERT_THAT(rows, SizeIs(1));
  EXPECT_EQ(rows[0].at("TOTAL_LOCK_WAIT_SECONDS").double_value(), 0);
}

TEST(SystemStatsCollectorTest, AccumulatesAndRanksByLockWait) {
  SystemStatsCollector collector;
  collector.RegisterTransaction(1, "");
  const absl::Time time = At(11, 0, 10);
  auto conflict = [](int64_t key, absl::Duration lock_wait) {
    LockConflict conflict;
    conflict.table_id = "t1";
    conflict.start_key = Key({googlesql::values::Int64(key)});
    conflict.requests = {{/*transaction_id=*/1, /*exclusive=*/true, {}}};
    conflict.lock_wait = lock_wait;
    return conflict;
  };
  // Key 1 has more conflicts, but key 2 waited longer.
  collector.RecordLockConflict(time, conflict(1, absl::ZeroDuration()));
  collector.RecordLockConflict(time, conflict(1, absl::ZeroDuration()));
  collector.RecordLockConflict(time, conflict(1, absl::Milliseconds(250)));
  collector.RecordLockConflict(time, conflict(2, absl::Milliseconds(1500)));

  std::vector<SpannerSysRow> rows =
      collector.Snapshot("LOCK_STATS_TOP_MINUTE", At(11, 1, 0), false, nullptr);
  ASSERT_THAT(rows, SizeIs(2));
  EXPECT_EQ(rows[0].at("ROW_RANGE_START_KEY").bytes_value(), "t1(2)");
  EXPECT_EQ(rows[0].at("LOCK_WAIT_SECONDS").double_value(), 1.5);
  EXPECT_EQ(rows[1].at("ROW_RANGE_START_KEY").bytes_value(), "t1(1)");
  EXPECT_EQ(rows[1].at("LOCK_WAIT_SECONDS").double_value(), 0.25);
  rows = collector.Snapshot("LOCK_STATS_TOTAL_MINUTE", At(11, 1, 0), false,
                            nullptr);
  ASSERT_THAT(rows, SizeIs(1));
  EXPECT_EQ(rows[0].at("TOTAL_LOCK_WAIT_SECONDS").double_value(), 1.75);
}

// Records statistics for every interval-based table at `time`.
void RecordEveryKind(SystemStatsCollector& collector, absl::Time time) {
  AccessFootprint read_footprint;
  read_footprint.AddRead("Singers", {"Name"});
  collector.RecordQuery(time, Query("SELECT 1", absl::Seconds(1)),
                        read_footprint);
  QueryExecution tagged = Query("SELECT 2", absl::Milliseconds(5));
  tagged.request_tag = "app=report";
  collector.RecordQuery(time, tagged, {});

  ReadExecution read;
  read.table = "Singers";
  read.columns = {"Name"};
  read.rows = 3;
  collector.RecordRead(time, read);

  TransactionAttempt attempt;
  attempt.transaction_tag = "writer";
  attempt.footprint.written_columns["Singers"].insert("Name");
  attempt.footprint.writes["Singers"] = {1, 11};
  attempt.footprint.deleted_tables.insert("Albums");
  attempt.commit_attempted = true;
  attempt.total_latency = absl::Milliseconds(3);
  collector.RecordTransactionAttempt(time, attempt);
  collector.RecordMutations(time, attempt.footprint);

  collector.RegisterTransaction(1, "writer");
  LockConflict conflict;
  conflict.table_id = "t1";
  conflict.start_key = Key({googlesql::values::Int64(2)});
  conflict.requests = {{/*transaction_id=*/1, /*exclusive=*/true, {"c1"}}};
  conflict.lock_wait = absl::Milliseconds(20);
  collector.RecordLockConflict(time, conflict);
  collector.UnregisterTransaction(1);
}

TEST(SystemStatsCollectorTest, RestoresSavedStatistics) {
  SystemStatsCollector collector;
  RecordEveryKind(collector, At(11, 0, 10));

  SystemStatsCollector restored;
  ASSERT_TRUE(restored.Restore(collector.Save(), At(11, 1, 0)).ok());
  for (const char* table :
       {"QUERY_STATS_TOP_MINUTE", "QUERY_STATS_TOTAL_10MINUTE",
        "READ_STATS_TOP_HOUR", "READ_STATS_TOTAL_MINUTE",
        "TXN_STATS_TOP_MINUTE", "TXN_STATS_TOTAL_HOUR",
        "LOCK_STATS_TOP_MINUTE", "LOCK_STATS_TOTAL_10MINUTE",
        "TABLE_OPERATIONS_STATS_MINUTE", "COLUMN_OPERATIONS_STATS_HOUR"}) {
    SCOPED_TRACE(table);
    std::vector<SpannerSysRow> expected =
        collector.Snapshot(table, At(12, 0, 0), false, nullptr);
    EXPECT_THAT(expected, Not(IsEmpty()));
    EXPECT_EQ(restored.Snapshot(table, At(12, 0, 0), false, nullptr),
              expected);
  }

  // Restored statistics keep accumulating.
  restored.RecordQuery(At(11, 0, 20), Query("SELECT 1", absl::Seconds(1)), {});
  std::vector<SpannerSysRow> rows =
      restored.Snapshot("QUERY_STATS_TOP_MINUTE", At(11, 1, 0), false, nullptr);
  ASSERT_THAT(rows, SizeIs(2));
  EXPECT_EQ(rows[0].at("TEXT").string_value(), "SELECT 1");
  EXPECT_EQ(rows[0].at("EXECUTION_COUNT").int64_value(), 2);
}

TEST(SystemStatsCollectorTest, RestoreDropsStatisticsPastRetention) {
  SystemStatsCollector collector;
  collector.RecordQuery(At(11, 0, 10), Query("SELECT 1", absl::Seconds(1)),
                        {});

  SystemStatsCollector restored;
  const absl::Time now = At(11, 0, 10) + absl::Hours(7);
  ASSERT_TRUE(restored.Restore(collector.Save(), now).ok());
  EXPECT_THAT(
      restored.Snapshot("QUERY_STATS_TOP_MINUTE", now, false, nullptr),
      IsEmpty());
  EXPECT_THAT(restored.Snapshot("QUERY_STATS_TOP_HOUR", now, false, nullptr),
              SizeIs(1));
}

TEST(SystemStatsCollectorTest, RestoreRejectsMalformedStatistics) {
  SystemStatsCollector collector;
  collector.RecordQuery(At(11, 0, 10), Query("SELECT 1", absl::Seconds(1)),
                        {});
  EXPECT_EQ(collector.Restore("{", At(11, 1, 0)).code(),
            absl::StatusCode::kDataLoss);
  EXPECT_EQ(collector.Restore(R"({"version": 2})", At(11, 1, 0)).code(),
            absl::StatusCode::kDataLoss);
  EXPECT_EQ(collector.Restore(R"({"version": 1})", At(11, 1, 0)).code(),
            absl::StatusCode::kDataLoss);
  EXPECT_THAT(
      collector.Snapshot("QUERY_STATS_TOP_MINUTE", At(11, 1, 0), false,
                         nullptr),
      SizeIs(1));
}

TEST(SystemStatsCollectorTest, PersistsWhenAnIntervalEndsAndOnRequest) {
  const std::string path =
      (std::filesystem::path(testing::TempDir()) / "persisted_stats.json")
          .string();
  std::filesystem::remove(path);
  {
    SystemStatsCollector collector(path, At(11, 0, 0));
    collector.RecordQuery(At(11, 0, 10), Query("SELECT 1", absl::Seconds(1)),
                          {});
    EXPECT_FALSE(std::filesystem::exists(path));
    // A statement in the next minute ends the first interval, which saves the
    // statistics recorded so far.
    collector.RecordQuery(At(11, 1, 10), Query("SELECT 1", absl::Seconds(1)),
                          {});
    EXPECT_TRUE(std::filesystem::exists(path));
  }

  SystemStatsCollector reloaded(path, At(11, 3, 0));
  std::vector<SpannerSysRow> rows =
      reloaded.Snapshot("QUERY_STATS_TOP_MINUTE", At(11, 3, 0), false, nullptr);
  ASSERT_THAT(rows, SizeIs(1));
  EXPECT_EQ(rows[0].at("INTERVAL_END"),
            googlesql::values::Timestamp(At(11, 1, 0)));

  // Persist() also saves the statistics of the interval in progress.
  reloaded.RecordQuery(At(11, 3, 10), Query("SELECT 1", absl::Seconds(1)),
                       {});
  reloaded.Persist();
  SystemStatsCollector reloaded_again(path, At(11, 5, 0));
  EXPECT_THAT(reloaded_again.Snapshot("QUERY_STATS_TOP_MINUTE", At(11, 5, 0),
                                      false, nullptr),
              SizeIs(2));
}

TEST(SystemStatsCollectorTest, DoesNotCreateMissingPersistenceDirectory) {
  const std::filesystem::path directory =
      std::filesystem::path(testing::TempDir()) / "deleted_database";
  std::filesystem::remove_all(directory);
  SystemStatsCollector collector((directory / "stats.json").string(),
                                 At(11, 0, 0));
  collector.RecordQuery(At(11, 0, 10), Query("SELECT 1", absl::Seconds(1)),
                        {});
  collector.Persist();
  EXPECT_FALSE(std::filesystem::exists(directory));
}

TEST(SystemStatsCollectorTest, ListsActiveQueries) {
  SystemStatsCollector collector;
  ActiveQuery old_query;
  old_query.text = "SELECT old";
  old_query.start_time = At(11, 0, 0);
  ActiveQuery new_query;
  new_query.text = "SELECT new";
  new_query.start_time = At(11, 0, 55);
  const int64_t new_id = collector.StartQuery(new_query);
  const int64_t old_id = collector.StartQuery(old_query);

  const absl::Time now = At(11, 1, 0);
  std::vector<SpannerSysRow> rows =
      collector.Snapshot("OLDEST_ACTIVE_QUERIES", now, false, nullptr);
  EXPECT_THAT(Column(rows, "TEXT"),
              ElementsAre(googlesql::values::String("SELECT old"),
                          googlesql::values::String("SELECT new")));
  EXPECT_EQ(rows[0].at("QUERY_ID").string_value(), absl::StrCat(old_id));

  rows = collector.Snapshot("ACTIVE_QUERIES_SUMMARY", now, false, nullptr);
  ASSERT_THAT(rows, SizeIs(1));
  EXPECT_EQ(rows[0].at("ACTIVE_COUNT").int64_value(), 2);
  EXPECT_EQ(rows[0].at("OLDEST_START_TIME"),
            googlesql::values::Timestamp(At(11, 0, 0)));
  EXPECT_EQ(rows[0].at("COUNT_OLDER_THAN_1S").int64_value(), 2);
  EXPECT_EQ(rows[0].at("COUNT_OLDER_THAN_10S").int64_value(), 1);
  EXPECT_EQ(rows[0].at("COUNT_OLDER_THAN_100S").int64_value(), 0);

  collector.EndQuery(old_id);
  collector.EndQuery(new_id);
  rows = collector.Snapshot("ACTIVE_QUERIES_SUMMARY", now, false, nullptr);
  EXPECT_EQ(rows[0].at("ACTIVE_COUNT").int64_value(), 0);
  EXPECT_TRUE(rows[0].at("OLDEST_START_TIME").is_null());
}

TEST(SystemStatsCollectorTest, ListsActivePartitionedDmls) {
  SystemStatsCollector collector;
  const int64_t id = collector.StartPartitionedDml(
      {.text = "DELETE FROM T WHERE true",
       .session_id = "session",
       .start_time = At(11, 0, 0)});

  std::vector<SpannerSysRow> rows = collector.Snapshot(
      "ACTIVE_PARTITIONED_DMLS", At(11, 0, 1), false, nullptr);
  ASSERT_THAT(rows, SizeIs(1));
  EXPECT_EQ(rows[0].at("TEXT").string_value(), "DELETE FROM T WHERE true");
  EXPECT_EQ(rows[0].at("SESSION_ID").string_value(), "session");
  EXPECT_EQ(rows[0].at("NUM_PARTITIONS_TOTAL").int64_value(), 1);
  EXPECT_EQ(rows[0].at("NUM_PARTITIONS_COMPLETE").int64_value(), 0);
  EXPECT_EQ(rows[0].at("PROGRESS").double_value(), 0);
  EXPECT_EQ(rows[0].at("START_TIMESTAMP"),
            googlesql::values::Timestamp(At(11, 0, 0)));

  collector.EndPartitionedDml(id);
  EXPECT_THAT(collector.Snapshot("ACTIVE_PARTITIONED_DMLS", At(11, 0, 2), false,
                                 nullptr),
              IsEmpty());
}

TEST(SystemStatsCollectorTest, AveragesTableSizeSamplesPerHour) {
  SystemStatsCollector collector;
  collector.RecordTableSizes(At(10, 5, 0), {{"Singers", 100}, {"Idx", 10}});
  collector.RecordTableSizes(At(10, 10, 0), {{"Singers", 300}});
  collector.RecordTableSizes(At(11, 5, 0), {{"Singers", 1000}});

  // Only the closed hour is shown, averaged over its samples.
  std::vector<SpannerSysRow> rows = collector.Snapshot(
      "TABLE_SIZES_STATS_1HOUR", At(11, 10, 0), false, nullptr);
  EXPECT_THAT(Column(rows, "TABLE_NAME"),
              ElementsAre(googlesql::values::String("Idx"),
                          googlesql::values::String("Singers")));
  EXPECT_THAT(Column(rows, "USED_BYTES"),
              ElementsAre(googlesql::values::Double(10),
                          googlesql::values::Double(200)));
  EXPECT_THAT(Column(rows, "USED_SSD_BYTES"),
              ElementsAre(googlesql::values::Double(10),
                          googlesql::values::Double(200)));
  EXPECT_THAT(Column(rows, "INTERVAL_END"),
              ElementsAre(googlesql::values::Timestamp(At(11, 0, 0)),
                          googlesql::values::Timestamp(At(11, 0, 0))));

  EXPECT_THAT(collector.Snapshot("TABLE_SIZES_STATS_1HOUR", At(11, 10, 0),
                                 /*include_open_intervals=*/true, nullptr),
              SizeIs(3));
}

TEST(SystemStatsCollectorTest, RestoresTableSizesAndKeepsCurrentState) {
  SystemStatsCollector collector;
  collector.RecordTableSizes(At(10, 5, 0), {{"Singers", 100}});
  collector.RecordTableSizes(At(10, 10, 0), {{"Singers", 300}});

  // Split points and executing partitioned DMLs are not in the saved
  // statistics, so a restore keeps the ones the collector already has.
  SystemStatsCollector restored;
  restored.AddUserSplitPoint({.table_name = "Singers",
                              .initiator = "test",
                              .split_key = "(1)",
                              .expire_time = At(12, 0, 0)});
  restored.StartPartitionedDml({});
  ASSERT_TRUE(restored.Restore(collector.Save(), At(11, 0, 0)).ok());

  std::vector<SpannerSysRow> rows = restored.Snapshot(
      "TABLE_SIZES_STATS_1HOUR", At(11, 10, 0), false, nullptr);
  EXPECT_THAT(Column(rows, "USED_BYTES"),
              ElementsAre(googlesql::values::Double(200)));
  EXPECT_THAT(restored.Snapshot("USER_SPLIT_POINTS", At(11, 0, 0), false,
                                nullptr),
              SizeIs(1));
  EXPECT_THAT(restored.Snapshot("ACTIVE_PARTITIONED_DMLS", At(11, 0, 0),
                                false, nullptr),
              SizeIs(1));

  // Table size samples past their retention are dropped.
  SystemStatsCollector expired;
  ASSERT_TRUE(
      expired.Restore(collector.Save(), At(10, 5, 0) + absl::Hours(31 * 24))
          .ok());
  EXPECT_THAT(expired.Snapshot("TABLE_SIZES_STATS_1HOUR",
                               At(10, 5, 0) + absl::Hours(31 * 24), false,
                               nullptr),
              IsEmpty());
}

TEST(SystemStatsCollectorTest, ListsUnexpiredUserSplitPoints) {
  SystemStatsCollector collector;
  collector.AddUserSplitPoint({.table_name = "T",
                               .initiator = "load",
                               .split_key = "T(1)",
                               .expire_time = At(12, 0, 0)});
  collector.AddUserSplitPoint({.table_name = "T",
                               .index_name = "I",
                               .initiator = "load",
                               .split_key = "Index: I on T, Index Key: (a)",
                               .expire_time = At(10, 0, 0)});
  // Adding a split point again replaces it.
  collector.AddUserSplitPoint({.table_name = "T",
                               .initiator = "again",
                               .split_key = "T(1)",
                               .expire_time = At(13, 0, 0)});

  std::vector<SpannerSysRow> rows =
      collector.Snapshot("USER_SPLIT_POINTS", At(11, 0, 0), false, nullptr);
  ASSERT_THAT(rows, SizeIs(1));
  EXPECT_EQ(rows[0].at("TABLE_NAME").string_value(), "T");
  EXPECT_EQ(rows[0].at("INDEX_NAME").string_value(), "");
  EXPECT_EQ(rows[0].at("INITIATOR").string_value(), "again");
  EXPECT_EQ(rows[0].at("SPLIT_KEY").string_value(), "T(1)");
  EXPECT_EQ(rows[0].at("EXPIRE_TIME"),
            googlesql::values::Timestamp(At(13, 0, 0)));
}

}  // namespace

}  // namespace backend
}  // namespace emulator
}  // namespace spanner
}  // namespace google

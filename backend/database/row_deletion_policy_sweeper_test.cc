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

#include "backend/database/row_deletion_policy_sweeper.h"

#include <cstdint>
#include <memory>
#include <ostream>
#include <string>
#include <utility>
#include <vector>

#include "googlesql/public/value.h"
#include "gmock/gmock.h"
#include "gtest/gtest.h"
#include "googlesql/base/testing/status_matchers.h"
#include "absl/container/flat_hash_set.h"
#include "absl/flags/flag.h"
#include "absl/functional/bind_front.h"
#include "absl/status/status.h"
#include "absl/status/statusor.h"
#include "absl/time/clock.h"
#include "absl/time/time.h"
#include "backend/access/read.h"
#include "backend/access/write.h"
#include "backend/actions/change_stream.h"
#include "backend/database/change_stream/change_stream_partition_churner.h"
#include "backend/database/database.h"
#include "backend/datamodel/key.h"
#include "backend/datamodel/key_set.h"
#include "backend/schema/updater/schema_updater.h"
#include "backend/stats/system_stats_collector.h"
#include "backend/transaction/options.h"
#include "backend/transaction/read_only_transaction.h"
#include "backend/transaction/read_write_transaction.h"
#include "common/clock.h"
#include "common/config.h"
#include "googlesql/base/status_macros.h"

namespace google {
namespace spanner {
namespace emulator {
namespace backend {
namespace {

using ::googlesql::values::Int64;
using ::googlesql::values::NullTimestamp;
using ::googlesql::values::Timestamp;
using ::testing::ElementsAre;
using ::testing::IsEmpty;
using ::testing::UnorderedElementsAre;

constexpr char kDatabaseId[] = "test-db";

// A data change record's table, mod type, transaction tag and system
// transaction flag.
struct Record {
  std::string table_name;
  std::string mod_type;
  std::string transaction_tag;
  bool is_system_transaction;

  bool operator==(const Record&) const = default;

  friend void PrintTo(const Record& record, std::ostream* os) {
    *os << "{" << record.table_name << ", " << record.mod_type << ", \""
        << record.transaction_tag << "\", " << record.is_system_transaction
        << "}";
  }
};

Record UserRecord(std::string table_name, std::string mod_type) {
  return Record{std::move(table_name), std::move(mod_type), "", false};
}

Record TtlDeleteRecord(std::string table_name) {
  return Record{std::move(table_name), "DELETE",
                kRowDeletionPolicyTransactionTag, true};
}

class RowDeletionPolicySweeperTest : public ::testing::Test {
 protected:
  void SetUp() override {
    // Tests sweep explicitly. Stop the background threads that could run
    // concurrent transactions.
    config::set_row_deletion_policy_sweep_interval_seconds(0);
    absl::SetFlag(&FLAGS_enable_change_stream_churning, false);
  }

  void TearDown() override {
    config::set_row_deletion_policy_sweep_interval_seconds(
        previous_sweep_interval_seconds_);
    absl::SetFlag(&FLAGS_enable_change_stream_churning,
                  previous_enable_churning_);
  }

  absl::StatusOr<std::unique_ptr<Database>> CreateDatabase(
      const std::vector<std::string>& statements,
      database_api::DatabaseDialect dialect =
          database_api::DatabaseDialect::GOOGLE_STANDARD_SQL) {
    return Database::Create(
        &clock_, kDatabaseId,
        SchemaChangeOperation{.statements = statements,
                              .database_dialect = dialect});
  }

  absl::Status Commit(Database* database, const Mutation& mutation) {
    GOOGLESQL_ASSIGN_OR_RETURN(
        std::unique_ptr<ReadWriteTransaction> txn,
        database->CreateReadWriteTransaction(ReadWriteOptions(), RetryState()));
    GOOGLESQL_RETURN_IF_ERROR(txn->Write(mutation));
    return txn->Commit();
  }

  absl::Status UpdateSchema(Database* database,
                            const std::vector<std::string>& statements) {
    int num_successful_statements;
    absl::Time commit_timestamp;
    absl::Status backfill_status;
    GOOGLESQL_RETURN_IF_ERROR(database->UpdateSchema(
        SchemaChangeOperation{.statements = statements},
        &num_successful_statements, &commit_timestamp, &backfill_status));
    return backfill_status;
  }

  absl::StatusOr<std::vector<std::vector<googlesql::Value>>> Read(
      Database* database, const ReadArg& read_arg) {
    GOOGLESQL_ASSIGN_OR_RETURN(
        std::unique_ptr<ReadOnlyTransaction> txn,
        database->CreateReadOnlyTransaction(ReadOnlyOptions()));
    std::unique_ptr<RowCursor> cursor;
    GOOGLESQL_RETURN_IF_ERROR(txn->Read(read_arg, &cursor));
    std::vector<std::vector<googlesql::Value>> rows;
    while (cursor->Next()) {
      std::vector<googlesql::Value>& row = rows.emplace_back();
      for (int i = 0; i < cursor->NumColumns(); ++i) {
        row.push_back(cursor->ColumnValue(i));
      }
    }
    GOOGLESQL_RETURN_IF_ERROR(cursor->Status());
    return rows;
  }

  // Returns the INT64 `column` of every row of `table`, or of `index` if set.
  std::vector<int64_t> ReadInt64Column(Database* database,
                                       const std::string& table,
                                       const std::string& column,
                                       const std::string& index = "") {
    ReadArg read_arg;
    read_arg.table = table;
    read_arg.index = index;
    read_arg.key_set = KeySet::All();
    read_arg.columns = {column};
    absl::StatusOr<std::vector<std::vector<googlesql::Value>>> rows =
        Read(database, read_arg);
    EXPECT_TRUE(rows.ok()) << rows.status();
    std::vector<int64_t> values;
    if (!rows.ok()) return values;
    for (const std::vector<googlesql::Value>& row : *rows) {
      values.push_back(row[0].int64_value());
    }
    return values;
  }

  // Returns the data change records of `stream`, and adds the IDs of the
  // system transactions that wrote them to `system_transaction_ids`.
  std::vector<Record> ReadChangeStreamRecords(
      Database* database, const std::string& stream,
      absl::flat_hash_set<std::string>* system_transaction_ids = nullptr) {
    ReadArg read_arg;
    read_arg.change_stream_for_data_table = stream;
    read_arg.key_set = KeySet::All();
    read_arg.columns = {"table_name", "mod_type", "transaction_tag",
                        "is_system_transaction", "server_transaction_id"};
    absl::StatusOr<std::vector<std::vector<googlesql::Value>>> rows =
        Read(database, read_arg);
    EXPECT_TRUE(rows.ok()) << rows.status();
    std::vector<Record> records;
    if (!rows.ok()) return records;
    for (const std::vector<googlesql::Value>& row : *rows) {
      records.push_back(Record{row[0].string_value(), row[1].string_value(),
                               row[2].string_value(), row[3].bool_value()});
      if (row[3].bool_value() && system_transaction_ids != nullptr) {
        system_transaction_ids->insert(row[4].string_value());
      }
    }
    return records;
  }

  Clock clock_;
  const absl::Time now_ = absl::FromUnixMicros(absl::ToUnixMicros(absl::Now()));
  const googlesql::Value expired_ = Timestamp(now_ - absl::Hours(48));
  const googlesql::Value barely_expired_ =
      Timestamp(now_ - absl::Hours(24) - absl::Minutes(1));
  const googlesql::Value unexpired_ = Timestamp(now_ - absl::Hours(23));

 private:
  const int previous_sweep_interval_seconds_ = absl::ToInt64Seconds(
      config::row_deletion_policy_sweep_interval());
  const bool previous_enable_churning_ =
      absl::GetFlag(FLAGS_enable_change_stream_churning);
};

TEST_F(RowDeletionPolicySweeperTest,
       DeletesExpiredRowsWithInterleavedChildrenAndIndexEntries) {
  GOOGLESQL_ASSERT_OK_AND_ASSIGN(
      std::unique_ptr<Database> database,
      CreateDatabase({R"(
        CREATE TABLE T (
          k INT64 NOT NULL,
          ts TIMESTAMP,
          v INT64,
        ) PRIMARY KEY (k),
          ROW DELETION POLICY (OLDER_THAN(ts, INTERVAL 1 DAY)))",
                      R"(
        CREATE TABLE C (
          k INT64 NOT NULL,
          c INT64 NOT NULL,
        ) PRIMARY KEY (k, c),
          INTERLEAVE IN PARENT T ON DELETE CASCADE)",
                      "CREATE INDEX TByV ON T(v)"}));
  Mutation mutation;
  mutation.AddWriteOp(MutationOpType::kInsert, "T", {"k", "ts", "v"},
                      {{Int64(1), expired_, Int64(10)},
                       {Int64(2), unexpired_, Int64(20)},
                       {Int64(3), NullTimestamp(), Int64(30)},
                       {Int64(4), barely_expired_, Int64(40)}});
  mutation.AddWriteOp(MutationOpType::kInsert, "C", {"k", "c"},
                      {{Int64(1), Int64(1)},
                       {Int64(1), Int64(2)},
                       {Int64(2), Int64(1)},
                       {Int64(4), Int64(1)}});
  GOOGLESQL_ASSERT_OK(Commit(database.get(), mutation));

  GOOGLESQL_ASSERT_OK(database->get_row_deletion_policy_sweeper()->SweepOnce());

  EXPECT_THAT(ReadInt64Column(database.get(), "T", "k"), ElementsAre(2, 3));
  EXPECT_THAT(ReadInt64Column(database.get(), "C", "k"), ElementsAre(2));
  EXPECT_THAT(ReadInt64Column(database.get(), "T", "v", "TByV"),
              ElementsAre(20, 30));
}

TEST_F(RowDeletionPolicySweeperTest, DeletesRowsOfEveryTableWithAPolicy) {
  GOOGLESQL_ASSERT_OK_AND_ASSIGN(
      std::unique_ptr<Database> database,
      CreateDatabase({R"(
        CREATE TABLE Descending (
          k INT64 NOT NULL,
          ts TIMESTAMP,
        ) PRIMARY KEY (k DESC),
          ROW DELETION POLICY (OLDER_THAN(ts, INTERVAL 1 DAY)))",
                      R"(
        CREATE TABLE NoPolicy (
          k INT64 NOT NULL,
          ts TIMESTAMP,
        ) PRIMARY KEY (k))"}));
  Mutation mutation;
  mutation.AddWriteOp(
      MutationOpType::kInsert, "Descending", {"k", "ts"},
      {{Int64(1), expired_}, {Int64(2), unexpired_}, {Int64(3), expired_}});
  mutation.AddWriteOp(MutationOpType::kInsert, "NoPolicy", {"k", "ts"},
                      {{Int64(1), expired_}});
  GOOGLESQL_ASSERT_OK(Commit(database.get(), mutation));

  GOOGLESQL_ASSERT_OK(database->get_row_deletion_policy_sweeper()->SweepOnce());

  EXPECT_THAT(ReadInt64Column(database.get(), "Descending", "k"),
              ElementsAre(2));
  EXPECT_THAT(ReadInt64Column(database.get(), "NoPolicy", "k"),
              ElementsAre(1));
}

TEST_F(RowDeletionPolicySweeperTest, RecordsSweepsForSpannerSys) {
  GOOGLESQL_ASSERT_OK_AND_ASSIGN(
      std::unique_ptr<Database> database,
      CreateDatabase({R"(
        CREATE TABLE T (
          k INT64 NOT NULL,
          ts TIMESTAMP,
        ) PRIMARY KEY (k),
          ROW DELETION POLICY (OLDER_THAN(ts, INTERVAL 1 DAY)))",
                      R"(
        CREATE TABLE NoPolicy (
          k INT64 NOT NULL,
        ) PRIMARY KEY (k))"}));
  auto snapshot = [&]() {
    return database->stats_collector()->Snapshot(
        "ROW_DELETION_POLICIES", absl::Now(),
        /*include_open_intervals=*/false, database->GetLatestSchema());
  };
  // A policy that was not swept yet has no watermark.
  std::vector<SpannerSysRow> rows = snapshot();
  ASSERT_EQ(rows.size(), 1);
  EXPECT_EQ(rows[0].at("TABLE_NAME").string_value(), "T");
  EXPECT_FALSE(rows[0].contains("PROCESSED_WATERMARK"));
  EXPECT_EQ(rows[0].at("UNDELETABLE_ROWS").int64_value(), 0);

  Mutation mutation;
  mutation.AddWriteOp(MutationOpType::kInsert, "T", {"k", "ts"},
                      {{Int64(1), expired_}, {Int64(2), unexpired_}});
  GOOGLESQL_ASSERT_OK(Commit(database.get(), mutation));
  const absl::Time before_sweep = absl::Now();
  GOOGLESQL_ASSERT_OK(database->get_row_deletion_policy_sweeper()->SweepOnce());

  rows = snapshot();
  ASSERT_EQ(rows.size(), 1);
  EXPECT_GE(rows[0].at("PROCESSED_WATERMARK").ToTime(), before_sweep);
  EXPECT_EQ(rows[0].at("UNDELETABLE_ROWS").int64_value(), 0);
  EXPECT_FALSE(rows[0].contains("MIN_UNDELETABLE_TIMESTAMP"));
}

TEST_F(RowDeletionPolicySweeperTest, DeletesRowsExpiredByPostgresTtl) {
  GOOGLESQL_ASSERT_OK_AND_ASSIGN(
      std::unique_ptr<Database> database,
      CreateDatabase({R"(
        CREATE TABLE t (
          k bigint NOT NULL PRIMARY KEY,
          ts timestamptz
        ) TTL INTERVAL '1 day' ON ts)"},
                     database_api::DatabaseDialect::POSTGRESQL));
  Mutation mutation;
  mutation.AddWriteOp(
      MutationOpType::kInsert, "t", {"k", "ts"},
      {{Int64(1), expired_}, {Int64(2), unexpired_}, {Int64(3), NullTimestamp()}});
  GOOGLESQL_ASSERT_OK(Commit(database.get(), mutation));

  GOOGLESQL_ASSERT_OK(database->get_row_deletion_policy_sweeper()->SweepOnce());

  EXPECT_THAT(ReadInt64Column(database.get(), "t", "k"), ElementsAre(2, 3));
}

TEST_F(RowDeletionPolicySweeperTest,
       ChangeStreamsRecordDeletesAsSystemTransactionUnlessExcluded) {
  GOOGLESQL_ASSERT_OK_AND_ASSIGN(
      std::unique_ptr<Database> database,
      CreateDatabase(
          {R"(
        CREATE TABLE T (
          k INT64 NOT NULL,
          ts TIMESTAMP,
        ) PRIMARY KEY (k),
          ROW DELETION POLICY (OLDER_THAN(ts, INTERVAL 1 DAY)))",
           "CREATE CHANGE STREAM AllDeletes FOR T",
           "CREATE CHANGE STREAM NoTtlDeletes FOR T "
           "OPTIONS (exclude_ttl_deletes = true)"}));
  Mutation insert;
  insert.AddWriteOp(MutationOpType::kInsert, "T", {"k", "ts"},
                    {{Int64(1), expired_},
                     {Int64(2), expired_},
                     {Int64(3), expired_},
                     {Int64(4), unexpired_}});
  GOOGLESQL_ASSERT_OK(Commit(database.get(), insert));

  RowDeletionPolicySweeper sweeper(
      absl::bind_front(&Database::CreateReadOnlyTransaction, database.get()),
      absl::bind_front(&Database::CreateReadWriteTransaction, database.get()),
      /*sweep_interval=*/absl::ZeroDuration(), /*batch_size=*/2);
  GOOGLESQL_ASSERT_OK(sweeper.SweepOnce());
  Mutation user_delete;
  user_delete.AddDeleteOp("T", KeySet(Key({Int64(4)})));
  GOOGLESQL_ASSERT_OK(Commit(database.get(), user_delete));

  EXPECT_THAT(ReadInt64Column(database.get(), "T", "k"), IsEmpty());
  absl::flat_hash_set<std::string> ttl_transaction_ids;
  EXPECT_THAT(
      ReadChangeStreamRecords(database.get(), "AllDeletes",
                              &ttl_transaction_ids),
      UnorderedElementsAre(UserRecord("T", "INSERT"), TtlDeleteRecord("T"),
                           TtlDeleteRecord("T"), UserRecord("T", "DELETE")));
  // The three expired rows were deleted in two batches, each its own
  // transaction.
  EXPECT_EQ(ttl_transaction_ids.size(), 2);
  EXPECT_THAT(ReadChangeStreamRecords(database.get(), "NoTtlDeletes"),
              UnorderedElementsAre(UserRecord("T", "INSERT"),
                                   UserRecord("T", "DELETE")));
}

TEST_F(RowDeletionPolicySweeperTest, KeepsRowWhoseTimestampChangedAfterScan) {
  GOOGLESQL_ASSERT_OK_AND_ASSIGN(
      std::unique_ptr<Database> database,
      CreateDatabase({R"(
        CREATE TABLE T (
          k INT64 NOT NULL,
          ts TIMESTAMP,
        ) PRIMARY KEY (k),
          ROW DELETION POLICY (OLDER_THAN(ts, INTERVAL 1 DAY)))"}));
  Mutation insert;
  insert.AddWriteOp(MutationOpType::kInsert, "T", {"k", "ts"},
                    {{Int64(1), expired_}, {Int64(2), expired_}});
  GOOGLESQL_ASSERT_OK(Commit(database.get(), insert));

  // Refresh row 1's timestamp after the sweep's scan found it expired, before
  // the transaction that deletes it starts.
  RowDeletionPolicySweeper sweeper(
      absl::bind_front(&Database::CreateReadOnlyTransaction, database.get()),
      [&](const ReadWriteOptions& options, const RetryState& retry_state)
          -> absl::StatusOr<std::unique_ptr<ReadWriteTransaction>> {
        Mutation refresh;
        refresh.AddWriteOp(MutationOpType::kUpdate, "T", {"k", "ts"},
                           {{Int64(1), unexpired_}});
        GOOGLESQL_RETURN_IF_ERROR(Commit(database.get(), refresh));
        return database->CreateReadWriteTransaction(options, retry_state);
      },
      /*sweep_interval=*/absl::ZeroDuration(), /*batch_size=*/10);
  GOOGLESQL_ASSERT_OK(sweeper.SweepOnce());

  EXPECT_THAT(ReadInt64Column(database.get(), "T", "k"), ElementsAre(1));
}

TEST_F(RowDeletionPolicySweeperTest, RetriesAbortedBatch) {
  GOOGLESQL_ASSERT_OK_AND_ASSIGN(
      std::unique_ptr<Database> database,
      CreateDatabase({R"(
        CREATE TABLE T (
          k INT64 NOT NULL,
          ts TIMESTAMP,
        ) PRIMARY KEY (k),
          ROW DELETION POLICY (OLDER_THAN(ts, INTERVAL 1 DAY)))"}));
  Mutation insert;
  insert.AddWriteOp(MutationOpType::kInsert, "T", {"k", "ts"},
                    {{Int64(1), expired_}});
  GOOGLESQL_ASSERT_OK(Commit(database.get(), insert));

  std::vector<int> abort_retry_counts;
  RowDeletionPolicySweeper sweeper(
      absl::bind_front(&Database::CreateReadOnlyTransaction, database.get()),
      [&](const ReadWriteOptions& options, const RetryState& retry_state)
          -> absl::StatusOr<std::unique_ptr<ReadWriteTransaction>> {
        EXPECT_TRUE(options.row_deletion_policy_txn);
        abort_retry_counts.push_back(retry_state.abort_retry_count);
        if (abort_retry_counts.size() == 1) {
          return absl::AbortedError("Aborted by a concurrent transaction");
        }
        return database->CreateReadWriteTransaction(options, retry_state);
      },
      /*sweep_interval=*/absl::ZeroDuration(), /*batch_size=*/10);
  GOOGLESQL_ASSERT_OK(sweeper.SweepOnce());

  EXPECT_THAT(abort_retry_counts, ElementsAre(0, 1));
  EXPECT_THAT(ReadInt64Column(database.get(), "T", "k"), IsEmpty());
}

TEST_F(RowDeletionPolicySweeperTest,
       DoesNotAbortUserTransactionOnUnexpiredRows) {
  GOOGLESQL_ASSERT_OK_AND_ASSIGN(
      std::unique_ptr<Database> database,
      CreateDatabase({R"(
        CREATE TABLE T (
          k INT64 NOT NULL,
          ts TIMESTAMP,
        ) PRIMARY KEY (k),
          ROW DELETION POLICY (OLDER_THAN(ts, INTERVAL 1 DAY)))"}));
  Mutation insert;
  insert.AddWriteOp(MutationOpType::kInsert, "T", {"k", "ts"},
                    {{Int64(1), expired_}, {Int64(2), unexpired_}});
  GOOGLESQL_ASSERT_OK(Commit(database.get(), insert));
  GOOGLESQL_ASSERT_OK_AND_ASSIGN(
      std::unique_ptr<ReadWriteTransaction> user_txn,
      database->CreateReadWriteTransaction(ReadWriteOptions(), RetryState()));
  Mutation user_update;
  user_update.AddWriteOp(MutationOpType::kUpdate, "T", {"k", "ts"},
                         {{Int64(2), Timestamp(now_)}});
  GOOGLESQL_ASSERT_OK(user_txn->Write(user_update));

  GOOGLESQL_ASSERT_OK(database->get_row_deletion_policy_sweeper()->SweepOnce());
  GOOGLESQL_ASSERT_OK(user_txn->Commit());

  EXPECT_THAT(ReadInt64Column(database.get(), "T", "k"), ElementsAre(2));
}

TEST_F(RowDeletionPolicySweeperTest, BackgroundSweepStartsWhenPolicyIsAdded) {
  config::set_row_deletion_policy_sweep_interval_seconds(1);
  GOOGLESQL_ASSERT_OK_AND_ASSIGN(
      std::unique_ptr<Database> database,
      CreateDatabase({R"(
        CREATE TABLE T (
          k INT64 NOT NULL,
          ts TIMESTAMP,
        ) PRIMARY KEY (k))"}));
  Mutation insert;
  insert.AddWriteOp(MutationOpType::kInsert, "T", {"k", "ts"},
                    {{Int64(1), expired_}, {Int64(2), unexpired_}});
  GOOGLESQL_ASSERT_OK(Commit(database.get(), insert));

  GOOGLESQL_ASSERT_OK(UpdateSchema(
      database.get(), {"ALTER TABLE T ADD ROW DELETION POLICY "
                       "(OLDER_THAN(ts, INTERVAL 1 DAY))"}));

  const absl::Time deadline = absl::Now() + absl::Seconds(30);
  while (ReadInt64Column(database.get(), "T", "k").size() > 1 &&
         absl::Now() < deadline) {
    absl::SleepFor(absl::Milliseconds(100));
  }
  EXPECT_THAT(ReadInt64Column(database.get(), "T", "k"), ElementsAre(2));
}

}  // namespace
}  // namespace backend
}  // namespace emulator
}  // namespace spanner
}  // namespace google

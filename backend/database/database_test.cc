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

#include "backend/database/database.h"

#include <memory>
#include <string>
#include <utility>
#include <vector>

#include "absl/status/status.h"
#include "backend/access/read.h"
#include "backend/datamodel/key_set.h"
#include "backend/schema/catalog/change_stream.h"
#include "backend/schema/catalog/schema.h"
#include "backend/schema/updater/schema_updater.h"
#include "backend/transaction/options.h"
#include "common/clock.h"
#include "common/config.h"
#include "common/errors.h"
#include "gmock/gmock.h"
#include "googlesql/base/testing/status_matchers.h"
#include "gtest/gtest.h"
#include "tests/common/proto_matchers.h"
#include "tests/common/test.pb.h"

namespace google {
namespace spanner {
namespace emulator {
namespace backend {
namespace {

using googlesql::values::Int64;
using googlesql_base::testing::StatusIs;

constexpr char kDatabaseId[] = "test-db";

class DatabaseTest : public ::testing::Test {
 public:
  DatabaseTest() = default;

  ReadArg read_column(std::string table_name, std::string column_name) {
    ReadArg args;
    args.table = table_name;
    args.key_set = KeySet::All();
    args.columns = std::vector<std::string>{column_name};
    return args;
  }

 protected:
  Clock clock_;
};

TEST_F(DatabaseTest, CreateSuccessful) {
  GOOGLESQL_ASSERT_OK_AND_ASSIGN(
      std::unique_ptr<Database> database,
      Database::Create(&clock_, kDatabaseId, SchemaChangeOperation{}));
  // Verifies that by default, a GoogleSQL database is created.
  EXPECT_EQ(database->dialect(),
            database_api::DatabaseDialect::GOOGLE_STANDARD_SQL);

  std::vector<std::string> create_statements = {R"(
    CREATE TABLE T(
      k1 INT64,
      k2 INT64,
    ) PRIMARY KEY(k1)
  )",
                                                R"(
    CREATE INDEX I on T(k1))"};

  GOOGLESQL_ASSERT_OK_AND_ASSIGN(
      database,
      Database::Create(&clock_, kDatabaseId,
                       SchemaChangeOperation{.statements = create_statements}));
  // Verifies that by default, a GoogleSQL database is created.
  EXPECT_EQ(database->dialect(),
            database_api::DatabaseDialect::GOOGLE_STANDARD_SQL);
}

TEST_F(DatabaseTest, PersistentStorageDirectoryUsesStorageNamespace) {
  GOOGLESQL_ASSERT_OK_AND_ASSIGN(
      const std::string legacy,
      Database::PersistentStorageDirectory("/tmp/data", kDatabaseId));
  EXPECT_EQ(legacy, "/tmp/data/test-db/storage");
  GOOGLESQL_ASSERT_OK_AND_ASSIGN(
      const std::string scoped,
      Database::PersistentStorageDirectory(
          "/tmp/data", "projects/p/instances/i/databases/test-db"));
  EXPECT_EQ(scoped, "/tmp/data/projects/p/instances/i/databases/test-db/storage");
  EXPECT_THAT(
      Database::PersistentStorageDirectory("/tmp/data", "../backups/x"),
      StatusIs(absl::StatusCode::kInvalidArgument));
  EXPECT_THAT(
      Database::PersistentStorageDirectory(
          "/tmp/data", "projects/p/instances/i/databases/db/../../backups/x"),
      StatusIs(absl::StatusCode::kInvalidArgument));
}

TEST_F(DatabaseTest, PersistedIdCountersAdvanceAfterSchemaReplay) {
  const std::vector<std::string> statements = {R"(
    CREATE TABLE T(
      k1 INT64,
      k2 STRING(MAX),
    ) PRIMARY KEY(k1)
  )"};
  GOOGLESQL_ASSERT_OK_AND_ASSIGN(
      std::unique_ptr<Database> baseline,
      Database::Create(&clock_, kDatabaseId,
                       SchemaChangeOperation{.statements = statements}));
  GOOGLESQL_ASSERT_OK_AND_ASSIGN(
      std::unique_ptr<Database> restored,
      Database::Create(&clock_, kDatabaseId,
                       SchemaChangeOperation{.statements = statements},
                       Database::IdCounterValues{
                           .table_id = 7,
                           .column_id = 11,
                           .change_stream_id = 13,
                       }));

  ASSERT_NE(baseline->GetLatestSchema()->FindTable("T"), nullptr);
  ASSERT_NE(restored->GetLatestSchema()->FindTable("T"), nullptr);
  EXPECT_EQ(restored->GetLatestSchema()->FindTable("T")->id(),
            baseline->GetLatestSchema()->FindTable("T")->id());
  EXPECT_EQ(restored->GetIdCounterValues().table_id, 7);
  EXPECT_EQ(restored->GetIdCounterValues().column_id, 11);
  EXPECT_EQ(restored->GetIdCounterValues().change_stream_id, 13);
}

TEST_F(DatabaseTest, ReplaysCommittedSchemaBatchesInOrder) {
  std::vector<std::vector<std::string>> batches = {
      {"CREATE TABLE T (K INT64) PRIMARY KEY (K)"},
      {"DROP TABLE T"},
      {"CREATE TABLE T (K INT64, V STRING(MAX)) PRIMARY KEY (K)"},
  };
  std::vector<SchemaChangeOperation> operations;
  operations.reserve(batches.size());
  for (const auto& batch : batches) {
    operations.push_back({.statements = batch});
  }

  GOOGLESQL_ASSERT_OK_AND_ASSIGN(
      std::unique_ptr<Database> baseline,
      Database::Create(&clock_, kDatabaseId, operations.front()));
  for (int i = 1; i < operations.size(); ++i) {
    int completed_statements = 0;
    absl::Time commit_timestamp;
    absl::Status backfill_status;
    GOOGLESQL_EXPECT_OK(baseline->UpdateSchema(
        operations[i], &completed_statements, &commit_timestamp,
        &backfill_status));
  }

  GOOGLESQL_ASSERT_OK_AND_ASSIGN(
      std::unique_ptr<Database> restored,
      Database::Create(&clock_, kDatabaseId, operations,
                       Database::IdCounterValues{}, ""));
  const auto* baseline_table = baseline->GetLatestSchema()->FindTable("T");
  const auto* restored_table = restored->GetLatestSchema()->FindTable("T");
  ASSERT_NE(baseline_table, nullptr);
  ASSERT_NE(restored_table, nullptr);
  EXPECT_NE(restored_table->FindColumn("V"), nullptr);
  EXPECT_EQ(restored_table->id(), baseline_table->id());
  EXPECT_EQ(restored->GetIdCounterValues().table_id,
            baseline->GetIdCounterValues().table_id);
  EXPECT_EQ(restored->GetIdCounterValues().column_id,
            baseline->GetIdCounterValues().column_id);
}

TEST_F(DatabaseTest, ReplaysEachProtoDescriptorBatchWithItsOwnBundle) {
  google::protobuf::FileDescriptorSet descriptor_set;
  ::emulator::tests::common::Simple::descriptor()->file()->CopyTo(
      descriptor_set.add_file());
  const std::string descriptor_bytes = descriptor_set.SerializeAsString();
  const std::vector<std::vector<std::string>> statement_batches = {
      {"CREATE TABLE T (K INT64) PRIMARY KEY (K)"},
      {
          "CREATE PROTO BUNDLE (emulator.tests.common.Simple)",
          "ALTER TABLE T ADD COLUMN ProtoValue "
          "emulator.tests.common.Simple",
      },
  };
  const std::vector<SchemaChangeOperation> operations = {
      {.statements = statement_batches[0]},
      {.statements = statement_batches[1],
       .proto_descriptor_bytes = descriptor_bytes},
  };
  GOOGLESQL_ASSERT_OK_AND_ASSIGN(
      std::unique_ptr<Database> restored,
      Database::Create(&clock_, kDatabaseId, operations,
                       Database::IdCounterValues{}, ""));

  const auto* table = restored->GetLatestSchema()->FindTable("T");
  ASSERT_NE(table, nullptr);
  EXPECT_NE(table->FindColumn("ProtoValue"), nullptr);
  GOOGLESQL_ASSERT_OK_AND_ASSIGN(
      const std::string restored_descriptors,
      restored->GetLatestSchema()->proto_bundle()->GetProtoDescriptorBytes());
  EXPECT_EQ(restored_descriptors, descriptor_bytes);
}

TEST_F(DatabaseTest, CreateWithGSQLDialectSuccessful) {
  GOOGLESQL_ASSERT_OK_AND_ASSIGN(
      std::unique_ptr<Database> database,
      Database::Create(
          &clock_, kDatabaseId,
          SchemaChangeOperation{
              .database_dialect =
                  database_api::DatabaseDialect::GOOGLE_STANDARD_SQL}));
  EXPECT_EQ(database->dialect(),
            database_api::DatabaseDialect::GOOGLE_STANDARD_SQL);

  std::vector<std::string> create_statements = {R"(
    CREATE TABLE T(
      k1 INT64,
      k2 INT64,
    ) PRIMARY KEY(k1)
  )",
                                                R"(
    CREATE INDEX I on T(k1))"};

  GOOGLESQL_ASSERT_OK_AND_ASSIGN(
      database,
      Database::Create(
          &clock_, kDatabaseId,
          SchemaChangeOperation{
              .statements = create_statements,
              .database_dialect =
                  database_api::DatabaseDialect::GOOGLE_STANDARD_SQL}));
  EXPECT_EQ(database->dialect(),
            database_api::DatabaseDialect::GOOGLE_STANDARD_SQL);
}

TEST_F(DatabaseTest, CreateWithPostgresDialectSuccessful) {
  GOOGLESQL_ASSERT_OK_AND_ASSIGN(
      std::unique_ptr<Database> database,
      Database::Create(
          &clock_, kDatabaseId,
          SchemaChangeOperation{
              .database_dialect = database_api::DatabaseDialect::POSTGRESQL}));
  EXPECT_EQ(database->dialect(), database_api::DatabaseDialect::POSTGRESQL);

  std::vector<std::string> create_statements = {R"(
    CREATE TABLE T(
      k1 bigint primary key,
      k2 bigint
    )
  )",
                                                R"(
    CREATE INDEX I on T(k1))"};

  GOOGLESQL_ASSERT_OK_AND_ASSIGN(
      database,
      Database::Create(
          &clock_, kDatabaseId,
          SchemaChangeOperation{
              .statements = create_statements,
              .database_dialect = database_api::DatabaseDialect::POSTGRESQL}));
  EXPECT_EQ(database->dialect(), database_api::DatabaseDialect::POSTGRESQL);
}

TEST_F(DatabaseTest, UpdateSchemaSuccessful) {
  GOOGLESQL_ASSERT_OK_AND_ASSIGN(
      auto db, Database::Create(&clock_, kDatabaseId, SchemaChangeOperation{}));

  std::vector<std::string> update_statements = {R"(
    CREATE TABLE T(
      k1 INT64,
      k2 INT64,
    ) PRIMARY KEY(k1)
  )",
                                                R"(
    CREATE INDEX I on T(k1)
  )"};

  absl::Status backfill_status;
  int completed_statements;
  absl::Time commit_ts;
  GOOGLESQL_EXPECT_OK(
      db->UpdateSchema(SchemaChangeOperation{.statements = update_statements},
                       &completed_statements, &commit_ts, &backfill_status));
  GOOGLESQL_EXPECT_OK(backfill_status);
}

TEST_F(DatabaseTest, UpdateSchemaPartialSuccess) {
  std::vector<std::string> create_statements = {R"(
    CREATE TABLE T(
      k1 INT64,
      k2 INT64,
    ) PRIMARY KEY(k1)
  )"};
  GOOGLESQL_ASSERT_OK_AND_ASSIGN(
      auto db,
      Database::Create(&clock_, kDatabaseId,
                       SchemaChangeOperation{.statements = create_statements}));

  GOOGLESQL_ASSERT_OK_AND_ASSIGN(
      std::unique_ptr<ReadWriteTransaction> txn,
      db->CreateReadWriteTransaction(ReadWriteOptions(), RetryState()));

  Mutation m;
  m.AddWriteOp(MutationOpType::kInsert, "T", {"k1", "k2"},
               {{Int64(1), Int64(2)}});
  m.AddWriteOp(MutationOpType::kInsert, "T", {"k1", "k2"},
               {{Int64(2), Int64(2)}});
  m.AddWriteOp(MutationOpType::kInsert, "T", {"k1", "k2"},
               {{Int64(3), Int64(2)}});
  GOOGLESQL_ASSERT_OK(txn->Write(m));
  GOOGLESQL_ASSERT_OK(txn->Commit());

  std::vector<std::string> update_statements = {R"(
    CREATE TABLE IF NOT EXISTS T(
      ignored INT64,
    ) PRIMARY KEY(ignored)
  )",
                                                R"(
    CREATE TABLE T1(
      a INT64,
    ) PRIMARY KEY(a)
  )",
                                                R"(
    CREATE UNIQUE INDEX Idx on T(k2)
  )",
                                                R"(
    CREATE TABLE T2(
      b INT64,
    ) PRIMARY KEY(b)
  )"};

  absl::Status backfill_status;
  int completed_statements;
  absl::Time commit_ts;

  // The statements are semantically valid, indicated by an OK return status.
  GOOGLESQL_EXPECT_OK(
      db->UpdateSchema(SchemaChangeOperation{.statements = update_statements},
                       &completed_statements, &commit_ts, &backfill_status));

  // But the backfill statements fail.
  EXPECT_EQ(backfill_status,
            error::UniqueIndexViolationOnIndexCreation("Idx", "{Int64(2)}"));

  // The no-op and the following schema mutation form the committed prefix.
  EXPECT_EQ(completed_statements, 2);
}

TEST_F(DatabaseTest, ConcurrentSchemaChangeIsAborted) {
  auto current_probability = config::abort_current_transaction_probability();
  config::set_abort_current_transaction_probability(0);

  std::vector<std::string> create_statements = {R"(
    CREATE TABLE T(
      k1 INT64,
      k2 INT64,
    ) PRIMARY KEY(k1)
  )"};
  GOOGLESQL_ASSERT_OK_AND_ASSIGN(
      auto db,
      Database::Create(&clock_, kDatabaseId,
                       SchemaChangeOperation{.statements = create_statements}));

  // Initiate a Read inside a read-write transaction to acquire locks.
  std::unique_ptr<RowCursor> row_cursor;
  GOOGLESQL_ASSERT_OK_AND_ASSIGN(
      std::unique_ptr<ReadWriteTransaction> txn,
      db->CreateReadWriteTransaction(ReadWriteOptions(), RetryState()));
  GOOGLESQL_EXPECT_OK(txn->Read(read_column("T", "k1"), &row_cursor));

  std::vector<std::string> update_statements = {R"(
    CREATE TABLE T(
      k1 INT64,
      k2 INT64,
    ) PRIMARY KEY(k1)
  )"};
  absl::Status backfill_status;
  int completed_statements;
  absl::Time commit_ts;
  EXPECT_EQ(
      db->UpdateSchema(SchemaChangeOperation{.statements = update_statements},
                       &completed_statements, &commit_ts, &backfill_status),
      error::ConcurrentSchemaChangeOrReadWriteTxnInProgress());

  config::set_abort_current_transaction_probability(current_probability);
}

TEST_F(DatabaseTest, SchemaChangeLocksSuccesfullyReleased) {
  std::vector<std::string> create_statements = {R"(
    CREATE TABLE T(
      k1 INT64,
      k2 INT64,
    ) PRIMARY KEY(k1)
  )"};
  GOOGLESQL_ASSERT_OK_AND_ASSIGN(
      auto db,
      Database::Create(&clock_, kDatabaseId,
                       SchemaChangeOperation{.statements = create_statements}));

  // Schema update will fail.
  std::vector<std::string> update_statements = {R"(
    CREATE TABLE T(
      k1 INT64,
      k2 INT64,
    ) PRIMARY KEY(k1)
  )"};
  absl::Status backfill_status;
  int completed_statements;
  absl::Time commit_ts;
  EXPECT_FALSE(
      db->UpdateSchema(SchemaChangeOperation{.statements = update_statements},
                       &completed_statements, &commit_ts, &backfill_status)
          .ok());

  // Can still run transactions as locks would have been released.
  std::unique_ptr<RowCursor> row_cursor;
  GOOGLESQL_ASSERT_OK_AND_ASSIGN(
      std::unique_ptr<ReadWriteTransaction> txn,

      db->CreateReadWriteTransaction(ReadWriteOptions(), RetryState()));
  GOOGLESQL_EXPECT_OK(txn->Read(read_column("T", "k1"), &row_cursor));
  GOOGLESQL_EXPECT_OK(txn->Commit());
}
TEST_F(DatabaseTest, RecoveryGateRejectsTransactionsCreatedBeforeQuarantine) {
  std::vector<std::string> create_statements = {R"(
    CREATE TABLE T(
      k1 INT64,
    ) PRIMARY KEY(k1)
  )"};
  GOOGLESQL_ASSERT_OK_AND_ASSIGN(
      auto db,
      Database::Create(&clock_, kDatabaseId,
                       SchemaChangeOperation{.statements = create_statements}));
  GOOGLESQL_ASSERT_OK_AND_ASSIGN(
      std::unique_ptr<ReadWriteTransaction> read_write,
      db->CreateReadWriteTransaction(ReadWriteOptions(), RetryState()));
  GOOGLESQL_ASSERT_OK_AND_ASSIGN(
      std::unique_ptr<ReadOnlyTransaction> read_only,
      db->CreateReadOnlyTransaction(ReadOnlyOptions()));

  db->MarkRestoreRequired();

  Mutation mutation;
  mutation.AddWriteOp(MutationOpType::kInsert, "T", {"k1"}, {{Int64(1)}});
  EXPECT_THAT(read_write->Write(mutation),
              StatusIs(absl::StatusCode::kFailedPrecondition));
  EXPECT_THAT(read_write->Commit(),
              StatusIs(absl::StatusCode::kFailedPrecondition));
  std::unique_ptr<RowCursor> cursor;
  EXPECT_THAT(read_only->Read(read_column("T", "k1"), &cursor),
              StatusIs(absl::StatusCode::kFailedPrecondition));
  EXPECT_THAT(
      db->CreateReadWriteTransaction(ReadWriteOptions(), RetryState()),
      StatusIs(absl::StatusCode::kFailedPrecondition));
  GOOGLESQL_EXPECT_OK(read_write->Rollback());
}

TEST_F(DatabaseTest, ChangeStreamCreationTimePersistsAndMatchesCreateTime) {
  const absl::Time create_start_time = clock_.Now();
  const std::vector<std::string> create_statements = {
      "CREATE TABLE T(k1 INT64, c1 STRING(MAX)) PRIMARY KEY(k1)",
      "CREATE CHANGE STREAM CS FOR ALL",
  };
  GOOGLESQL_ASSERT_OK_AND_ASSIGN(
      std::unique_ptr<Database> baseline,
      Database::Create(&clock_, kDatabaseId,
                       SchemaChangeOperation{.statements = create_statements}));

  const auto* baseline_cs =
      baseline->GetLatestSchema()->FindChangeStream("CS");
  ASSERT_NE(baseline_cs, nullptr);
  EXPECT_GE(baseline_cs->creation_time(), create_start_time);
  EXPECT_LE(baseline_cs->creation_time(), clock_.Now());

  // Simulate restore replaying the batch at the exact schema_change_timestamp
  const absl::Time original_ts = baseline_cs->creation_time();
  std::vector<SchemaChangeOperation> operations = {
      {.statements = create_statements,
       .schema_change_timestamp = original_ts},
  };
  GOOGLESQL_ASSERT_OK_AND_ASSIGN(
      std::unique_ptr<Database> restored,
      Database::Create(&clock_, kDatabaseId, operations,
                       baseline->GetIdCounterValues(), ""));

  const auto* restored_cs =
      restored->GetLatestSchema()->FindChangeStream("CS");
  ASSERT_NE(restored_cs, nullptr);
  EXPECT_EQ(restored_cs->creation_time(), original_ts);
}

}  // namespace
}  // namespace backend
}  // namespace emulator
}  // namespace spanner
}  // namespace google

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

#include <algorithm>
#include <chrono>
#include <filesystem>
#include <memory>
#include <string>
#include <thread>
#include <utility>
#include <vector>

#include "absl/container/flat_hash_map.h"
#include "absl/container/flat_hash_set.h"
#include "absl/flags/declare.h"
#include "absl/flags/flag.h"
#include "absl/random/random.h"
#include "absl/status/status.h"
#include "absl/strings/str_cat.h"
#include "backend/access/read.h"
#include "backend/datamodel/key.h"
#include "backend/datamodel/key_set.h"
#include "backend/query/query_context.h"
#include "backend/query/query_engine.h"
#include "backend/schema/catalog/change_stream.h"
#include "backend/schema/catalog/schema.h"
#include "backend/schema/updater/schema_updater.h"
#include "backend/stats/operation_stats.h"
#include "backend/stats/system_stats_collector.h"
#include "backend/transaction/options.h"
#include "common/clock.h"
#include "common/config.h"
#include "common/errors.h"
#include "gmock/gmock.h"
#include "googlesql/public/uuid_value.h"
#include "googlesql/public/value.h"
#include "googlesql/base/testing/status_matchers.h"
#include "gtest/gtest.h"
#include "tests/common/proto_matchers.h"
#include "tests/common/test.pb.h"

ABSL_DECLARE_FLAG(std::string, data_dir);

namespace google {
namespace spanner {
namespace emulator {
namespace backend {
namespace {

using googlesql::values::Int64;
using googlesql_base::testing::StatusIs;

constexpr char kDatabaseId[] = "test-db";

class ScopedDatabaseDataDir {
 public:
  ScopedDatabaseDataDir()
      : previous_(absl::GetFlag(FLAGS_data_dir)),
        path_((std::filesystem::temp_directory_path() /
               absl::StrCat(
                   "spanner-uuid-persist-",
                   std::chrono::steady_clock::now().time_since_epoch().count()))
                  .string()) {
    std::filesystem::create_directories(path_);
    absl::SetFlag(&FLAGS_data_dir, path_);
  }

  ~ScopedDatabaseDataDir() {
    absl::SetFlag(&FLAGS_data_dir, previous_);
    std::error_code ignored;
    std::filesystem::remove_all(path_, ignored);
  }

 private:
  std::string previous_;
  std::string path_;
};

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

TEST_F(DatabaseTest, UserSplitPointsPersistAfterReopen) {
  ScopedDatabaseDataDir data_dir;
  const std::vector<std::string> statements = {
      "CREATE TABLE T (k INT64 NOT NULL) PRIMARY KEY (k)"};
  const SchemaChangeOperation schema_operation{.statements = statements};
  const absl::Time expire_time = absl::Now() + absl::Hours(1);
  GOOGLESQL_ASSERT_OK_AND_ASSIGN(
      std::unique_ptr<Database> database,
      Database::Create(&clock_, kDatabaseId, schema_operation));
  GOOGLESQL_ASSERT_OK(database->AddSplitPoints({{.table_name = "T",
                                       .initiator = "load",
                                       .split_key = "T(10)",
                                       .expire_time = expire_time}}));
  const Database::IdCounterValues counters = database->GetIdCounterValues();
  database.reset();  // Release LevelDB's lock before reopening the same data_dir.

  GOOGLESQL_ASSERT_OK_AND_ASSIGN(
      std::unique_ptr<Database> reopened,
      Database::Create(&clock_, kDatabaseId, schema_operation, counters));
  std::vector<SpannerSysRow> rows = reopened->stats_collector()->Snapshot(
      "USER_SPLIT_POINTS", absl::Now(), /*include_open_intervals=*/false,
      reopened->GetLatestSchema());
  ASSERT_EQ(rows.size(), 1);
  EXPECT_EQ(rows[0].at("SPLIT_KEY").string_value(), "T(10)");
  EXPECT_EQ(rows[0].at("INITIATOR").string_value(), "load");
  EXPECT_EQ(rows[0].at("EXPIRE_TIME"),
            googlesql::values::Timestamp(expire_time));
}

TEST_F(DatabaseTest, SamplesTableAndIndexSizes) {
  std::vector<std::string> statements = {
      "CREATE TABLE T (k INT64 NOT NULL, s STRING(MAX)) PRIMARY KEY (k)",
      "CREATE INDEX TByS ON T(s)"};
  GOOGLESQL_ASSERT_OK_AND_ASSIGN(
      std::unique_ptr<Database> database,
      Database::Create(&clock_, kDatabaseId,
                       SchemaChangeOperation{.statements = statements}));
  GOOGLESQL_ASSERT_OK_AND_ASSIGN(
      std::unique_ptr<ReadWriteTransaction> writer,
      database->CreateReadWriteTransaction(ReadWriteOptions(), RetryState()));
  Mutation mutation;
  mutation.AddWriteOp(MutationOpType::kInsert, "T", {"k", "s"},
                      {{Int64(1), googlesql::values::String("abcd")}});
  GOOGLESQL_ASSERT_OK(writer->Write(mutation));
  GOOGLESQL_ASSERT_OK(writer->Commit());

  GOOGLESQL_ASSERT_OK(database->table_size_sampler()->SampleOnce());

  // The open hour holds the sample taken at creation, when the table was
  // empty, and the one above: an 8-byte key and a 4-byte string, averaged.
  std::vector<SpannerSysRow> rows = database->stats_collector()->Snapshot(
      "TABLE_SIZES_STATS_1HOUR", absl::Now(), /*include_open_intervals=*/true,
      database->GetLatestSchema());
  absl::flat_hash_map<std::string, double> used_bytes;
  for (const SpannerSysRow& row : rows) {
    double& table_bytes = used_bytes[row.at("TABLE_NAME").string_value()];
    table_bytes = std::max(table_bytes, row.at("USED_BYTES").double_value());
  }
  EXPECT_GT(used_bytes["T"], 0);
  EXPECT_LE(used_bytes["T"], 12);
  EXPECT_GT(used_bytes["TByS"], 0);
}

// Checks that the emails of the unique index EmployeesByEmail are unique and
// index every row of Employees.
void ExpectUniqueEmails(Database* database) {
  GOOGLESQL_ASSERT_OK_AND_ASSIGN(
      std::unique_ptr<ReadOnlyTransaction> reader,
      database->CreateReadOnlyTransaction(ReadOnlyOptions()));
  auto count_rows = [&](const std::string& index) -> int {
    ReadArg read_arg;
    read_arg.table = "Employees";
    read_arg.index = index;
    read_arg.key_set = KeySet::All();
    read_arg.columns = {"Email"};
    std::unique_ptr<RowCursor> cursor;
    EXPECT_TRUE(reader->Read(read_arg, &cursor).ok());
    absl::flat_hash_set<std::string> emails;
    int rows = 0;
    while (cursor->Next()) {
      ++rows;
      if (!index.empty()) {
        EXPECT_TRUE(emails.insert(cursor->ColumnValue(0).string_value()).second)
            << "duplicate email " << cursor->ColumnValue(0);
      }
    }
    EXPECT_TRUE(cursor->Status().ok()) << cursor->Status();
    return rows;
  };
  const int table_rows = count_rows(/*index=*/"");
  EXPECT_GT(table_rows, 0);
  EXPECT_EQ(count_rows("EmployeesByEmail"), table_rows);
}

// Runs threads that insert rows of Employees with emails from a small pool, so
// that many collide, and change the email of every fifth row. Like a client, a
// thread retries an aborted transaction and picks a new email after a unique
// index violation. The threads with an odd number use repeatable read.
void WriteEmployeesConcurrently(Database* database) {
  constexpr int kThreads = 8;
  constexpr int kWritesPerThread = 30;
  constexpr int kEmails = 60;
  std::vector<std::thread> threads;
  for (int thread = 0; thread < kThreads; ++thread) {
    threads.emplace_back([database, thread]() {
      absl::BitGen gen;
      ReadWriteOptions options;
      options.repeatable_read = thread % 2 == 1;
      for (int write = 0; write < kWritesPerThread; ++write) {
        const bool update = write % 5 == 4;
        const int64_t id =
            thread * kWritesPerThread + (update ? write - 1 : write);
        RetryState retry_state;
        for (int attempt = 0; attempt < 200; ++attempt) {
          const std::string email = absl::StrCat(
              "user", absl::Uniform(gen, 0, kEmails), "@example.com");
          Mutation mutation;
          mutation.AddWriteOp(
              update ? MutationOpType::kUpdate : MutationOpType::kInsert,
              "Employees", {"Id", "Email"},
              {{Int64(id), googlesql::values::String(email)}});
          absl::StatusOr<std::unique_ptr<ReadWriteTransaction>> txn =
              database->CreateReadWriteTransaction(options, retry_state);
          absl::Status status = txn.status();
          if (status.ok()) status = (*txn)->Write(mutation);
          if (status.ok()) status = (*txn)->Commit();
          if (status.ok() || absl::IsNotFound(status)) {
            // An update of a row that was never inserted is not retried.
            break;
          }
          if (absl::IsAborted(status)) {
            ++retry_state.abort_retry_count;
          } else if (!absl::IsAlreadyExists(status)) {
            ADD_FAILURE() << status;
            return;
          }
        }
      }
    });
  }
  for (std::thread& thread : threads) {
    thread.join();
  }
}

constexpr char kCreateEmployees[] =
    "CREATE TABLE Employees (Id INT64 NOT NULL, Email STRING(MAX)) "
    "PRIMARY KEY (Id)";
constexpr char kCreateEmployeesByEmail[] =
    "CREATE UNIQUE INDEX EmployeesByEmail ON Employees(Email)";

// Regression test for the unique index incident: concurrent writers of a
// unique index, some of them colliding, must never persist a duplicate key,
// and the database must restore from --data_dir afterwards. Persistent storage
// used to miss rows in range reads over keys of different lengths, so the
// unique index check did not see existing entries of the same value.
TEST_F(DatabaseTest, ConcurrentUniqueIndexWritersSurviveRestore) {
  ScopedDatabaseDataDir data_dir;
  const std::vector<std::string> statements = {kCreateEmployees,
                                               kCreateEmployeesByEmail};
  const SchemaChangeOperation schema_operation{.statements = statements};
  GOOGLESQL_ASSERT_OK_AND_ASSIGN(
      std::unique_ptr<Database> database,
      Database::Create(&clock_, kDatabaseId, schema_operation));
  WriteEmployeesConcurrently(database.get());
  ExpectUniqueEmails(database.get());

  const Database::IdCounterValues counters = database->GetIdCounterValues();
  database.reset();  // Release LevelDB's lock before reopening the same data_dir.
  GOOGLESQL_ASSERT_OK_AND_ASSIGN(
      std::unique_ptr<Database> reopened,
      Database::Create(&clock_, kDatabaseId, schema_operation, counters));
  ExpectUniqueEmails(reopened.get());
}

TEST_F(DatabaseTest, PostgreSqlUuidKeyAndValuePersistAfterReopen) {
  ScopedDatabaseDataDir data_dir;
  const std::vector<std::string> statements = {
      "CREATE TABLE uuid_values (id uuid PRIMARY KEY, payload uuid)"};
  const SchemaChangeOperation schema_operation{
      .statements = statements,
      .database_dialect = database_api::DatabaseDialect::POSTGRESQL};
  GOOGLESQL_ASSERT_OK_AND_ASSIGN(
      const googlesql::UuidValue id_uuid,
      googlesql::UuidValue::FromString(
          "9a31411b-caca-4ff1-86e9-39fbd2bc3f39"));
  GOOGLESQL_ASSERT_OK_AND_ASSIGN(
      const googlesql::UuidValue payload_uuid,
      googlesql::UuidValue::FromString(
          "12345678-1234-4abc-8def-123456789abc"));
  const googlesql::Value id = googlesql::Value::Uuid(id_uuid);
  const googlesql::Value payload = googlesql::Value::Uuid(payload_uuid);

  GOOGLESQL_ASSERT_OK_AND_ASSIGN(
      std::unique_ptr<Database> database,
      Database::Create(&clock_, kDatabaseId, schema_operation));
  GOOGLESQL_ASSERT_OK_AND_ASSIGN(
      std::unique_ptr<ReadWriteTransaction> writer,
      database->CreateReadWriteTransaction(ReadWriteOptions(), RetryState()));
  Mutation mutation;
  mutation.AddWriteOp(MutationOpType::kInsert, "uuid_values", {"id", "payload"},
                      {{id, payload}});
  GOOGLESQL_ASSERT_OK(writer->Write(mutation));
  GOOGLESQL_ASSERT_OK(writer->Commit());
  writer.reset();

  const Database::IdCounterValues counters = database->GetIdCounterValues();
  database.reset();  // Release LevelDB's lock before reopening the same data_dir.
  GOOGLESQL_ASSERT_OK_AND_ASSIGN(
      std::unique_ptr<Database> reopened,
      Database::Create(&clock_, kDatabaseId, schema_operation, counters));
  EXPECT_EQ(reopened->dialect(), database_api::DatabaseDialect::POSTGRESQL);
  GOOGLESQL_ASSERT_OK_AND_ASSIGN(
      std::unique_ptr<ReadOnlyTransaction> reader,
      reopened->CreateReadOnlyTransaction(ReadOnlyOptions()));

  ReadArg read_arg;
  read_arg.table = "uuid_values";
  read_arg.key_set = KeySet(Key({id}));
  read_arg.columns = {"id", "payload"};
  std::unique_ptr<RowCursor> rows;
  GOOGLESQL_ASSERT_OK(reader->Read(read_arg, &rows));
  ASSERT_TRUE(rows->Next());
  EXPECT_TRUE(rows->ColumnType(0)->IsUuid());
  EXPECT_TRUE(rows->ColumnType(1)->IsUuid());
  EXPECT_EQ(rows->ColumnValue(0), id);
  EXPECT_EQ(rows->ColumnValue(1), payload);
  EXPECT_FALSE(rows->Next());
  GOOGLESQL_EXPECT_OK(rows->Status());

  GOOGLESQL_ASSERT_OK_AND_ASSIGN(
      QueryResult result,
      reopened->query_engine()->ExecuteSql(
          Query{.sql = "SELECT id, payload FROM uuid_values WHERE id = "
                        "CAST('9a31411b-caca-4ff1-86e9-39fbd2bc3f39' AS "
                        "uuid)"},
          QueryContext{.schema = reopened->GetLatestSchema(),
                       .reader = reader.get(),
                       .is_read_only_txn = true}));
  ASSERT_NE(result.rows, nullptr);
  ASSERT_TRUE(result.rows->Next());
  EXPECT_TRUE(result.rows->ColumnType(0)->IsUuid());
  EXPECT_TRUE(result.rows->ColumnType(1)->IsUuid());
  EXPECT_EQ(result.rows->ColumnValue(0), id);
  EXPECT_EQ(result.rows->ColumnValue(1), payload);
  EXPECT_FALSE(result.rows->Next());
  GOOGLESQL_EXPECT_OK(result.rows->Status());
}

TEST_F(DatabaseTest, SpannerSysStatisticsPersistAcrossReopen) {
  ScopedDatabaseDataDir data_dir;
  const std::vector<std::string> statements = {
      "CREATE TABLE T(k INT64) PRIMARY KEY(k)"};
  const SchemaChangeOperation schema_operation{.statements = statements};
  QueryExecution query;
  query.text = "SELECT k FROM T";
  const absl::Time start = absl::Now() - absl::Minutes(30);
  auto minute_intervals = [](Database* database) {
    return database->stats_collector()
        ->Snapshot("QUERY_STATS_TOP_MINUTE", absl::Now(),
                   /*include_open_intervals=*/false, nullptr)
        .size();
  };

  GOOGLESQL_ASSERT_OK_AND_ASSIGN(
      std::unique_ptr<Database> database,
      Database::Create(&clock_, kDatabaseId, schema_operation));
  database->stats_collector()->RecordQuery(start, query, AccessFootprint());
  // A statement in a later minute ends the first statement's interval, which
  // saves the statistics.
  database->stats_collector()->RecordQuery(start + absl::Minutes(1), query,
                                           AccessFootprint());
  Database::IdCounterValues counters = database->GetIdCounterValues();
  database.reset();

  GOOGLESQL_ASSERT_OK_AND_ASSIGN(
      database,
      Database::Create(&clock_, kDatabaseId, schema_operation, counters));
  EXPECT_EQ(minute_intervals(database.get()), 1);

  // Persist() saves the statistics recorded since, as on shutdown.
  database->stats_collector()->RecordQuery(start + absl::Minutes(2), query,
                                           AccessFootprint());
  database->stats_collector()->Persist();
  counters = database->GetIdCounterValues();
  database.reset();
  GOOGLESQL_ASSERT_OK_AND_ASSIGN(
      database,
      Database::Create(&clock_, kDatabaseId, schema_operation, counters));
  EXPECT_EQ(minute_intervals(database.get()), 2);

  // Deleting the database deletes its statistics.
  database.reset();
  GOOGLESQL_ASSERT_OK(Database::DeletePersistentStorageDirectory(
      config::data_dir(), kDatabaseId));
  GOOGLESQL_ASSERT_OK_AND_ASSIGN(
      database, Database::Create(&clock_, kDatabaseId, schema_operation));
  EXPECT_EQ(minute_intervals(database.get()), 0);
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

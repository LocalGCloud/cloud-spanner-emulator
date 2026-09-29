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

#include "backend/transaction/read_write_transaction.h"

#include <cstdint>
#include <functional>
#include <memory>
#include <string>
#include <thread>  // NOLINT
#include <utility>
#include <vector>

#include "gmock/gmock.h"
#include "gtest/gtest.h"
#include "googlesql/base/testing/status_matchers.h"
#include "tests/common/proto_matchers.h"
#include "absl/container/flat_hash_set.h"
#include "absl/flags/declare.h"
#include "absl/flags/flag.h"
#include "absl/log/log.h"
#include "absl/status/status.h"
#include "absl/status/statusor.h"
#include "absl/strings/str_cat.h"
#include "absl/synchronization/barrier.h"
#include "absl/time/clock.h"
#include "absl/time/time.h"
#include "backend/access/write.h"
#include "backend/actions/manager.h"
#include "backend/actions/ops.h"
#include "backend/common/ids.h"
#include "backend/datamodel/key.h"
#include "backend/datamodel/key_range.h"
#include "backend/datamodel/key_set.h"
#include "backend/datamodel/value.h"
#include "backend/query/function_catalog.h"
#include "backend/schema/catalog/versioned_catalog.h"
#include "backend/storage/in_memory_storage.h"
#include "backend/transaction/actions.h"
#include "backend/transaction/options.h"
#include "common/clock.h"
#include "common/config.h"
#include "tests/common/schema_constructor.h"
#include "tests/common/scoped_feature_flags_setter.h"
#include "absl/status/status.h"

ABSL_DECLARE_FLAG(bool, enable_fault_injection);

namespace google {
namespace spanner {
namespace emulator {
namespace backend {
namespace {

using googlesql::values::Int64;
using googlesql::values::String;
using googlesql_base::testing::StatusIs;

class ReadWriteTransactionTest : public testing::Test {
 public:
  ReadWriteTransactionTest() = default;
  void SetUp() override {
    type_factory_ = std::make_unique<googlesql::TypeFactory>();
    lock_manager_ = std::make_unique<LockManager>(&clock_);
    storage_ = std::make_unique<InMemoryStorage>();
    versioned_catalog_ =
        std::make_unique<VersionedCatalog>(std::move(GetSchema()).value());
    action_manager_ = std::make_unique<ActionManager>();
    action_manager_->AddActionsForSchema(
        versioned_catalog_->GetSchema(absl::InfiniteFuture()),
        /*function_catalog=*/nullptr, type_factory_.get());
  }

  virtual absl::StatusOr<std::unique_ptr<const backend::Schema>> GetSchema() {
    return test::CreateSchemaFromDDL(
        {
            R"sql(
                  CREATE TABLE test_table (
                    int64_col INT64 NOT NULL,
                    string_col STRING(MAX),
                    int64_val_col INT64
                  ) PRIMARY KEY (int64_col)
                )sql",
            R"sql(
                  CREATE UNIQUE INDEX test_index ON test_table(string_col DESC)
                )sql"},
        type_factory_.get());
  }

 protected:
  Clock clock_;

  // The type factory must outlive the type objects that it has made.
  std::unique_ptr<googlesql::TypeFactory> type_factory_;

  // Internal state of database exposed for the purpose of testing.
  std::unique_ptr<LockManager> lock_manager_;
  std::unique_ptr<InMemoryStorage> storage_;
  std::unique_ptr<VersionedCatalog> versioned_catalog_;
  std::unique_ptr<ActionManager> action_manager_;

  // Counter to generate TransactionID.
  std::atomic<int> id_counter_ = 0;

  std::unique_ptr<ReadWriteTransaction> CreateReadWriteTransaction(
      ReadWriteOptions options = ReadWriteOptions()) {
    return std::make_unique<ReadWriteTransaction>(
        options, RetryState(), ++id_counter_, &clock_,
        storage_.get(), lock_manager_.get(), versioned_catalog_.get(),
        action_manager_.get());
  }

  absl::StatusOr<std::vector<ValueList>> ReadAll(
      ReadWriteTransaction* txn, std::vector<std::string> columns,
      std::string table_name = "test_table") {
    return ReadAllUsingIndex(txn, /*index =*/"", columns, table_name);
  }

  absl::StatusOr<std::vector<ValueList>> ReadAllUsingIndex(
      ReadWriteTransaction* txn, std::string index,
      std::vector<std::string> columns, std::string table_name = "test_table") {
    return ReadUsingIndex(txn, KeySet(KeyRange::All()), index, columns,
                          table_name);
  }

  absl::StatusOr<std::vector<ValueList>> ReadUsingIndex(
      ReadWriteTransaction* txn, KeySet key_set, std::string index,
      std::vector<std::string> columns, std::string table_name = "test_table") {
    backend::ReadArg read_arg{.table = table_name,
                              .index = index,
                              .key_set = key_set,
                              .columns = columns};

    std::unique_ptr<backend::RowCursor> cursor;
    GOOGLESQL_RETURN_IF_ERROR(txn->Read(read_arg, &cursor));

    std::vector<ValueList> rows;
    while (cursor->Next()) {
      rows.emplace_back();
      for (int i = 0; i < cursor->NumColumns(); i++) {
        rows.back().push_back(cursor->ColumnValue(i));
      }
    }
    return rows;
  }

  auto IsOkAndHoldsRows(const std::vector<ValueList>& rows) {
    return googlesql_base::testing::IsOkAndHolds(testing::ElementsAreArray(rows));
  }
};

TEST_F(ReadWriteTransactionTest, CanReadAfterFlush) {
  // Buffer mutations.
  Mutation m;
  m.AddWriteOp(MutationOpType::kInsert, "test_table",
               {"int64_col", "string_col"}, {{Int64(1), String("value1")}});
  m.AddWriteOp(MutationOpType::kInsert, "test_table",
               {"int64_col", "string_col"}, {{Int64(2), String("value2")}});

  // Commit the transaction.
  auto txn1 = CreateReadWriteTransaction();
  GOOGLESQL_EXPECT_OK(txn1->Write(m));
  GOOGLESQL_EXPECT_OK(txn1->Commit());

  // Verify the values.
  auto txn2 = CreateReadWriteTransaction();
  EXPECT_THAT(ReadAll(txn2.get(), {"int64_col", "string_col"}),
              IsOkAndHoldsRows({{Int64(1), String("value1")},
                                {Int64(2), String("value2")}}));

  // Update some of the existing rows and insert some new rows.
  Mutation m2;
  m2.AddWriteOp(MutationOpType::kInsert, "test_table",
                {"int64_col", "string_col"}, {{Int64(3), String("value3")}});
  m2.AddWriteOp(MutationOpType::kUpdate, "test_table",
                {"int64_col", "string_col"},
                {{Int64(1), String("new-value1")}});
  m2.AddDeleteOp("test_table", KeySet(Key({Int64(2)})));
  GOOGLESQL_EXPECT_OK(txn2->Write(m2));
  GOOGLESQL_EXPECT_OK(txn2->Commit());

  // Verify that updates are flushed to underlying storage.
  auto txn3 = CreateReadWriteTransaction();
  EXPECT_THAT(ReadAll(txn3.get(), {"int64_col", "string_col"}),
              IsOkAndHoldsRows({{Int64(1), String("new-value1")},
                                {Int64(3), String("value3")}}));
}

TEST_F(ReadWriteTransactionTest, RepeatableReadUsesSnapshotAndAbortsLostUpdate) {
  Mutation insert;
  insert.AddWriteOp(MutationOpType::kInsert, "test_table",
                    {"int64_col", "string_col"},
                    {{Int64(1), String("v1")}, {Int64(2), String("v2")}});
  auto setup = CreateReadWriteTransaction();
  GOOGLESQL_ASSERT_OK(setup->Write(insert));
  GOOGLESQL_ASSERT_OK(setup->Commit());

  ReadWriteOptions repeatable_read;
  repeatable_read.repeatable_read = true;
  auto reader = CreateReadWriteTransaction(repeatable_read);
  EXPECT_THAT(ReadAll(reader.get(), {"int64_col", "string_col"}),
              IsOkAndHoldsRows({{Int64(1), String("v1")},
                                {Int64(2), String("v2")}}));

  // A concurrent writer is not blocked by the snapshot read and commits.
  Mutation update;
  update.AddWriteOp(MutationOpType::kUpdate, "test_table",
                    {"int64_col", "string_col"}, {{Int64(1), String("w1")}});
  auto writer = CreateReadWriteTransaction();
  GOOGLESQL_ASSERT_OK(writer->Write(update));
  GOOGLESQL_ASSERT_OK(writer->Commit());

  // The repeatable-read transaction still sees its snapshot.
  EXPECT_THAT(ReadAll(reader.get(), {"int64_col", "string_col"}),
              IsOkAndHoldsRows({{Int64(1), String("v1")},
                                {Int64(2), String("v2")}}));

  // Writing the row the other transaction committed after the snapshot is a
  // lost update and aborts at commit.
  Mutation overwrite;
  overwrite.AddWriteOp(MutationOpType::kUpdate, "test_table",
                       {"int64_col", "string_col"}, {{Int64(1), String("x1")}});
  GOOGLESQL_ASSERT_OK(reader->Write(overwrite));
  EXPECT_THAT(reader->Commit(), StatusIs(absl::StatusCode::kAborted));

  auto verify = CreateReadWriteTransaction();
  EXPECT_THAT(ReadAll(verify.get(), {"int64_col", "string_col"}),
              IsOkAndHoldsRows({{Int64(1), String("w1")},
                                {Int64(2), String("v2")}}));
}

TEST_F(ReadWriteTransactionTest, RepeatableReadAllowsDisjointWrites) {
  Mutation insert;
  insert.AddWriteOp(MutationOpType::kInsert, "test_table",
                    {"int64_col", "string_col"},
                    {{Int64(1), String("v1")}, {Int64(2), String("v2")}});
  auto setup = CreateReadWriteTransaction();
  GOOGLESQL_ASSERT_OK(setup->Write(insert));
  GOOGLESQL_ASSERT_OK(setup->Commit());

  // Both transactions read both rows and each writes a different row: write
  // skew is allowed under repeatable read.
  ReadWriteOptions repeatable_read;
  repeatable_read.repeatable_read = true;
  auto first = CreateReadWriteTransaction(repeatable_read);
  auto second = CreateReadWriteTransaction(repeatable_read);
  GOOGLESQL_ASSERT_OK(ReadAll(first.get(), {"int64_col", "string_col"}));
  GOOGLESQL_ASSERT_OK(ReadAll(second.get(), {"int64_col", "string_col"}));

  Mutation first_update;
  first_update.AddWriteOp(MutationOpType::kUpdate, "test_table",
                          {"int64_col", "string_col"},
                          {{Int64(1), String("a")}});
  Mutation second_update;
  second_update.AddWriteOp(MutationOpType::kUpdate, "test_table",
                           {"int64_col", "string_col"},
                           {{Int64(2), String("b")}});
  GOOGLESQL_ASSERT_OK(first->Write(first_update));
  GOOGLESQL_ASSERT_OK(second->Write(second_update));
  GOOGLESQL_EXPECT_OK(first->Commit());
  GOOGLESQL_EXPECT_OK(second->Commit());

  auto verify = CreateReadWriteTransaction();
  EXPECT_THAT(ReadAll(verify.get(), {"int64_col", "string_col"}),
              IsOkAndHoldsRows({{Int64(1), String("a")},
                                {Int64(2), String("b")}}));
}

TEST_F(ReadWriteTransactionTest, ReadEmptyDatabase) {
  auto txn1 = CreateReadWriteTransaction();
  EXPECT_THAT(ReadAll(txn1.get(), {"int64_col", "string_col"}),
              IsOkAndHoldsRows({}));
}

TEST_F(ReadWriteTransactionTest, ReadTableNotFound) {
  std::unique_ptr<backend::RowCursor> cursor;
  backend::ReadArg read_arg;
  read_arg.table = "non-existend-table";
  read_arg.columns = {"int64_col", "string_col"};
  read_arg.key_set = KeySet(Key({Int64(1)}));

  auto txn = CreateReadWriteTransaction();
  EXPECT_THAT(txn->Read(read_arg, &cursor),
              StatusIs(absl::StatusCode::kNotFound));
}

TEST_F(ReadWriteTransactionTest, ReadColumnNotFound) {
  std::unique_ptr<backend::RowCursor> cursor;
  backend::ReadArg read_arg;
  read_arg.table = "test_table";
  read_arg.columns = {"non-existent-column", "string_col"};
  read_arg.key_set = KeySet(Key({Int64(1)}));

  auto txn = CreateReadWriteTransaction();
  EXPECT_THAT(txn->Read(read_arg, &cursor),
              StatusIs(absl::StatusCode::kNotFound));
}

TEST_F(ReadWriteTransactionTest, Commit) {
  // Buffer mutations.
  Mutation m;
  m.AddWriteOp(MutationOpType::kInsert, "test_table",
               {"int64_col", "string_col"}, {{Int64(3), String("value")}});

  auto txn1 = CreateReadWriteTransaction();
  GOOGLESQL_EXPECT_OK(txn1->Write(m));

  // Commit the transaction.
  absl::Time before_commit_timestamp_ = clock_.Now();
  GOOGLESQL_EXPECT_OK(txn1->Commit());

  // Verify Commit Timestamp.
  GOOGLESQL_ASSERT_OK_AND_ASSIGN(auto commit_timestamp_, txn1->GetCommitTimestamp());
  EXPECT_GT(commit_timestamp_, before_commit_timestamp_);
  EXPECT_EQ(txn1->state(), ReadWriteTransaction::State::kCommitted);

  // Start new transaction and verify the values.
  auto txn2 = CreateReadWriteTransaction();
  EXPECT_THAT(ReadAll(txn2.get(), {"int64_col", "string_col"}),
              IsOkAndHoldsRows({{Int64(3), String("value")}}));

  // Verify read using index.
  EXPECT_THAT(ReadAllUsingIndex(txn2.get(), "test_index", {"string_col"}),
              IsOkAndHoldsRows({{String("value")}}));
}

TEST_F(ReadWriteTransactionTest, CommitWithNoBufferedMutation) {
  absl::Time before_commit_timestamp_ = clock_.Now();

  // Commit the transaction.
  auto txn = CreateReadWriteTransaction();
  GOOGLESQL_EXPECT_OK(txn->Commit());

  // Verify Commit Timestamp.
  GOOGLESQL_ASSERT_OK_AND_ASSIGN(auto commit_timestamp_, txn->GetCommitTimestamp());
  EXPECT_GT(commit_timestamp_, before_commit_timestamp_);
  EXPECT_EQ(txn->state(), ReadWriteTransaction::State::kCommitted);
}

TEST_F(ReadWriteTransactionTest, CommitWithMultipleChangesToDatabase) {
  // Buffer mutations.
  Mutation m;
  m.AddWriteOp(MutationOpType::kInsert, "test_table",
               {"int64_col", "string_col"}, {{Int64(3), String("value")}});
  m.AddWriteOp(MutationOpType::kInsert, "test_table",
               {"int64_col", "string_col"}, {{Int64(6), String("value-2")}});

  absl::Time before_commit_timestamp_ = clock_.Now();

  // Commit the transaction.
  auto txn1 = CreateReadWriteTransaction();
  GOOGLESQL_EXPECT_OK(txn1->Write(m));
  GOOGLESQL_EXPECT_OK(txn1->Commit());

  // Verify Commit Timestamp.
  GOOGLESQL_ASSERT_OK_AND_ASSIGN(auto commit_timestamp_, txn1->GetCommitTimestamp());
  EXPECT_GT(commit_timestamp_, before_commit_timestamp_);
  EXPECT_EQ(txn1->state(), ReadWriteTransaction::State::kCommitted);

  // Verify the values.
  auto txn2 = CreateReadWriteTransaction();
  EXPECT_THAT(ReadAll(txn2.get(), {"int64_col", "string_col"}),
              IsOkAndHoldsRows({{Int64(3), String("value")},
                                {Int64(6), String("value-2")}}));

  // Verify that read using index results in descending order for string_col.
  EXPECT_THAT(ReadAllUsingIndex(txn2.get(), "test_index", {"string_col"}),
              IsOkAndHoldsRows({{String("value-2")}, {String("value")}}));
}

TEST_F(ReadWriteTransactionTest,
       OverlappingReadWriteTransactionsReturnAborted) {
  auto current_probability = config::abort_current_transaction_probability();
  config::set_abort_current_transaction_probability(0);
  const absl::Duration current_timeout = config::lock_wait_timeout();
  config::set_lock_wait_timeout_ms(10);
  Mutation seed;
  seed.AddWriteOp(MutationOpType::kInsert, "test_table",
                  {"int64_col", "string_col"}, {{Int64(1), String("initial")}});
  auto seeded = CreateReadWriteTransaction();
  GOOGLESQL_EXPECT_OK(seeded->Write(seed));
  GOOGLESQL_EXPECT_OK(seeded->Commit());

  Mutation m1;
  m1.AddWriteOp(MutationOpType::kUpdate, "test_table",
                {"int64_col", "string_col"}, {{Int64(1), String("value-1")}});

  auto txn1 = CreateReadWriteTransaction();
  GOOGLESQL_EXPECT_OK(txn1->Write(m1));

  // The second, younger transaction writes the same row. It waits for the
  // first to release its lock, and aborts when the wait times out.
  auto txn2 = CreateReadWriteTransaction();
  Mutation m2;
  m2.AddWriteOp(MutationOpType::kUpdate, "test_table",
                {"int64_col", "string_col"}, {{Int64(1), String("value-2")}});
  for (int i = 0; i < 5; i++) {
    EXPECT_THAT(txn2->Write(m2), StatusIs(absl::StatusCode::kAborted));
  }

  // Commit the first transaction.
  GOOGLESQL_EXPECT_OK(txn1->Commit());
  EXPECT_EQ(txn1->state(), ReadWriteTransaction::State::kCommitted);

  // Now, secondary transaction can write / commit.
  GOOGLESQL_EXPECT_OK(txn2->Write(m2));
  GOOGLESQL_EXPECT_OK(txn2->Commit());
  EXPECT_EQ(txn2->state(), ReadWriteTransaction::State::kCommitted);

  config::set_lock_wait_timeout_ms(absl::ToInt64Milliseconds(current_timeout));
  config::set_abort_current_transaction_probability(current_probability);
}

TEST_F(ReadWriteTransactionTest, YoungerTransactionWaitsForOlderToCommit) {
  auto current_probability = config::abort_current_transaction_probability();
  config::set_abort_current_transaction_probability(0);
  auto update = [](const std::string& value) {
    Mutation mutation;
    mutation.AddWriteOp(MutationOpType::kInsertOrUpdate, "test_table",
                        {"int64_col", "string_col"},
                        {{Int64(1), String(value)}});
    return mutation;
  };
  auto older = CreateReadWriteTransaction();
  auto younger = CreateReadWriteTransaction();
  GOOGLESQL_ASSERT_OK(older->Write(update("older")));

  // The younger transaction blocks on the older one's lock instead of
  // aborting, and proceeds once the older transaction commits.
  absl::Status younger_status;
  std::thread younger_thread([&] {
    younger_status = younger->Write(update("younger"));
    if (younger_status.ok()) younger_status = younger->Commit();
  });
  absl::SleepFor(absl::Milliseconds(50));
  GOOGLESQL_EXPECT_OK(older->Commit());
  younger_thread.join();
  GOOGLESQL_EXPECT_OK(younger_status);

  auto reader = CreateReadWriteTransaction();
  EXPECT_THAT(ReadAll(reader.get(), {"int64_col", "string_col"}),
              IsOkAndHoldsRows({{Int64(1), String("younger")}}));
  config::set_abort_current_transaction_probability(current_probability);
}

TEST_F(ReadWriteTransactionTest, OlderTransactionWoundsYoungerHolder) {
  auto update = [](const std::string& value) {
    Mutation mutation;
    mutation.AddWriteOp(MutationOpType::kInsertOrUpdate, "test_table",
                        {"int64_col", "string_col"},
                        {{Int64(1), String(value)}});
    return mutation;
  };
  auto older = CreateReadWriteTransaction();
  auto younger = CreateReadWriteTransaction();
  GOOGLESQL_ASSERT_OK(younger->Write(update("younger")));

  GOOGLESQL_ASSERT_OK(older->Write(update("older")));
  GOOGLESQL_ASSERT_OK(older->Commit());
  EXPECT_THAT(younger->Commit(), StatusIs(absl::StatusCode::kAborted));
}

TEST_F(ReadWriteTransactionTest, TwoKeyDeadlockAbortsExactlyOneTransaction) {
  Mutation seed;
  seed.AddWriteOp(MutationOpType::kInsert, "test_table",
                  {"int64_col", "string_col"},
                  {{Int64(1), String("seed/1")}, {Int64(2), String("seed/2")}});
  auto setup = CreateReadWriteTransaction();
  GOOGLESQL_ASSERT_OK(setup->Write(seed));
  GOOGLESQL_ASSERT_OK(setup->Commit());

  // Tags each value with its key, as test_index is a unique index.
  auto update = [](int64_t key, const std::string& value) {
    Mutation mutation;
    mutation.AddWriteOp(MutationOpType::kUpdate, "test_table",
                        {"int64_col", "string_col"},
                        {{Int64(key), String(absl::StrCat(value, "/", key))}});
    return mutation;
  };
  // Repeat so that the two conflicting requests race in many runs.
  for (int run = 0; run < 50; ++run) {
    SCOPED_TRACE(absl::StrCat("run ", run));
    const std::string first_value = absl::StrCat("first-", run);
    const std::string second_value = absl::StrCat("second-", run);
    auto first = CreateReadWriteTransaction();
    auto second = CreateReadWriteTransaction();
    GOOGLESQL_ASSERT_OK(first->Write(update(1, first_value)));
    GOOGLESQL_ASSERT_OK(second->Write(update(2, second_value)));

    // Each transaction now writes the key the other one holds, then commits.
    absl::Barrier start(2);
    absl::Status first_status;
    absl::Status second_status;
    std::thread first_thread([&] {
      start.Block();
      first_status = first->Write(update(2, first_value));
      if (first_status.ok()) first_status = first->Commit();
    });
    std::thread second_thread([&] {
      start.Block();
      second_status = second->Write(update(1, second_value));
      if (second_status.ok()) second_status = second->Commit();
    });
    first_thread.join();
    second_thread.join();

    ASSERT_NE(first_status.ok(), second_status.ok())
        << "first: " << first_status << ", second: " << second_status;
    const bool first_committed = first_status.ok();
    EXPECT_THAT(first_committed ? second_status : first_status,
                StatusIs(absl::StatusCode::kAborted));

    // Retrying the aborted transaction succeeds.
    const std::string& retried_value =
        first_committed ? second_value : first_value;
    const int64_t retried_first_key = first_committed ? 2 : 1;
    auto retry = CreateReadWriteTransaction();
    GOOGLESQL_ASSERT_OK(retry->Write(update(retried_first_key, retried_value)));
    GOOGLESQL_ASSERT_OK(
        retry->Write(update(3 - retried_first_key, retried_value)));
    GOOGLESQL_ASSERT_OK(retry->Commit());

    auto verify = CreateReadWriteTransaction();
    EXPECT_THAT(ReadAll(verify.get(), {"int64_col", "string_col"}),
                IsOkAndHoldsRows(
                    {{Int64(1), String(absl::StrCat(retried_value, "/1"))},
                     {Int64(2), String(absl::StrCat(retried_value, "/2"))}}));
    GOOGLESQL_ASSERT_OK(verify->Commit());
  }
}

// Regression tests for the unique index incident: a unique index ended up
// with a duplicate key in persisted storage although clients saw
// ALREADY_EXISTS errors. The tests cover the suspected mechanisms: a
// transaction wounded after its insert passed the unique index check, a batch
// with duplicate values, and sequential inserts retried with new values.
TEST_F(ReadWriteTransactionTest,
       AbortedTransactionLeavesNoUniqueIndexEntryInStorage) {
  auto insert = [](int64_t key, const std::string& value) {
    Mutation mutation;
    mutation.AddWriteOp(MutationOpType::kInsert, "test_table",
                        {"int64_col", "string_col"},
                        {{Int64(key), String(value)}});
    return mutation;
  };
  const int previous_probability =
      config::abort_current_transaction_probability();
  // 100 makes the second insert wound the first one after it passed the
  // unique index check; 0 makes the second insert abort itself instead.
  for (const int probability : {100, 0}) {
    SCOPED_TRACE(absl::StrCat("probability ", probability));
    config::set_abort_current_transaction_probability(probability);
    const std::string value = absl::StrCat("duplicate-", probability);
    const int64_t first_key = probability + 1;
    const int64_t second_key = probability + 2;
    auto first = CreateReadWriteTransaction();
    auto second = CreateReadWriteTransaction();
    GOOGLESQL_ASSERT_OK(first->Write(insert(first_key, value)));
    const absl::Status second_write = second->Write(insert(second_key, value));
    const absl::Status second_status =
        second_write.ok() ? second->Commit() : second_write;
    const absl::Status first_status = first->Commit();
    ASSERT_NE(first_status.ok(), second_status.ok())
        << "first: " << first_status << ", second: " << second_status;
    EXPECT_THAT(first_status.ok() ? second_status : first_status,
                StatusIs(absl::StatusCode::kAborted));
    const int64_t committed_key = first_status.ok() ? first_key : second_key;
    const int64_t aborted_key = first_status.ok() ? second_key : first_key;
    EXPECT_EQ(committed_key, probability == 100 ? second_key : first_key);

    // Only the committed row reached storage, in the table and the index.
    auto verify = CreateReadWriteTransaction();
    GOOGLESQL_ASSERT_OK_AND_ASSIGN(
        std::vector<ValueList> index_rows,
        ReadAllUsingIndex(verify.get(), "test_index",
                          {"string_col", "int64_col"}));
    EXPECT_THAT(index_rows, testing::Contains(ValueList{
                                String(value), Int64(committed_key)}));
    EXPECT_THAT(index_rows, testing::Not(testing::Contains(ValueList{
                                String(value), Int64(aborted_key)})));
    EXPECT_THAT(ReadUsingIndex(verify.get(), KeySet(Key({Int64(aborted_key)})),
                               /*index=*/"", {"int64_col"}),
                IsOkAndHoldsRows({}));
    GOOGLESQL_ASSERT_OK(verify->Commit());

    // Retrying the aborted insert now violates the unique index.
    auto retry = CreateReadWriteTransaction();
    EXPECT_THAT(retry->Write(insert(aborted_key, value)),
                StatusIs(absl::StatusCode::kAlreadyExists));
  }
  config::set_abort_current_transaction_probability(previous_probability);
}

TEST_F(ReadWriteTransactionTest, BatchWithDuplicateUniqueValuesCommitsNothing) {
  auto txn = CreateReadWriteTransaction();
  Mutation mutation;
  mutation.AddWriteOp(
      MutationOpType::kInsert, "test_table", {"int64_col", "string_col"},
      {{Int64(1), String("duplicate")}, {Int64(2), String("duplicate")}});
  EXPECT_THAT(txn->Write(mutation),
              StatusIs(absl::StatusCode::kAlreadyExists));

  auto verify = CreateReadWriteTransaction();
  EXPECT_THAT(ReadAll(verify.get(), {"int64_col"}), IsOkAndHoldsRows({}));
  EXPECT_THAT(ReadAllUsingIndex(verify.get(), "test_index", {"string_col"}),
              IsOkAndHoldsRows({}));
  GOOGLESQL_ASSERT_OK(verify->Commit());
}

TEST_F(ReadWriteTransactionTest,
       SequentialInsertsRetriedWithNewValuesKeepTheIndexUnique) {
  // Like a random data generator: insert a row, and on ALREADY_EXISTS retry
  // with a new value.
  constexpr int kRows = 40;
  for (int key = 0; key < kRows; ++key) {
    for (int attempt = 0;; ++attempt) {
      ASSERT_LT(attempt, 10);
      auto txn = CreateReadWriteTransaction();
      Mutation mutation;
      mutation.AddWriteOp(
          MutationOpType::kInsert, "test_table", {"int64_col", "string_col"},
          {{Int64(key), String(absl::StrCat("value-", (key + attempt) % 8,
                                            "-", attempt))}});
      absl::Status status = txn->Write(mutation);
      if (status.ok()) status = txn->Commit();
      if (status.ok()) break;
      ASSERT_THAT(status, StatusIs(absl::StatusCode::kAlreadyExists));
    }
  }

  auto verify = CreateReadWriteTransaction();
  GOOGLESQL_ASSERT_OK_AND_ASSIGN(
      std::vector<ValueList> rows,
      ReadAllUsingIndex(verify.get(), "test_index", {"string_col"}));
  EXPECT_EQ(rows.size(), kRows);
  absl::flat_hash_set<std::string> values;
  for (const ValueList& row : rows) {
    EXPECT_TRUE(values.insert(row[0].string_value()).second)
        << "duplicate " << row[0];
  }
  GOOGLESQL_ASSERT_OK(verify->Commit());
}

TEST_F(ReadWriteTransactionTest, DisjointWritesCanCommitWhileBothAreActive) {
  Mutation first_mutation;
  first_mutation.AddWriteOp(MutationOpType::kInsert, "test_table",
                            {"int64_col", "string_col"},
                            {{Int64(1), String("value-1")}});
  Mutation second_mutation;
  second_mutation.AddWriteOp(MutationOpType::kInsert, "test_table",
                             {"int64_col", "string_col"},
                             {{Int64(2), String("value-2")}});

  auto first = CreateReadWriteTransaction();
  auto second = CreateReadWriteTransaction();
  GOOGLESQL_ASSERT_OK(first->Write(first_mutation));
  GOOGLESQL_ASSERT_OK(second->Write(second_mutation));
  GOOGLESQL_ASSERT_OK(second->Commit());
  GOOGLESQL_ASSERT_OK(first->Commit());

  auto reader = CreateReadWriteTransaction();
  EXPECT_THAT(ReadAll(reader.get(), {"int64_col", "string_col"}),
              IsOkAndHoldsRows({{Int64(1), String("value-1")},
                                {Int64(2), String("value-2")}}));
}

TEST_F(ReadWriteTransactionTest, FaultInjectionRetriesAfterFirstCommitAbort) {
  struct RestoreFaultInjectionFlag {
    bool previous = absl::GetFlag(FLAGS_enable_fault_injection);
    ~RestoreFaultInjectionFlag() {
      absl::SetFlag(&FLAGS_enable_fault_injection, previous);
    }
  } restore_flag;
  absl::SetFlag(&FLAGS_enable_fault_injection, true);
  ASSERT_TRUE(config::fault_injection_enabled());

  bool injected_abort = false;
  for (int i = 1; i <= 500; ++i) {
    auto txn = CreateReadWriteTransaction();
    Mutation mutation;
    mutation.AddWriteOp(MutationOpType::kInsert, "test_table",
                        {"int64_col", "string_col"},
                        {{Int64(i), String(std::to_string(i))}});
    GOOGLESQL_ASSERT_OK(txn->Write(mutation));

    absl::Status first_commit = txn->Commit();
    if (first_commit.code() == absl::StatusCode::kAborted) {
      injected_abort = true;
      GOOGLESQL_ASSERT_OK(txn->Write(mutation));
      GOOGLESQL_ASSERT_OK(txn->Commit());
      break;
    }
    GOOGLESQL_ASSERT_OK(first_commit);
  }
  EXPECT_TRUE(injected_abort) << "No injected first-commit abort in 500 attempts";
}

TEST_F(ReadWriteTransactionTest, ConcurrentTransactionsEventuallySucceed) {
  // Start n threads each doing a transactional increment k times.
  int n = 20;
  int k = 10;

  // Seed value that will now be updated n*k times.
  Mutation seed_m;
  seed_m.AddWriteOp(MutationOpType::kInsert, "test_table",
                    {"int64_col", "int64_val_col"}, {{Int64(1), Int64(0)}});

  auto txn1 = CreateReadWriteTransaction();
  GOOGLESQL_EXPECT_OK(txn1->Write(seed_m));
  GOOGLESQL_EXPECT_OK(txn1->Commit());
  EXPECT_EQ(txn1->state(), ReadWriteTransaction::State::kCommitted);

  // Read args.
  backend::ReadArg read_arg;
  read_arg.table = "test_table";
  read_arg.columns = {"int64_val_col"};
  read_arg.key_set = KeySet(Key({Int64(1)}));

  std::vector<std::thread> threads;
  for (int i = 0; i < n; ++i) {
    threads.emplace_back([&]() {
      for (int j = 0; j < k; ++j) {
        while (true) {
          // Start a  ReadWriteTransaction that will read and increment
          // this value.
          auto cur_txn = CreateReadWriteTransaction();
          std::unique_ptr<backend::RowCursor> cursor;
          absl::Status status = cur_txn->Read(read_arg, &cursor);
          if (status.ok()) {
            // Increment and save this value to the database.
            int cur_val = 0;
            while (cursor->Next()) {
              cur_val = cursor->ColumnValue(0).int64_value();
            }
            GOOGLESQL_ASSERT_OK(cursor->Status());
            Mutation m;
            m.AddWriteOp(MutationOpType::kUpdate, "test_table",
                         {"int64_col", "int64_val_col"},
                         {{Int64(1), Int64(cur_val + 1)}});

            status.Update(cur_txn->Write(m));
            if (status.ok()) {
              status.Update(cur_txn->Commit());
              if (status.ok()) {
                GOOGLESQL_ASSERT_OK(status);

                EXPECT_EQ(cur_txn->state(),
                          ReadWriteTransaction::State::kCommitted);

                break;
              }
            }
          }

          // Retry on abort.
        }
      }
    });
  }

  // Wait for all threads to complete.
  for (std::thread& thread : threads) {
    thread.join();
  }

  // Verify the value.
  auto txn2 = CreateReadWriteTransaction();
  std::unique_ptr<backend::RowCursor> cursor;
  GOOGLESQL_EXPECT_OK(txn2->Read(read_arg, &cursor));
  int final_val;
  while (cursor->Next()) {
    final_val = cursor->ColumnValue(0).int64_value();
  }
  EXPECT_THAT(final_val, n * k);
}

TEST_F(ReadWriteTransactionTest, OneTransactionDoesNotBlockAllOthers) {
  // Create a row that can be read by read/write transactions.
  Mutation seed_m;
  seed_m.AddWriteOp(MutationOpType::kInsert, "test_table",
                    {"int64_col", "int64_val_col"}, {{Int64(1), Int64(0)}});

  auto setup_txn = CreateReadWriteTransaction();
  GOOGLESQL_ASSERT_OK(setup_txn->Write(seed_m));
  GOOGLESQL_ASSERT_OK(setup_txn->Commit());
  ASSERT_EQ(setup_txn->state(), ReadWriteTransaction::State::kCommitted);

  // Read args.
  backend::ReadArg read_arg;
  read_arg.table = "test_table";
  read_arg.columns = {"int64_val_col"};
  read_arg.key_set = KeySet(Key({Int64(1)}));

  // Start a ReadWriteTransaction that will read and increment
  // the value, but not (yet) commit.
  auto cur_txn = CreateReadWriteTransaction();
  std::unique_ptr<backend::RowCursor> cursor;
  GOOGLESQL_ASSERT_OK(cur_txn->Read(read_arg, &cursor));
  // Increment and save this value to the database.
  int cur_val = 0;
  while (cursor->Next()) {
    cur_val = cursor->ColumnValue(0).int64_value();
  }
  GOOGLESQL_ASSERT_OK(cursor->Status());
  Mutation m;
  m.AddWriteOp(MutationOpType::kUpdate, "test_table",
               {"int64_col", "int64_val_col"},
               {{Int64(1), Int64(cur_val + 1)}});
  GOOGLESQL_ASSERT_OK(cur_txn->Write(m));

  // Start another read/write transaction that will read and update the same
  // value. We do this in a retry loop to make sure it eventually succeeds.
  // This is how all read/write transactions on Spanner should be executed.
  // Each attempt is a new, younger transaction that waits for the idle one
  // until the wait times out, unless it wounds the idle transaction per
  // --abort_current_transaction_probability. Shorten the waits.
  const absl::Duration current_timeout = config::lock_wait_timeout();
  config::set_lock_wait_timeout_ms(10);
  auto attempts = 0;
  while (true) {
    auto other_txn = CreateReadWriteTransaction();
    auto status = other_txn->Read(read_arg, &cursor);
    if (status.ok()) {
      while (cursor->Next()) {
        cur_val = cursor->ColumnValue(0).int64_value();
      }
      GOOGLESQL_ASSERT_OK(cursor->Status());
      m.AddWriteOp(MutationOpType::kUpdate, "test_table",
                   {"int64_col", "int64_val_col"},
                   {{Int64(1), Int64(cur_val + 1)}});
      GOOGLESQL_ASSERT_OK(other_txn->Write(m));
      GOOGLESQL_ASSERT_OK(other_txn->Commit());
      ASSERT_EQ(other_txn->state(), ReadWriteTransaction::State::kCommitted);
      break;
    } else {
      // Retry if the status is Aborted. Fail in all other cases.
      ASSERT_EQ(status.code(), absl::StatusCode::kAborted);
    }
    attempts++;
    if (attempts > 1000) {
      FAIL() << "Transaction did not succeed after 1000 attempts.";
    }
  }
  config::set_lock_wait_timeout_ms(absl::ToInt64Milliseconds(current_timeout));

  // Verify that the first transaction was aborted.
  EXPECT_EQ(cur_txn->state(), ReadWriteTransaction::State::kAborted);

  // Verify the value.
  auto verify_txn = CreateReadWriteTransaction();
  GOOGLESQL_ASSERT_OK(verify_txn->Read(read_arg, &cursor));
  int final_val;
  while (cursor->Next()) {
    final_val = cursor->ColumnValue(0).int64_value();
  }
  GOOGLESQL_ASSERT_OK(verify_txn->Commit());
  EXPECT_EQ(final_val, 1);
}

TEST_F(ReadWriteTransactionTest, ConcurrentSchemaUpdatesWithTransactions) {
  // Start a ReadWrite transaction. This holds default schema at start of the
  // database creation.
  auto txn = CreateReadWriteTransaction();

  // Update the schema with "new_table".
  auto schema = test::CreateSchemaFromDDL(
                    {
                        R"(
                          CREATE TABLE new_table (
                            int64_col INT64 NOT NULL,
                            string_col STRING(MAX)
                          ) PRIMARY KEY (int64_col)
                        )",
                    },
                    type_factory_.get())
                    .value();
  GOOGLESQL_ASSERT_OK(versioned_catalog_->AddSchema(clock_.Now(), std::move(schema)));
  action_manager_->AddActionsForSchema(versioned_catalog_->GetLatestSchema(),
                                       /*function_catalog=*/nullptr,
                                       type_factory_.get());

  // Transaction should return latest schema unless an operation is performed.
  ASSERT_NE(txn->schema()->FindTable("new_table"), nullptr);

  Mutation m;
  m.AddWriteOp(MutationOpType::kInsert, "new_table",
               {"int64_col", "string_col"}, {{Int64(1), String("value")}});
  GOOGLESQL_EXPECT_OK(txn->Write(m));

  // Schema change to verify the transaction is aborted in subsequent requests.
  schema = test::CreateSchemaFromDDL(
               {
                   R"(
                    CREATE TABLE another_new_table (
                      int64_col INT64 NOT NULL,
                      string_col STRING(MAX)
                    ) PRIMARY KEY (int64_col)
                  )",
               },
               type_factory_.get())
               .value();
  GOOGLESQL_ASSERT_OK(versioned_catalog_->AddSchema(clock_.Now(), std::move(schema)));
  action_manager_->AddActionsForSchema(versioned_catalog_->GetLatestSchema(),
                                       /*function_catalog=*/nullptr,
                                       type_factory_.get());

  // Transaction is aborted.
  EXPECT_THAT(txn->Write(m), StatusIs(absl::StatusCode::kAborted));
}

TEST_F(ReadWriteTransactionTest, CommitWithNoEffectiveChangesToDatabase) {
  // Buffer mutations.
  Mutation m;
  m.AddWriteOp(MutationOpType::kInsert, "test_table",
               {"int64_col", "string_col"}, {{Int64(3), String("value")}});
  m.AddDeleteOp("test_table", KeySet(Key({Int64(3)})));

  absl::Time before_commit_timestamp_ = clock_.Now();

  // Commit the transaction.
  auto txn1 = CreateReadWriteTransaction();
  GOOGLESQL_EXPECT_OK(txn1->Write(m));
  GOOGLESQL_EXPECT_OK(txn1->Commit());

  // Verify Commit Timestamp.
  GOOGLESQL_ASSERT_OK_AND_ASSIGN(auto commit_timestamp_, txn1->GetCommitTimestamp());
  EXPECT_GT(commit_timestamp_, before_commit_timestamp_);
  EXPECT_EQ(txn1->state(), ReadWriteTransaction::State::kCommitted);

  // Verify the values.
  auto txn2 = CreateReadWriteTransaction();
  EXPECT_THAT(ReadAll(txn2.get(), {"int64_col", "string_col"}),
              IsOkAndHoldsRows({}));
}

TEST_F(ReadWriteTransactionTest, DuplicateCommitFails) {
  // Commit the transaction.
  auto txn = CreateReadWriteTransaction();
  GOOGLESQL_EXPECT_OK(txn->Commit());

  // Verify Commit State.
  EXPECT_EQ(txn->state(), ReadWriteTransaction::State::kCommitted);

  // Call commit on a committed transaction should fail.
  EXPECT_THAT(txn->Commit(), StatusIs(absl::StatusCode::kInternal));
}

TEST_F(ReadWriteTransactionTest, CommitAfterRollbackFails) {
  // Rollback transaction.
  auto txn = CreateReadWriteTransaction();
  GOOGLESQL_EXPECT_OK(txn->Rollback());
  EXPECT_EQ(txn->state(), ReadWriteTransaction::State::kRolledback);

  // Call commit on a rolled-back transaction should fail.
  EXPECT_THAT(txn->Commit(), StatusIs(absl::StatusCode::kInternal));
}

TEST_F(ReadWriteTransactionTest, GetCommitTimestampWithoutTransactionCommit) {
  auto txn = CreateReadWriteTransaction();
  EXPECT_THAT(txn->GetCommitTimestamp(), StatusIs(absl::StatusCode::kInternal));
}

TEST_F(ReadWriteTransactionTest, FailsReadWithInvalidIndex) {
  // Buffer mutations.
  Mutation m;
  m.AddWriteOp(MutationOpType::kInsert, "test_table",
               {"int64_col", "string_col"}, {{Int64(3), String("value")}});

  // Commit the transaction.
  auto txn1 = CreateReadWriteTransaction();
  GOOGLESQL_EXPECT_OK(txn1->Write(m));
  GOOGLESQL_EXPECT_OK(txn1->Commit());

  // Verify that reading from an invalid index fails.
  auto txn2 = CreateReadWriteTransaction();
  EXPECT_THAT(ReadAllUsingIndex(txn2.get(), "invalid_index",
                                {"int64_col", "string_col"}),
              StatusIs(absl::StatusCode::kNotFound));
}

TEST_F(ReadWriteTransactionTest, CanReadUsingIndex) {
  // Buffer mutations.
  Mutation m;
  m.AddWriteOp(MutationOpType::kInsert, "test_table",
               {"int64_col", "string_col"}, {{Int64(3), String("value")}});

  // Commit the transaction.
  auto txn1 = CreateReadWriteTransaction();
  GOOGLESQL_EXPECT_OK(txn1->Write(m));
  GOOGLESQL_EXPECT_OK(txn1->Commit());

  // Verify the values.
  auto txn2 = CreateReadWriteTransaction();
  EXPECT_THAT(ReadUsingIndex(txn2.get(), KeySet(Key({String("value")})),
                             "test_index", {"string_col", "int64_col"}),
              IsOkAndHoldsRows({{String("value"), Int64(3)}}));
}

TEST_F(ReadWriteTransactionTest, IndexInsertTest) {
  // Buffer mutations.
  Mutation m;
  m.AddWriteOp(MutationOpType::kInsert, "test_table",
               {"int64_col", "string_col"}, {{Int64(3), String("value")}});

  // Commit the transaction.
  auto txn1 = CreateReadWriteTransaction();
  GOOGLESQL_EXPECT_OK(txn1->Write(m));
  GOOGLESQL_EXPECT_OK(txn1->Commit());

  // Verify the values.
  auto txn2 = CreateReadWriteTransaction();
  EXPECT_THAT(
      ReadAllUsingIndex(txn2.get(), "test_index", {"string_col", "int64_col"}),
      IsOkAndHoldsRows({{String("value"), Int64(3)}}));

  EXPECT_THAT(ReadUsingIndex(txn2.get(), KeySet(Key({String("value")})),
                             "test_index", {"string_col"}),
              IsOkAndHoldsRows({{String("value")}}));

  // Searching for empty string should result in no elements.
  EXPECT_THAT(ReadUsingIndex(txn2.get(), KeySet(Key({String("")})),
                             "test_index", {"string_col"}),
              IsOkAndHoldsRows({}));
}

TEST_F(ReadWriteTransactionTest, IndexUpdateTest) {
  // Buffer mutations.
  Mutation m;
  m.AddWriteOp(MutationOpType::kInsert, "test_table",
               {"int64_col", "string_col"}, {{Int64(3), String("value")}});
  m.AddWriteOp(MutationOpType::kUpdate, "test_table",
               {"int64_col", "string_col"}, {{Int64(3), String("new-value")}});

  // Commit the transaction.
  auto txn1 = CreateReadWriteTransaction();
  GOOGLESQL_EXPECT_OK(txn1->Write(m));
  GOOGLESQL_EXPECT_OK(txn1->Commit());

  // Verify the index contains value "new-value" and not "value"
  auto txn2 = CreateReadWriteTransaction();
  EXPECT_THAT(ReadUsingIndex(txn2.get(), KeySet(Key({String("value")})),
                             "test_index", {"string_col"}),
              IsOkAndHoldsRows({}));

  EXPECT_THAT(ReadUsingIndex(txn2.get(), KeySet(Key({String("new-value")})),
                             "test_index", {"string_col"}),
              IsOkAndHoldsRows({{String("new-value")}}));
}

TEST_F(ReadWriteTransactionTest, IndexDeleteTest) {
  // Buffer mutations.
  Mutation m;
  m.AddWriteOp(MutationOpType::kInsert, "test_table",
               {"int64_col", "string_col"}, {{Int64(3), String("value")}});
  m.AddDeleteOp("test_table", KeySet(Key({Int64(3)})));

  // Commit the transaction.
  auto txn1 = CreateReadWriteTransaction();
  GOOGLESQL_EXPECT_OK(txn1->Write(m));
  GOOGLESQL_EXPECT_OK(txn1->Commit());

  // Verify the values.
  auto txn2 = CreateReadWriteTransaction();
  EXPECT_THAT(ReadUsingIndex(txn2.get(), KeySet(Key({String("value")})),
                             "test_index", {"string_col"}),
              IsOkAndHoldsRows({}));
}

TEST_F(ReadWriteTransactionTest, IndexDeleteAreIdempotentTest) {
  // Buffer mutations.
  Mutation m;
  m.AddWriteOp(MutationOpType::kInsert, "test_table",
               {"int64_col", "string_col"}, {{Int64(3), String("value")}});
  m.AddDeleteOp("test_table", KeySet(Key({Int64(3)})));

  // Replace mutation op is translated to a Delete WriteOp which in turn
  // triggers a index delete op.
  m.AddWriteOp(MutationOpType::kReplace, "test_table",
               {"int64_col", "string_col"}, {{Int64(3), String("value2")}});

  // Commit the transaction.
  auto txn1 = CreateReadWriteTransaction();
  GOOGLESQL_EXPECT_OK(txn1->Write(m));
  GOOGLESQL_EXPECT_OK(txn1->Commit());

  // Verify the values.
  auto txn2 = CreateReadWriteTransaction();
  EXPECT_THAT(ReadUsingIndex(txn2.get(), KeySet(Key({String("value")})),
                             "test_index", {"string_col"}),
              IsOkAndHoldsRows({}));
}

TEST_F(ReadWriteTransactionTest, IndexUniquenessFailTest) {
  // Buffer two mutations that should violate index uniqueness constraint.
  Mutation m;
  m.AddWriteOp(MutationOpType::kInsert, "test_table",
               {"int64_col", "string_col"}, {{Int64(3), String("value")}});
  m.AddWriteOp(MutationOpType::kInsert, "test_table",
               {"int64_col", "string_col"}, {{Int64(4), String("value")}});

  auto txn = CreateReadWriteTransaction();
  EXPECT_THAT(txn->Write(m), StatusIs(absl::StatusCode::kAlreadyExists));
}

TEST_F(ReadWriteTransactionTest, IndexUniquenessMultiStatementFailTest) {
  Mutation m1;
  m1.AddWriteOp(MutationOpType::kInsert, "test_table",
                {"int64_col", "string_col"}, {{Int64(3), String("value")}});
  Mutation m2;
  m2.AddWriteOp(MutationOpType::kInsert, "test_table",
                {"int64_col", "string_col"}, {{Int64(4), String("value")}});

  auto txn = CreateReadWriteTransaction();
  GOOGLESQL_EXPECT_OK(txn->Write(m1));
  EXPECT_THAT(txn->Write(m2), StatusIs(absl::StatusCode::kAlreadyExists));
}

TEST_F(ReadWriteTransactionTest, DeduplicationInsertAndUpdateSameStatement) {
  auto txn = CreateReadWriteTransaction();
  Mutation m;
  m.AddWriteOp(MutationOpType::kInsert, "test_table",
               {"int64_col", "string_col"}, {{Int64(10), String("val1")}});
  m.AddWriteOp(MutationOpType::kUpdate, "test_table",
               {"int64_col", "string_col"}, {{Int64(10), String("val2")}});

  GOOGLESQL_EXPECT_OK(txn->Write(m));

  backend::ReadArg arg;
  arg.table = "test_table";
  arg.key_set = KeySet{Key{{Int64(10)}}};
  arg.columns = {"string_col"};
  std::unique_ptr<backend::RowCursor> cursor;
  GOOGLESQL_EXPECT_OK(txn->Read(arg, &cursor));

  EXPECT_TRUE(cursor->Next());
  EXPECT_EQ(cursor->ColumnValue(0), String("val2"));
  EXPECT_FALSE(cursor->Next());
}

TEST_F(ReadWriteTransactionTest,
       DeduplicationUpdatePartialColumnsSameStatement) {
  auto txn = CreateReadWriteTransaction();
  Mutation m;
  m.AddWriteOp(MutationOpType::kInsert, "test_table",
               {"int64_col", "string_col", "int64_val_col"},
               {{Int64(10), String("val1"), Int64(1)}});
  m.AddWriteOp(MutationOpType::kUpdate, "test_table",
               {"int64_col", "string_col"}, {{Int64(10), String("val2")}});

  // This should not fail because the operations are merged.
  GOOGLESQL_EXPECT_OK(txn->Write(m));

  backend::ReadArg arg;
  arg.table = "test_table";
  arg.key_set = KeySet{Key{{Int64(10)}}};
  arg.columns = {"string_col", "int64_val_col"};
  std::unique_ptr<backend::RowCursor> cursor;
  GOOGLESQL_EXPECT_OK(txn->Read(arg, &cursor));

  EXPECT_TRUE(cursor->Next());
  EXPECT_EQ(cursor->ColumnValue(0), String("val2"));
  EXPECT_EQ(cursor->ColumnValue(1), Int64(1));
  EXPECT_FALSE(cursor->Next());
}

TEST_F(ReadWriteTransactionTest, DeduplicationMultipleInsertFailSameStatement) {
  auto txn = CreateReadWriteTransaction();
  Mutation m;
  m.AddWriteOp(MutationOpType::kInsert, "test_table",
               {"int64_col", "string_col"}, {{Int64(10), String("val1")}});
  m.AddWriteOp(MutationOpType::kInsert, "test_table",
               {"int64_col", "string_col"}, {{Int64(10), String("val2")}});

  // Even though we merge for verifiers, the transaction store should still
  // report ALREADY_EXISTS for the second insert.
  EXPECT_THAT(txn->Write(m), StatusIs(absl::StatusCode::kAlreadyExists));
}

TEST_F(ReadWriteTransactionTest, DeduplicationDeleteAndInsertSameStatement) {
  auto txn = CreateReadWriteTransaction();
  // Ensure the row exists first.
  Mutation m1;
  m1.AddWriteOp(MutationOpType::kInsert, "test_table",
                {"int64_col", "string_col"}, {{Int64(10), String("initial")}});
  GOOGLESQL_EXPECT_OK(txn->Write(m1));

  Mutation m2;
  m2.AddDeleteOp("test_table", KeySet{Key{{Int64(10)}}});
  m2.AddWriteOp(MutationOpType::kInsert, "test_table",
                {"int64_col", "string_col"}, {{Int64(10), String("new")}});

  GOOGLESQL_EXPECT_OK(txn->Write(m2));

  backend::ReadArg arg;
  arg.table = "test_table";
  arg.key_set = KeySet{Key{{Int64(10)}}};
  arg.columns = {"string_col"};
  std::unique_ptr<backend::RowCursor> cursor;
  GOOGLESQL_EXPECT_OK(txn->Read(arg, &cursor));

  EXPECT_TRUE(cursor->Next());
  EXPECT_EQ(cursor->ColumnValue(0), String("new"));
  EXPECT_FALSE(cursor->Next());
}

TEST_F(ReadWriteTransactionTest, DeduplicationMultipleUpdatesSameStatement) {
  auto txn = CreateReadWriteTransaction();
  Mutation m_insert;
  m_insert.AddWriteOp(MutationOpType::kInsert, "test_table",
                      {"int64_col", "string_col"},
                      {{Int64(11), String("initial")}});
  GOOGLESQL_EXPECT_OK(txn->Write(m_insert));

  Mutation m_update;
  m_update.AddWriteOp(MutationOpType::kUpdate, "test_table",
                      {"int64_col", "string_col"},
                      {{Int64(11), String("update1")}});
  m_update.AddWriteOp(MutationOpType::kUpdate, "test_table",
                      {"int64_col", "string_col"},
                      {{Int64(11), String("update2")}});
  GOOGLESQL_EXPECT_OK(txn->Write(m_update));

  backend::ReadArg arg;
  arg.table = "test_table";
  arg.key_set = KeySet{Key{{Int64(11)}}};
  arg.columns = {"string_col"};
  std::unique_ptr<backend::RowCursor> cursor;
  GOOGLESQL_EXPECT_OK(txn->Read(arg, &cursor));

  EXPECT_TRUE(cursor->Next());
  EXPECT_EQ(cursor->ColumnValue(0), String("update2"));
  EXPECT_FALSE(cursor->Next());
}

TEST_F(ReadWriteTransactionTest, DeduplicationInsertAndDeleteSameStatement) {
  auto txn = CreateReadWriteTransaction();
  Mutation m;
  m.AddWriteOp(MutationOpType::kInsert, "test_table",
               {"int64_col", "string_col"},
               {{Int64(12), String("to_be_deleted")}});
  m.AddDeleteOp("test_table", KeySet{Key{{Int64(12)}}});

  GOOGLESQL_EXPECT_OK(txn->Write(m));

  backend::ReadArg arg;
  arg.table = "test_table";
  arg.key_set = KeySet{Key{{Int64(12)}}};
  arg.columns = {"string_col"};
  std::unique_ptr<backend::RowCursor> cursor;
  GOOGLESQL_EXPECT_OK(txn->Read(arg, &cursor));

  EXPECT_FALSE(cursor->Next());
}

TEST_F(ReadWriteTransactionTest, UpdateAfterDeleteFails) {
  Mutation m;
  m.AddDeleteOp("test_table", KeySet{Key{{Int64(4)}}});
  m.AddWriteOp(MutationOpType::kUpdate, "test_table",
               {"int64_col", "string_col"}, {{Int64(4), String("value")}});

  auto txn = CreateReadWriteTransaction();
  EXPECT_THAT(txn->Write(m), StatusIs(absl::StatusCode::kInvalidArgument));
}

TEST_F(ReadWriteTransactionTest, InsertSucceeds) {
  Mutation m;
  m.AddWriteOp(MutationOpType::kInsert, "test_table",
               {"int64_col", "string_col"}, {{Int64(1), String("val1")}});

  // Commit the transaction.
  auto txn1 = CreateReadWriteTransaction();
  GOOGLESQL_EXPECT_OK(txn1->Write(m));
  GOOGLESQL_EXPECT_OK(txn1->Commit());

  // Verify the values.
  auto txn2 = CreateReadWriteTransaction();
  EXPECT_THAT(ReadAll(txn2.get(), {"int64_col", "string_col"}),
              IsOkAndHoldsRows({{Int64(1), String("val1")}}));
}

TEST_F(ReadWriteTransactionTest, CannotInsertWithEmptyColumns) {
  Mutation m;
  m.AddWriteOp(MutationOpType::kInsert, "test_table", {}, {});

  auto txn1 = CreateReadWriteTransaction();
  EXPECT_THAT(txn1->Write(m), StatusIs(absl::StatusCode::kFailedPrecondition));
}

TEST_F(ReadWriteTransactionTest, CannotInsertWithMissingKeyColumn) {
  Mutation m;
  m.AddWriteOp(MutationOpType::kInsert, "test_table", {"string_col"},
               {{String("val1")}});

  auto txn1 = CreateReadWriteTransaction();
  EXPECT_THAT(txn1->Write(m), StatusIs(absl::StatusCode::kFailedPrecondition));
}

TEST_F(ReadWriteTransactionTest, CanInsertWithCaseInsensitiveColumns) {
  Mutation m;
  m.AddWriteOp(MutationOpType::kInsert, "test_table",
               {"InT64_cOl", "sTriNg_CoL"}, {{Int64(1), String("val1")}});

  // Commit the transaction.
  auto txn1 = CreateReadWriteTransaction();
  GOOGLESQL_EXPECT_OK(txn1->Write(m));
  GOOGLESQL_EXPECT_OK(txn1->Commit());

  // Verify the values.
  auto txn2 = CreateReadWriteTransaction();
  EXPECT_THAT(ReadAll(txn2.get(), {"int64_col", "string_col"}),
              IsOkAndHoldsRows({{Int64(1), String("val1")}}));
}

TEST_F(ReadWriteTransactionTest, CannotInsertWithDuplicateColumns) {
  Mutation m;
  m.AddWriteOp(MutationOpType::kInsert, "test_table",
               {"string_col", "string_col"},
               {{String("val1"), String("val2")}});

  auto txn1 = CreateReadWriteTransaction();
  EXPECT_THAT(txn1->Write(m), StatusIs(absl::StatusCode::kInvalidArgument));
}

TEST_F(ReadWriteTransactionTest, ManyInserts) {
  int num_inserts = 5000;
  auto txn = CreateReadWriteTransaction();

  // We write one mutation at a time to simulate DML statements which are
  // executed sequentially. Each DML statement triggers validation.
  absl::Time start = absl::Now();
  for (int i = 0; i < num_inserts; ++i) {
    Mutation m;
    m.AddWriteOp(MutationOpType::kInsert, "test_table",
                 {"int64_col", "string_col"},
                 {{Int64(i), String(std::to_string(i))}});
    ASSERT_EQ(txn->Write(m), absl::OkStatus());
  }
  absl::Time end = absl::Now();
  absl::Duration duration = end - start;
  // If it's O(N^2), 5000 inserts might take a few seconds (~90s).
  // If it's O(N), it should be very fast (< 0.5s).
  ABSL_LOG(INFO) << "Time taken for " << num_inserts << " inserts: " << duration;

  // Fail if too slow (e.g. > 5 second for 5000 inserts implies < 1k ops/sec).
  EXPECT_LE(duration, absl::Seconds(5));

  // Validate the data can be read back and is correct.
  backend::ReadArg arg;
  arg.table = "test_table";
  arg.key_set = KeySet::All();
  arg.columns = {"int64_col", "string_col"};
  std::unique_ptr<backend::RowCursor> cursor;
  ASSERT_EQ(txn->Read(arg, &cursor), absl::OkStatus());

  int count = 0;
  while (cursor->Next()) {
    EXPECT_EQ(cursor->ColumnValue(0), Int64(count));
    EXPECT_EQ(cursor->ColumnValue(1), String(std::to_string(count)));
    ++count;
  }
  EXPECT_EQ(cursor->Status(), absl::OkStatus());
  EXPECT_EQ(count, num_inserts);
}

class GeneratedPrimaryKeyTransactionTest : public ReadWriteTransactionTest {
 public:
  GeneratedPrimaryKeyTransactionTest()
      : feature_flags_({.enable_generated_pk = true}) {}
  absl::StatusOr<std::unique_ptr<const backend::Schema>> GetSchema() override {
    return test::CreateSchemaFromDDL(
        {
            R"sql(
                  CREATE TABLE test_table (
                    k1 INT64 NOT NULL,
                    k2 INT64,
                    k3 INT64 AS (k2) STORED,
                    k4 INT64 NOT NULL,
                  ) PRIMARY KEY (k1,k3)
                )sql"},
        type_factory_.get());
  }

 private:
  test::ScopedEmulatorFeatureFlagsSetter feature_flags_;
};

TEST_F(GeneratedPrimaryKeyTransactionTest, FailsWhenFeatureDisabled) {
  test::ScopedEmulatorFeatureFlagsSetter disabled_flags(
      {.enable_generated_pk = false});
  EXPECT_THAT(test::CreateSchemaFromDDL(
                  {
                      R"(
                          CREATE TABLE new_table (
                            k1 INT64 NOT NULL,
                            k2 INT64 AS (k1) STORED,
                          ) PRIMARY KEY (k1,k2)
                        )",
                  },
                  type_factory_.get()),
              googlesql_base::testing::StatusIs(
                  absl::StatusCode::kInvalidArgument,
                  testing::HasSubstr("Generated column `new_table.k2` cannot "
                                     "be part of the primary key.")));
}

TEST_F(GeneratedPrimaryKeyTransactionTest, InsertMutationsTableWithOnlyKeys) {
  // Update the schema with "new_table".
  auto schema = test::CreateSchemaFromDDL(
                    {
                        R"(
                          CREATE TABLE new_table (
                            k1 INT64 NOT NULL,
                            k2 INT64 AS (k1) STORED,
                          ) PRIMARY KEY (k1,k2)
                        )",
                    },
                    type_factory_.get())
                    .value();
  GOOGLESQL_ASSERT_OK(versioned_catalog_->AddSchema(clock_.Now(), std::move(schema)));
  action_manager_->AddActionsForSchema(versioned_catalog_->GetLatestSchema(),
                                       /*function_catalog=*/nullptr,
                                       type_factory_.get());
  auto txn = CreateReadWriteTransaction();
  ASSERT_NE(txn->schema()->FindTable("new_table"), nullptr);

  Mutation m;
  m.AddWriteOp(MutationOpType::kInsert, "new_table", {"k1"}, {{Int64(1)}});
  GOOGLESQL_EXPECT_OK(txn->Write(m));
}

TEST_F(GeneratedPrimaryKeyTransactionTest, InsertMutations) {
  Mutation m;
  m.AddWriteOp(
      MutationOpType::kInsert, "test_table", {"k1", "k2", "k4"},
      {{Int64(1), Int64(1), Int64(1)}, {Int64(3), Int64(3), Int64(3)}});
  m.AddWriteOp(MutationOpType::kInsert, "test_table", {"k1", "k2", "k4"},
               {{Int64(1), Int64(2), Int64(2)}});

  // Commit the transaction.
  auto txn1 = CreateReadWriteTransaction();
  GOOGLESQL_EXPECT_OK(txn1->Write(m));
  GOOGLESQL_EXPECT_OK(txn1->Commit());

  // Verify the values.
  auto txn2 = CreateReadWriteTransaction();
  EXPECT_THAT(ReadAll(txn2.get(), {"k1", "k2", "k3", "k4"}),
              IsOkAndHoldsRows({{Int64(1), Int64(1), Int64(1), Int64(1)},
                                {Int64(1), Int64(2), Int64(2), Int64(2)},
                                {Int64(3), Int64(3), Int64(3), Int64(3)}}));
}

TEST_F(GeneratedPrimaryKeyTransactionTest,
       InsertMutationsTableWithDependentGeneratedKeyColumns) {
  // Update the schema with "new_table".
  // This test also checks if topological sort is working as expected. Note k3
  // is dependent on a column k5 which is defined later. k5 should be evaluated
  // before k3.
  auto schema = test::CreateSchemaFromDDL(
                    {
                        R"sql(
                  CREATE TABLE new_table (
                    k1 INT64 NOT NULL,
                    k2 INT64,
                    k3 INT64 AS (k5) STORED,
                    k4 INT64 NOT NULL,
                    k5 INT64 AS (k2) STORED,
                  ) PRIMARY KEY (k1,k3,k5)
                )sql"},
                    type_factory_.get())
                    .value();
  GOOGLESQL_ASSERT_OK(versioned_catalog_->AddSchema(clock_.Now(), std::move(schema)));
  action_manager_->AddActionsForSchema(versioned_catalog_->GetLatestSchema(),
                                       /*function_catalog=*/nullptr,
                                       type_factory_.get());

  auto txn = CreateReadWriteTransaction();
  ASSERT_NE(txn->schema()->FindTable("new_table"), nullptr);

  Mutation m;
  m.AddWriteOp(
      MutationOpType::kInsert, "new_table", {"k1", "k2", "k4"},
      {{Int64(1), Int64(1), Int64(1)}, {Int64(3), Int64(3), Int64(3)}});
  m.AddWriteOp(MutationOpType::kInsert, "new_table", {"k1", "k2", "k4"},
               {{Int64(1), Int64(2), Int64(2)}});

  // Commit the transaction.
  GOOGLESQL_EXPECT_OK(txn->Write(m));
  GOOGLESQL_EXPECT_OK(txn->Commit());

  // Verify the values.
  auto txn2 = CreateReadWriteTransaction();
  EXPECT_THAT(
      ReadAll(txn2.get(), {"k1", "k2", "k3", "k4", "k5"}, "new_table"),
      IsOkAndHoldsRows({{Int64(1), Int64(1), Int64(1), Int64(1), Int64(1)},
                        {Int64(1), Int64(2), Int64(2), Int64(2), Int64(2)},
                        {Int64(3), Int64(3), Int64(3), Int64(3), Int64(3)}}));
}

TEST_F(GeneratedPrimaryKeyTransactionTest,
       FailsInsertDependentColumnsNotPresent) {
  Mutation m;
  m.AddWriteOp(MutationOpType::kInsert, "test_table", {"k1", "k4"},
               {{Int64(1), Int64(1)}});

  // Commit the transaction.
  auto txn1 = CreateReadWriteTransaction();
  EXPECT_THAT(txn1->Write(m),
              googlesql_base::testing::StatusIs(
                  absl::StatusCode::kFailedPrecondition,
                  testing::HasSubstr(
                      "The value of generated primary key column "
                      "`test_table.k3` cannot be evaluated since value of all "
                      "its dependent columns is not specified.")));
}

TEST_F(GeneratedPrimaryKeyTransactionTest, DuplicateKeyInsertionsFail) {
  Mutation m;
  m.AddWriteOp(MutationOpType::kInsert, "test_table", {"k1", "k2", "k4"},
               {{Int64(1), Int64(1), Int64(1)}});

  // Commit the transaction.
  auto txn1 = CreateReadWriteTransaction();
  GOOGLESQL_ASSERT_OK(txn1->Write(m));
  GOOGLESQL_ASSERT_OK(txn1->Commit());

  // Verify the values.
  auto txn2 = CreateReadWriteTransaction();
  EXPECT_THAT(ReadAll(txn2.get(), {"k1", "k2", "k3", "k4"}),
              IsOkAndHoldsRows({{Int64(1), Int64(1), Int64(1), Int64(1)}}));
  GOOGLESQL_EXPECT_OK(txn2->Commit());

  Mutation m2;
  m2.AddWriteOp(MutationOpType::kInsert, "test_table", {"k1", "k2", "k4"},
                {{Int64(1), Int64(1), Int64(100)}});

  // Row with PRIMARY KEY(1,1) already exists.
  auto txn3 = CreateReadWriteTransaction();
  EXPECT_THAT(
      txn3->Write(m2),
      googlesql_base::testing::StatusIs(
          absl::StatusCode::kAlreadyExists,
          testing::HasSubstr(
              "Table test_table: Row {Int64(1), Int64(1)} already exists.")));
}

TEST_F(GeneratedPrimaryKeyTransactionTest, UpdateNonPkSamePk) {
  Mutation m;
  m.AddWriteOp(MutationOpType::kInsert, "test_table", {"k1", "k2", "k4"},
               {{Int64(1), Int64(1), Int64(1)}});
  m.AddWriteOp(MutationOpType::kUpdate, "test_table", {"k1", "k2", "k3", "k4"},
               {{Int64(1), Int64(1), Int64(1), Int64(4)}});

  // Commit the transaction.
  auto txn1 = CreateReadWriteTransaction();
  GOOGLESQL_EXPECT_OK(txn1->Write(m));
  GOOGLESQL_EXPECT_OK(txn1->Commit());

  // Verify the values.
  auto txn2 = CreateReadWriteTransaction();
  EXPECT_THAT(ReadAll(txn2.get(), {"k1", "k2", "k3", "k4"}),
              IsOkAndHoldsRows({{Int64(1), Int64(1), Int64(1), Int64(4)}}));
}

TEST_F(GeneratedPrimaryKeyTransactionTest,
       CantUpdateGpkThroughDependentColumns) {
  Mutation m;
  m.AddWriteOp(MutationOpType::kInsert, "test_table", {"k1", "k2", "k4"},
               {{Int64(1), Int64(1), Int64(1)}});
  m.AddWriteOp(MutationOpType::kUpdate, "test_table", {"k1", "k2", "k3", "k4"},
               {{Int64(1), Int64(2), Int64(1), Int64(4)}});

  // Value of generated primary key columns evaluated should be same as that of
  // user provided value of generated primary key column.
  auto txn1 = CreateReadWriteTransaction();
  EXPECT_THAT(txn1->Write(m), StatusIs(absl::StatusCode::kOutOfRange));
}

TEST_F(GeneratedPrimaryKeyTransactionTest,
       FailsUpdateWithoutExplicitPrimaryKey) {
  Mutation m;
  m.AddWriteOp(MutationOpType::kInsert, "test_table", {"k1", "k2", "k4"},
               {{Int64(1), Int64(1), Int64(1)}});
  // Update is successful when columns dependent on the key column are provided.
  m.AddWriteOp(MutationOpType::kUpdate, "test_table", {"k1", "k2", "k4"},
               {{Int64(1), Int64(1), Int64(50)}});

  // Commit the transaction.
  auto txn1 = CreateReadWriteTransaction();
  GOOGLESQL_EXPECT_OK(txn1->Write(m));
  GOOGLESQL_EXPECT_OK(txn1->Commit());

  // Verify the values.
  auto txn2 = CreateReadWriteTransaction();
  EXPECT_THAT(ReadAll(txn2.get(), {"k1", "k2", "k3", "k4"}),
              IsOkAndHoldsRows({{Int64(1), Int64(1), Int64(1), Int64(50)}}));
  GOOGLESQL_EXPECT_OK(txn2->Commit());

  Mutation m2;
  m2.AddWriteOp(MutationOpType::kUpdate, "test_table", {"k1", "k4"},
                {{Int64(1), Int64(100)}});

  // Value of generated primary key column must be explicitly specified or
  // all the dependent columns of gpk columns should be specified.
  auto txn3 = CreateReadWriteTransaction();
  EXPECT_THAT(
      txn3->Write(m2),
      googlesql_base::testing::StatusIs(
          absl::StatusCode::kFailedPrecondition,
          testing::HasSubstr("The value of generated primary key column "
                             "`test_table.k3` must be explicitly specified")));
}

TEST_F(GeneratedPrimaryKeyTransactionTest, FailsUpdateAsRowDoesntExist) {
  Mutation m;
  m.AddWriteOp(MutationOpType::kInsert, "test_table", {"k1", "k2", "k4"},
               {{Int64(1), Int64(1), Int64(1)}});

  // Commit the transaction.
  auto txn1 = CreateReadWriteTransaction();
  GOOGLESQL_EXPECT_OK(txn1->Write(m));
  GOOGLESQL_EXPECT_OK(txn1->Commit());

  // Verify the values.
  auto txn2 = CreateReadWriteTransaction();
  EXPECT_THAT(ReadAll(txn2.get(), {"k1", "k2", "k3", "k4"}),
              IsOkAndHoldsRows({{Int64(1), Int64(1), Int64(1), Int64(1)}}));
  GOOGLESQL_EXPECT_OK(txn2->Commit());

  Mutation m2;
  m2.AddWriteOp(MutationOpType::kUpdate, "test_table", {"k1", "k3", "k4"},
                {{Int64(1), Int64(2), Int64(100)}});

  // Row to update not is found.
  auto txn3 = CreateReadWriteTransaction();
  EXPECT_THAT(txn3->Write(m2),
              googlesql_base::testing::StatusIs(
                  absl::StatusCode::kNotFound,
                  testing::HasSubstr("Row {Int64(1), Int64(2)} not found.")));
}

TEST_F(GeneratedPrimaryKeyTransactionTest,
       FailsInsertOrUpdateWhenSpecifyingGpk) {
  Mutation m;
  m.AddWriteOp(MutationOpType::kInsert, "test_table", {"k1", "k2", "k4"},
               {{Int64(1), Int64(1), Int64(1)}});

  // Commit the transaction.
  auto txn1 = CreateReadWriteTransaction();
  GOOGLESQL_EXPECT_OK(txn1->Write(m));
  GOOGLESQL_EXPECT_OK(txn1->Commit());

  // Verify the values.
  auto txn2 = CreateReadWriteTransaction();
  EXPECT_THAT(ReadAll(txn2.get(), {"k1", "k2", "k3", "k4"}),
              IsOkAndHoldsRows({{Int64(1), Int64(1), Int64(1), Int64(1)}}));
  GOOGLESQL_EXPECT_OK(txn2->Commit());

  Mutation m2;
  m2.AddWriteOp(MutationOpType::kInsertOrUpdate, "test_table",
                {"k1", "k2", "k3", "k4"},
                {{Int64(1), Int64(1), Int64(1), Int64(100)}});

  // Value of generated primary key column cannot be specified except in
  // update operations.
  auto txn3 = CreateReadWriteTransaction();
  EXPECT_THAT(txn3->Write(m2),
              googlesql_base::testing::StatusIs(
                  absl::StatusCode::kFailedPrecondition,
                  testing::HasSubstr(
                      "Cannot write into generated column `test_table.k3`.")));
}

TEST_F(GeneratedPrimaryKeyTransactionTest, DeleteMutations) {
  Mutation m;
  m.AddWriteOp(
      MutationOpType::kInsert, "test_table", {"k1", "k2", "k4"},
      {{Int64(1), Int64(1), Int64(1)}, {Int64(3), Int64(3), Int64(3)}});
  m.AddWriteOp(MutationOpType::kInsert, "test_table", {"k1", "k2", "k4"},
               {{Int64(1), Int64(2), Int64(2)}});

  // Commit the transaction.
  auto txn1 = CreateReadWriteTransaction();
  GOOGLESQL_ASSERT_OK(txn1->Write(m));
  GOOGLESQL_ASSERT_OK(txn1->Commit());

  // Verify the values.
  auto txn2 = CreateReadWriteTransaction();
  ASSERT_THAT(ReadAll(txn2.get(), {"k1", "k2", "k3", "k4"}),
              IsOkAndHoldsRows({{Int64(1), Int64(1), Int64(1), Int64(1)},
                                {Int64(1), Int64(2), Int64(2), Int64(2)},
                                {Int64(3), Int64(3), Int64(3), Int64(3)}}));
  GOOGLESQL_ASSERT_OK(txn2->Commit());

  Mutation m2;
  m2.AddDeleteOp("test_table", KeySet(Key({Int64(1), Int64(1)})));
  auto txn3 = CreateReadWriteTransaction();
  GOOGLESQL_EXPECT_OK(txn3->Write(m2));
  GOOGLESQL_EXPECT_OK(txn3->Commit());

  auto txn4 = CreateReadWriteTransaction();
  EXPECT_THAT(ReadAll(txn4.get(), {"k1", "k2", "k3", "k4"}),
              IsOkAndHoldsRows({{Int64(1), Int64(2), Int64(2), Int64(2)},
                                {Int64(3), Int64(3), Int64(3), Int64(3)}}));
}

TEST_F(GeneratedPrimaryKeyTransactionTest,
       InsertMutationsTableWithDependentDefaultColumn) {
  // This test checks if the a gpk column depending on a default column having
  // default value works fine.
  auto schema = test::CreateSchemaFromDDL(
                    {
                        R"sql(
                        CREATE TABLE new_table (
                          id INT64 DEFAULT (1),
                          gen_id INT64 AS (id+1) STORED,
                          value INT64
                        ) PRIMARY KEY (id, gen_id)
                      )sql"},
                    type_factory_.get())
                    .value();
  FunctionCatalog function_catalog(
      type_factory_.get(),
      /*catalog_name=*/kCloudSpannerEmulatorFunctionCatalogName,
      /*latest_schema=*/schema.get());
  GOOGLESQL_ASSERT_OK(versioned_catalog_->AddSchema(clock_.Now(), std::move(schema)));
  action_manager_->AddActionsForSchema(versioned_catalog_->GetLatestSchema(),
                                       &function_catalog, type_factory_.get());

  auto txn = CreateReadWriteTransaction();
  ASSERT_NE(txn->schema()->FindTable("new_table"), nullptr);
  Mutation m;
  m.AddWriteOp(MutationOpType::kInsert, "new_table", {"value"}, {{Int64(1)}});
  // Commit the transaction.
  GOOGLESQL_ASSERT_OK(txn->Write(m));
  GOOGLESQL_ASSERT_OK(txn->Commit());

  // Verify the values.
  EXPECT_THAT(ReadAll(CreateReadWriteTransaction().get(),
                      {"id", "gen_id", "value"}, "new_table"),
              IsOkAndHoldsRows({{Int64(1), Int64(2), Int64(1)}}));
}

}  // namespace
}  // namespace backend
}  // namespace emulator
}  // namespace spanner
}  // namespace google

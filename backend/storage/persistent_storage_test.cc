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

#include "backend/storage/persistent_storage.h"

#include <atomic>
#include <filesystem>
#include <memory>
#include <string>
#include <thread>
#include <vector>

#include "gmock/gmock.h"
#include "gtest/gtest.h"
#include "zetasql/base/testing/status_matchers.h"
#include "tests/common/proto_matchers.h"
#include "absl/status/status.h"
#include "absl/strings/string_view.h"
#include "absl/time/clock.h"
#include "absl/time/time.h"
#include "backend/datamodel/key_range.h"
#include "backend/storage/iterator.h"
#include "zetasql/public/json_value.h"
#include "zetasql/public/numeric_value.h"
#include "zetasql/public/types/type.h"

namespace google {
namespace spanner {
namespace emulator {
namespace backend {
namespace {

using zetasql::values::Bool;
using zetasql::values::Double;
using zetasql::values::Int64;
using zetasql::values::Null;
using zetasql::values::Numeric;
using zetasql::values::String;

// Returns a unique temp directory path under TEST_TMPDIR (or /tmp).
std::string MakeTempDir(const std::string& suffix) {
  const char* test_tmpdir = std::getenv("TEST_TMPDIR");
  std::string base = test_tmpdir ? test_tmpdir : "/tmp";
  std::string path = base + "/persistent_storage_test_" + suffix + "_" +
                     std::to_string(getpid());
  std::filesystem::remove_all(path);
  return path;
}

class PersistentStorageTest : public testing::Test {
 protected:
  void SetUp() override {
    data_dir_ = MakeTempDir("basic");
    // Create the directory so Create() succeeds for basic tests.
    std::filesystem::create_directories(data_dir_);
    auto storage_or = PersistentStorage::Create(data_dir_);
    ASSERT_TRUE(storage_or.ok()) << storage_or.status();
    storage_ = std::move(*storage_or);
  }

  void TearDown() override {
    storage_.reset();
    std::filesystem::remove_all(data_dir_);
  }

  const TableID kTableId0 = "test_table:0";
  const TableID kTableId1 = "test_table:1";
  const ColumnID kColumnID = "test_column:0";
  const KeyRange kKeyRange0To5 =
      KeyRange::ClosedOpen(Key({Int64(0)}), Key({Int64(5)}));

  std::string data_dir_;
  std::unique_ptr<PersistentStorage> storage_;
  std::unique_ptr<StorageIterator> itr_;
};

// ---------------------------------------------------------------------------
// Directory creation tests — these catch the mkdir bug.
// ---------------------------------------------------------------------------

// This is the exact scenario that triggered the bug: the storage path includes
// nested directories that don't exist yet (e.g., {data_dir}/{db_id}/storage/).
TEST(PersistentStorageCreateTest, CreatesNestedParentDirectories) {
  std::string base = MakeTempDir("mkdir");
  // Simulate the path database.cc builds: {data_dir}/{database_id}/storage
  std::string nested_path = base + "/my-database-1234/storage";

  // The parent directories should NOT exist yet.
  ASSERT_FALSE(std::filesystem::exists(base));

  auto storage_or = PersistentStorage::Create(nested_path);
  ASSERT_TRUE(storage_or.ok()) << storage_or.status();

  // Verify the directories were created.
  EXPECT_TRUE(std::filesystem::exists(nested_path));
  EXPECT_TRUE(std::filesystem::is_directory(nested_path));

  // Verify LevelDB files exist in the directory.
  bool has_files = false;
  for (const auto& entry : std::filesystem::directory_iterator(nested_path)) {
    has_files = true;
    break;
  }
  EXPECT_TRUE(has_files) << "LevelDB should have created files in "
                         << nested_path;

  // Clean up.
  storage_or->reset();
  std::filesystem::remove_all(base);
}

TEST(PersistentStorageCreateTest, CreatesDeeplyNestedDirectories) {
  std::string base = MakeTempDir("deep");
  std::string deep_path = base + "/a/b/c/d/e/storage";

  ASSERT_FALSE(std::filesystem::exists(base));

  auto storage_or = PersistentStorage::Create(deep_path);
  ASSERT_TRUE(storage_or.ok()) << storage_or.status();
  EXPECT_TRUE(std::filesystem::exists(deep_path));

  storage_or->reset();
  std::filesystem::remove_all(base);
}

TEST(PersistentStorageCreateTest, SucceedsWhenDirectoryAlreadyExists) {
  std::string base = MakeTempDir("existing");
  std::filesystem::create_directories(base);

  auto storage_or = PersistentStorage::Create(base);
  ASSERT_TRUE(storage_or.ok()) << storage_or.status();

  storage_or->reset();
  std::filesystem::remove_all(base);
}

// ---------------------------------------------------------------------------
// Open, close, and reopen — data persistence across restarts.
// ---------------------------------------------------------------------------

TEST(PersistentStorageCreateTest, DataPersistsAcrossCloseAndReopen) {
  std::string base = MakeTempDir("persist");
  std::string path = base + "/db/storage";
  const TableID table_id = "persist_table:0";
  const ColumnID col_id = "col:0";
  absl::Time t0 = absl::Now();

  // Open, write, close.
  {
    auto storage_or = PersistentStorage::Create(path);
    ASSERT_TRUE(storage_or.ok()) << storage_or.status();
    ZETASQL_ASSERT_OK((*storage_or)->Write(t0, table_id, Key({Int64(42)}),
                                   {col_id}, {String("hello")}));
  }  // storage destroyed here, LevelDB closed.

  // Reopen and verify data survived.
  {
    auto storage_or = PersistentStorage::Create(path);
    ASSERT_TRUE(storage_or.ok()) << storage_or.status();
    std::vector<zetasql::Value> values;
    ZETASQL_ASSERT_OK((*storage_or)->Lookup(t0, table_id, Key({Int64(42)}),
                                    {col_id}, &values));
    EXPECT_THAT(values, testing::ElementsAre(String("hello")));
  }

  std::filesystem::remove_all(base);
}

TEST(PersistentStorageCreateTest, ArrayValuesPersistAcrossCloseAndReopen) {
  std::string base = MakeTempDir("array_persist");
  std::string path = base + "/db/storage";
  const TableID table_id = "array_table:0";
  const ColumnID int_array_col = "int_array:0";
  const ColumnID string_array_col = "string_array:0";
  const ColumnID double_array_col = "double_array:0";
  const ColumnID bool_array_col = "bool_array:0";
  const ColumnID numeric_array_col = "numeric_array:0";
  const ColumnID json_array_col = "json_array:0";
  const ColumnID empty_array_col = "empty_array:0";
  const ColumnID null_array_col = "null_array:0";
  absl::Time t0 = absl::Now();

  auto numeric = [](absl::string_view value) {
    return Numeric(zetasql::NumericValue::FromStringStrict(value).value());
  };
  auto json = [](absl::string_view value) {
    return zetasql::values::Json(
        zetasql::JSONValue::ParseJSONString(value).value());
  };

  zetasql::Value int_array =
      zetasql::values::Array(zetasql::types::Int64ArrayType(),
                             {Int64(1), Null(zetasql::types::Int64Type()),
                              Int64(3)});
  zetasql::Value string_array =
      zetasql::values::Array(zetasql::types::StringArrayType(),
                             {String("tag1"), String("tag2")});
  zetasql::Value double_array =
      zetasql::values::Array(zetasql::types::DoubleArrayType(),
                             {Double(1.25), Double(2.5)});
  zetasql::Value bool_array =
      zetasql::values::Array(zetasql::types::BoolArrayType(),
                             {Bool(true), Bool(false)});
  zetasql::Value numeric_array =
      zetasql::values::Array(zetasql::types::NumericArrayType(),
                             {numeric("123.456"), numeric("-0.000001")});
  zetasql::Value json_array =
      zetasql::values::Array(zetasql::types::JsonArrayType(),
                             {json(R"({"a":1})"), json(R"(["b",2])")});
  zetasql::Value empty_array =
      zetasql::Value::EmptyArray(zetasql::types::StringArrayType());
  zetasql::Value null_array =
      zetasql::values::Null(zetasql::types::StringArrayType());

  {
    auto storage_or = PersistentStorage::Create(path);
    ASSERT_TRUE(storage_or.ok()) << storage_or.status();
    ZETASQL_ASSERT_OK((*storage_or)->Write(
        t0, table_id, Key({Int64(42)}),
        {int_array_col, string_array_col, double_array_col, bool_array_col,
         numeric_array_col, json_array_col, empty_array_col, null_array_col},
        {int_array, string_array, double_array, bool_array, numeric_array,
         json_array, empty_array, null_array}));
  }

  {
    auto storage_or = PersistentStorage::Create(path);
    ASSERT_TRUE(storage_or.ok()) << storage_or.status();
    std::vector<zetasql::Value> values;
    ZETASQL_ASSERT_OK((*storage_or)->Lookup(
        t0, table_id, Key({Int64(42)}),
        {int_array_col, string_array_col, double_array_col, bool_array_col,
         numeric_array_col, json_array_col, empty_array_col, null_array_col},
        &values));
    EXPECT_THAT(values,
                testing::ElementsAre(int_array, string_array, double_array,
                                     bool_array, numeric_array, json_array,
                                     empty_array, null_array));
  }

  std::filesystem::remove_all(base);
}

// ---------------------------------------------------------------------------
// Basic CRUD — mirrors a subset of in_memory_storage_test.cc to ensure
// PersistentStorage behaves identically.
// ---------------------------------------------------------------------------

TEST_F(PersistentStorageTest, LookupByTable) {
  absl::Time t0 = absl::Now();

  ZETASQL_EXPECT_OK(storage_->Write(t0, kTableId0, Key({Int64(1)}), {kColumnID},
                            {String("value-1")}));
  ZETASQL_EXPECT_OK(storage_->Write(t0, kTableId1, Key({Int64(10)}), {kColumnID},
                            {String("value-10")}));

  std::vector<zetasql::Value> values;
  ZETASQL_EXPECT_OK(
      storage_->Lookup(t0, kTableId0, Key({Int64(1)}), {kColumnID}, &values));
  EXPECT_THAT(values, testing::ElementsAre(String("value-1")));
  ZETASQL_EXPECT_OK(
      storage_->Lookup(t0, kTableId1, Key({Int64(10)}), {kColumnID}, &values));
  EXPECT_THAT(values, testing::ElementsAre(String("value-10")));
}

TEST_F(PersistentStorageTest, ReadRangeFromSingleTable) {
  absl::Time t0 = absl::Now();

  for (int i = 0; i < 5; ++i) {
    ZETASQL_EXPECT_OK(storage_->Write(t0, kTableId0, Key({Int64(i)}), {kColumnID},
                              {String(absl::StrCat("value-", i))}));
  }

  ZETASQL_EXPECT_OK(
      storage_->Read(t0, kTableId0, kKeyRange0To5, {kColumnID}, &itr_));
  for (int i = 0; i < 5; ++i) {
    EXPECT_TRUE(itr_->Next());
    EXPECT_EQ(itr_->NumColumns(), 1);
    EXPECT_EQ(itr_->ColumnValue(0), String(absl::StrCat("value-", i)));
  }
  EXPECT_FALSE(itr_->Next());
}

TEST_F(PersistentStorageTest, LookupByTimestamp) {
  absl::Time write_ts = absl::Now();

  ZETASQL_EXPECT_OK(storage_->Write(write_ts, kTableId0, Key({Int64(1)}), {kColumnID},
                            {String("value-1")}));

  std::vector<zetasql::Value> values;
  ZETASQL_EXPECT_OK(storage_->Lookup(write_ts, kTableId0, Key({Int64(1)}),
                             {kColumnID}, &values));
  EXPECT_THAT(values, testing::ElementsAre(String("value-1")));

  // Future timestamp should still find the value.
  absl::Time future_ts = write_ts + absl::Nanoseconds(24);
  ZETASQL_EXPECT_OK(storage_->Lookup(future_ts, kTableId0, Key({Int64(1)}),
                             {kColumnID}, &values));
  EXPECT_THAT(values, testing::ElementsAre(String("value-1")));

  // Before write timestamp should not find the value.
  // Use microsecond delta since PersistentStorage encodes timestamps in
  // microsecond precision (absl::ToUnixMicros).
  absl::Time before_ts = write_ts - absl::Microseconds(1);
  EXPECT_THAT(storage_->Lookup(before_ts, kTableId0, Key({Int64(1)}),
                               {kColumnID}, &values),
              zetasql_base::testing::StatusIs(absl::StatusCode::kNotFound));
}

TEST_F(PersistentStorageTest, LookupMissingKeyReturnsNotFound) {
  absl::Time t0 = absl::Now();

  ZETASQL_EXPECT_OK(storage_->Write(t0, kTableId0, Key({Int64(1)}), {kColumnID},
                            {String("value-1")}));

  std::vector<zetasql::Value> values;
  EXPECT_THAT(
      storage_->Lookup(t0, kTableId0, Key({Int64(100)}), {kColumnID}, &values),
      zetasql_base::testing::StatusIs(absl::StatusCode::kNotFound));
}

TEST_F(PersistentStorageTest, DeleteAndLookup) {
  absl::Time write_ts = absl::Now();
  absl::Time delete_ts = write_ts + absl::Seconds(1);
  absl::Time after_delete_ts = delete_ts + absl::Seconds(1);
  Key key({Int64(1)});

  ZETASQL_EXPECT_OK(storage_->Write(write_ts, kTableId0, key, {kColumnID},
                            {String("value-1")}));
  ZETASQL_EXPECT_OK(storage_->Delete(delete_ts, kTableId0, KeyRange::Point(key)));

  // Before delete: still visible.
  std::vector<zetasql::Value> values;
  ZETASQL_EXPECT_OK(
      storage_->Lookup(write_ts, kTableId0, key, {kColumnID}, &values));
  EXPECT_THAT(values, testing::ElementsAre(String("value-1")));

  // After delete: not found.
  EXPECT_THAT(
      storage_->Lookup(after_delete_ts, kTableId0, key, {kColumnID}, &values),
      zetasql_base::testing::StatusIs(absl::StatusCode::kNotFound));
}

TEST_F(PersistentStorageTest, PointDeleteDoesNotDeleteOtherRows) {
  absl::Time write_ts = absl::Now();
  absl::Time delete_ts = write_ts + absl::Seconds(1);
  absl::Time after_delete_ts = delete_ts + absl::Seconds(1);

  // Write three rows.
  Key key1({Int64(1)});
  Key key2({Int64(2)});
  Key key3({Int64(3)});
  ZETASQL_EXPECT_OK(storage_->Write(write_ts, kTableId0, key1, {kColumnID},
                            {String("val-1")}));
  ZETASQL_EXPECT_OK(storage_->Write(write_ts, kTableId0, key2, {kColumnID},
                            {String("val-2")}));
  ZETASQL_EXPECT_OK(storage_->Write(write_ts, kTableId0, key3, {kColumnID},
                            {String("val-3")}));

  // Delete only row 1.
  ZETASQL_EXPECT_OK(
      storage_->Delete(delete_ts, kTableId0, KeyRange::Point(key1)));

  // Row 1 should be gone.
  std::vector<zetasql::Value> values;
  EXPECT_THAT(
      storage_->Lookup(after_delete_ts, kTableId0, key1, {kColumnID}, &values),
      zetasql_base::testing::StatusIs(absl::StatusCode::kNotFound));

  // Rows 2 and 3 must still exist.
  ZETASQL_EXPECT_OK(
      storage_->Lookup(after_delete_ts, kTableId0, key2, {kColumnID}, &values));
  EXPECT_THAT(values, testing::ElementsAre(String("val-2")));
  ZETASQL_EXPECT_OK(
      storage_->Lookup(after_delete_ts, kTableId0, key3, {kColumnID}, &values));
  EXPECT_THAT(values, testing::ElementsAre(String("val-3")));

  // Full scan should return exactly 2 rows.
  ZETASQL_EXPECT_OK(storage_->Read(after_delete_ts, kTableId0, kKeyRange0To5,
                            {kColumnID}, &itr_));
  int row_count = 0;
  while (itr_->Next()) row_count++;
  EXPECT_EQ(row_count, 2);
}

TEST_F(PersistentStorageTest, SnapshotRead) {
  absl::Time write_ts = absl::Now();
  absl::Time snapshot_ts = write_ts + absl::Seconds(1);
  absl::Time second_write_ts = snapshot_ts + absl::Seconds(1);
  Key key({Int64(1)});

  ZETASQL_EXPECT_OK(storage_->Write(write_ts, kTableId0, key, {kColumnID},
                            {String("value-old")}));
  ZETASQL_EXPECT_OK(storage_->Write(second_write_ts, kTableId0, key, {kColumnID},
                            {String("value-new")}));

  // Snapshot read at the point between writes should return old value.
  ZETASQL_EXPECT_OK(
      storage_->Read(snapshot_ts, kTableId0, kKeyRange0To5, {kColumnID}, &itr_));
  EXPECT_TRUE(itr_->Next());
  EXPECT_EQ(itr_->ColumnValue(0), String("value-old"));
}

TEST_F(PersistentStorageTest, LookupInvalidTableReturnsNotFound) {
  absl::Time t0 = absl::Now();

  std::vector<zetasql::Value> values;
  EXPECT_THAT(
      storage_->Lookup(t0, kTableId0, Key({Int64(1)}), {kColumnID}, &values),
      zetasql_base::testing::StatusIs(absl::StatusCode::kNotFound));
}

TEST_F(PersistentStorageTest, ReadEmptyKeyRangeReturnsEmptyItr) {
  absl::Time t0 = absl::Now();

  ZETASQL_EXPECT_OK(storage_->Write(t0, kTableId0, Key({Int64(1)}), {kColumnID},
                            {String("value-1")}));
  ZETASQL_EXPECT_OK(
      storage_->Read(t0, kTableId0, KeyRange::Empty(), {kColumnID}, &itr_));
  EXPECT_FALSE(itr_->Next());
}

TEST_F(PersistentStorageTest, DroppedTablesAreRemovedAfterRetentionPeriod) {
  absl::Time t0 = absl::Now();

  ZETASQL_EXPECT_OK(storage_->Write(t0, kTableId0, Key({Int64(1)}), {kColumnID},
                            {String("value-1")}));
  ZETASQL_EXPECT_OK(storage_->Write(t0, kTableId1, Key({Int64(10)}), {kColumnID},
                            {String("value-10")}));

  storage_->MarkDroppedTable(t0, kTableId0);
  storage_->CleanUpDeletedTables(t0 + absl::Hours(1) + absl::Seconds(1));

  std::vector<zetasql::Value> values;
  EXPECT_THAT(
      storage_->Lookup(t0, kTableId0, Key({Int64(1)}), {kColumnID}, &values),
      zetasql_base::testing::StatusIs(absl::StatusCode::kNotFound));
  ZETASQL_EXPECT_OK(
      storage_->Lookup(t0, kTableId1, Key({Int64(10)}), {kColumnID}, &values));
}

// ---------------------------------------------------------------------------
// Multiple databases — simulates what database.cc does for each db.
// ---------------------------------------------------------------------------

TEST(PersistentStorageCreateTest, MultipleDbsUnderSameDataDir) {
  std::string base = MakeTempDir("multidb");
  std::string path1 = base + "/db-aaa/storage";
  std::string path2 = base + "/db-bbb/storage";
  const TableID table = "t:0";
  const ColumnID col = "c:0";
  absl::Time t0 = absl::Now();

  ASSERT_FALSE(std::filesystem::exists(base));

  auto s1 = PersistentStorage::Create(path1);
  ASSERT_TRUE(s1.ok()) << s1.status();
  auto s2 = PersistentStorage::Create(path2);
  ASSERT_TRUE(s2.ok()) << s2.status();

  ZETASQL_ASSERT_OK((*s1)->Write(t0, table, Key({Int64(1)}), {col},
                          {String("from-db1")}));
  ZETASQL_ASSERT_OK((*s2)->Write(t0, table, Key({Int64(1)}), {col},
                          {String("from-db2")}));

  std::vector<zetasql::Value> vals;
  ZETASQL_ASSERT_OK((*s1)->Lookup(t0, table, Key({Int64(1)}), {col}, &vals));
  EXPECT_THAT(vals, testing::ElementsAre(String("from-db1")));
  ZETASQL_ASSERT_OK((*s2)->Lookup(t0, table, Key({Int64(1)}), {col}, &vals));
  EXPECT_THAT(vals, testing::ElementsAre(String("from-db2")));

  s1->reset();
  s2->reset();
  std::filesystem::remove_all(base);
}

// ---------------------------------------------------------------------------
// WriteQueue tests (tasks 4.1–4.3)
// ---------------------------------------------------------------------------

TEST_F(PersistentStorageTest, WriteQueue_SequentialSubmit) {
  absl::Time t0 = absl::Now();

  // Submit 3 batches sequentially.
  ZETASQL_ASSERT_OK(storage_->Write(t0, kTableId0, Key({Int64(1)}), {kColumnID},
                            {String("batch-1")}));
  ZETASQL_ASSERT_OK(storage_->Write(t0, kTableId0, Key({Int64(2)}), {kColumnID},
                            {String("batch-2")}));
  ZETASQL_ASSERT_OK(storage_->Write(t0, kTableId0, Key({Int64(3)}), {kColumnID},
                            {String("batch-3")}));

  // Verify all data is readable.
  ZETASQL_ASSERT_OK(storage_->Read(t0, kTableId0, kKeyRange0To5, {kColumnID}, &itr_));
  int count = 0;
  while (itr_->Next()) count++;
  EXPECT_EQ(count, 3);
}

TEST_F(PersistentStorageTest, WriteQueue_ConcurrentSubmit) {
  absl::Time t0 = absl::Now();
  std::vector<std::thread> threads;
  std::atomic<int> errors{0};

  for (int i = 0; i < 4; ++i) {
    threads.emplace_back([this, t0, i, &errors]() {
      auto status = storage_->Write(t0, kTableId0, Key({Int64(i)}),
                                    {kColumnID},
                                    {String(absl::StrCat("val-", i))});
      if (!status.ok()) {
        errors++;
      }
    });
  }

  for (auto& t : threads) t.join();

  EXPECT_EQ(errors.load(), 0);

  // Verify all 4 rows are readable with no interleaving.
  ZETASQL_ASSERT_OK(storage_->Read(t0, kTableId0, kKeyRange0To5, {kColumnID}, &itr_));
  int count = 0;
  while (itr_->Next()) {
    count++;
    // Each row should have exactly 1 column with the expected value.
    EXPECT_EQ(itr_->NumColumns(), 1);
    EXPECT_TRUE(itr_->ColumnValue(0).is_valid());
  }
  EXPECT_EQ(count, 4);
}

TEST_F(PersistentStorageTest, WriteQueue_Shutdown) {
  absl::Time t0 = absl::Now();

  // Submit a batch.
  ZETASQL_ASSERT_OK(storage_->Write(t0, kTableId0, Key({Int64(1)}), {kColumnID},
                            {String("shutdown-test")}));

  // Shutdown by destroying storage (TearDown will handle cleanup).
  // The destructor calls write_queue_.Shutdown() which processes remaining
  // batches and joins the worker thread.
  storage_.reset();

  // Reopen and verify data persisted (batch was committed before shutdown).
  auto storage_or = PersistentStorage::Create(data_dir_);
  ASSERT_TRUE(storage_or.ok()) << storage_or.status();
  storage_ = std::move(*storage_or);  // Let TearDown clean up.

  std::vector<zetasql::Value> values;
  ZETASQL_ASSERT_OK(storage_->Lookup(t0, kTableId0, Key({Int64(1)}),
                             {kColumnID}, &values));
  EXPECT_THAT(values, testing::ElementsAre(String("shutdown-test")));
}

// ---------------------------------------------------------------------------
// Snapshot read tests (tasks 4.4–4.6)
// ---------------------------------------------------------------------------

TEST_F(PersistentStorageTest, Snapshot_ReadDuringWrite) {
  absl::Time t0 = absl::Now();
  std::atomic<int> read_count{0};

  // Write concurrently with a read.
  std::thread writer([this, t0]() {
    ZETASQL_ASSERT_OK(storage_->Write(t0, kTableId0, Key({Int64(1)}),
                              {kColumnID}, {String("snapshot-test")}));
  });

  std::thread reader([this, t0, &read_count]() {
    auto local_itr = std::unique_ptr<StorageIterator>();
    ZETASQL_ASSERT_OK(storage_->Read(t0, kTableId0, kKeyRange0To5, {kColumnID},
                             &local_itr));
    while (local_itr->Next()) read_count++;
  });

  writer.join();
  reader.join();

  // read_count should be either 0 or 1 — either sees nothing or sees the
  // full committed row, never a partial row.
  EXPECT_THAT(read_count.load(), testing::AnyOf(0, 1));
}

TEST_F(PersistentStorageTest, Snapshot_MultipleConcurrentReads) {
  absl::Time t0 = absl::Now();

  // Pre-populate one row so reads have something to return.
  ZETASQL_ASSERT_OK(storage_->Write(t0, kTableId0, Key({Int64(1)}), {kColumnID},
                            {String("existing")}));

  const int kNumReaders = 10;
  std::atomic<int> reads_ok{0};
  std::atomic<int> writes_ok{0};
  std::atomic<int> total_rows{0};

  // One writer thread writes repeatedly.
  std::thread writer([this, t0, &writes_ok]() {
    for (int i = 0; i < 5; ++i) {
      auto s = storage_->Write(t0, kTableId0, Key({Int64(2)}),
                               {kColumnID}, {String(absl::StrCat("w", i))});
      if (s.ok()) writes_ok++;
    }
  });

  // Multiple reader threads read concurrently.
  std::vector<std::thread> readers;
  for (int i = 0; i < kNumReaders; ++i) {
    readers.emplace_back([this, t0, &reads_ok, &total_rows]() {
      auto local_itr = std::unique_ptr<StorageIterator>();
      auto s = storage_->Read(t0, kTableId0, kKeyRange0To5, {kColumnID},
                              &local_itr);
      if (s.ok()) {
        reads_ok++;
        int rows = 0;
        while (local_itr->Next()) rows++;
        total_rows += rows;
      }
    });
  }

  writer.join();
  for (auto& t : readers) t.join();

  EXPECT_EQ(writes_ok.load(), 5);
  EXPECT_EQ(reads_ok.load(), kNumReaders);
  // All reads should return consistent results — no crashes.
  EXPECT_GE(total_rows.load(), kNumReaders);  // At least 10 rows (1 existing × 10 reads).
}

TEST(PersistentStorageCreateTest, Snapshot_IteratorReleasesSnapshot) {
  std::string path = MakeTempDir("snapshot_release");
  const TableID table_id = "t:0";
  const ColumnID col_id = "c:0";
  absl::Time t0 = absl::Now();

  // Create storage, write data, read to create iterator with snapshot.
  {
    auto storage_or = PersistentStorage::Create(path);
    ASSERT_TRUE(storage_or.ok()) << storage_or.status();

    ZETASQL_ASSERT_OK((*storage_or)->Write(t0, table_id, Key({Int64(1)}),
                                   {col_id}, {String("val")}));

    {
      auto local_itr = std::unique_ptr<StorageIterator>();
      ZETASQL_ASSERT_OK((*storage_or)->Read(
          t0, table_id,
          KeyRange::ClosedOpen(Key({Int64(0)}), Key({Int64(5)})),
          {col_id}, &local_itr));
      // Iterator holds snapshot. Verify we can iterate.
      EXPECT_TRUE(local_itr->Next());
      EXPECT_EQ(local_itr->ColumnValue(0), String("val"));
    }
    // local_itr destroyed here — SnapshotOwningIterator releases the snapshot.
    // storage destroyed here — LevelDB handle closed without assertion failure.
  }

  // Reopen — LevelDB should open cleanly (no stale snapshot preventing
  // compaction/recovery).
  auto storage_or2 = PersistentStorage::Create(path);
  EXPECT_TRUE(storage_or2.ok()) << storage_or2.status();

  std::filesystem::remove_all(path);
}

}  // namespace
}  // namespace backend
}  // namespace emulator
}  // namespace spanner
}  // namespace google

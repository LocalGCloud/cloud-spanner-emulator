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

#include "backend/query/queryable_table.h"

#include <memory>
#include <optional>
#include <string>
#include <vector>

#include "googlesql/public/evaluator_table_iterator.h"
#include "googlesql/public/type.h"
#include "googlesql/public/value.h"
#include "gmock/gmock.h"
#include "gtest/gtest.h"
#include "googlesql/base/testing/status_matchers.h"
#include "absl/container/flat_hash_map.h"
#include "absl/status/status.h"
#include "backend/access/read.h"
#include "backend/datamodel/key_set.h"
#include "backend/query/catalog.h"
#include "backend/query/queryable_column.h"
#include "tests/common/row_cursor.h"
#include "tests/common/row_reader.h"
#include "tests/common/schema_constructor.h"

namespace google {
namespace spanner {
namespace emulator {
namespace backend {

namespace {

using testing::ElementsAre;

class QueryableTableTest : public testing::Test {
 public:
  const Schema* schema() { return schema_.get(); }
  RowReader* reader() { return &reader_; }

 private:
  googlesql::TypeFactory type_factory_;
  std::unique_ptr<const Schema> schema_ =
      test::CreateSchemaWithOneTable(&type_factory_);
  test::TestRowReader reader_{
      {{"test_table",
        {{"int64_col", "string_col"},
         {googlesql::types::Int64Type(), googlesql::types::StringType()},
         {{googlesql::values::Int64(42), googlesql::values::String("foo")}}}}}};
};

TEST_F(QueryableTableTest, FindColumnByName) {
  const auto* schema_table = schema()->FindTable("test_table");
  QueryableTable table{schema_table, reader()};
  const auto* column = table.FindColumnByName("string_col");
  EXPECT_NE(column, nullptr);
  const QueryableColumn* queryable_column =
      dynamic_cast<const QueryableColumn*>(column);
  EXPECT_NE(queryable_column, nullptr);
  EXPECT_EQ(queryable_column->wrapped_column(),
            schema_table->FindColumn("string_col"));
}

TEST_F(QueryableTableTest, PrimaryKey) {
  const auto* schema_table = schema()->FindTable("test_table");
  QueryableTable table{schema_table, reader()};
  ASSERT_TRUE(table.PrimaryKey().has_value());
  EXPECT_THAT(table.PrimaryKey().value(),
              ElementsAre(0 /* index of int64_col*/));
}

TEST_F(QueryableTableTest, CreateEvaluatorTableIteratorWithZeroColumns) {
  QueryableTable table{schema()->FindTable("test_table"), reader()};
  auto iterator =
      table.CreateEvaluatorTableIterator(/*column_idxs=*/{}).value();
  ASSERT_EQ(iterator->NumColumns(), 0);
  ASSERT_TRUE(iterator->NextRow());
  GOOGLESQL_ASSERT_OK(iterator->Status());
  ASSERT_FALSE(iterator->NextRow());
}

TEST_F(QueryableTableTest, CreateEvaluatorTableIteratorWithTheSecondColumn) {
  QueryableTable table{schema()->FindTable("test_table"), reader()};
  auto iterator =
      table.CreateEvaluatorTableIterator(/*column_idxs=*/{1}).value();
  ASSERT_EQ(iterator->NumColumns(), 1);
  EXPECT_EQ(iterator->GetColumnName(0), "string_col");
  EXPECT_TRUE(iterator->GetColumnType(0)->IsString());
  ASSERT_TRUE(iterator->NextRow());
  GOOGLESQL_ASSERT_OK(iterator->Status());
  EXPECT_EQ(iterator->GetValue(0).string_value(), "foo");
  ASSERT_FALSE(iterator->NextRow());
}

TEST_F(QueryableTableTest, CreateEvaluatorTableIteratorWithAllColumns) {
  QueryableTable table{schema()->FindTable("test_table"), reader()};
  auto iterator =
      table.CreateEvaluatorTableIterator(/*column_idxs=*/{0, 1}).value();
  ASSERT_EQ(iterator->NumColumns(), 2);
  EXPECT_EQ(iterator->GetColumnName(0), "int64_col");
  EXPECT_EQ(iterator->GetColumnName(1), "string_col");
  EXPECT_TRUE(iterator->GetColumnType(0)->IsInt64());
  EXPECT_TRUE(iterator->GetColumnType(1)->IsString());
  ASSERT_TRUE(iterator->NextRow());
  GOOGLESQL_ASSERT_OK(iterator->Status());
  EXPECT_EQ(iterator->GetValue(0).int64_value(), 42);
  EXPECT_EQ(iterator->GetValue(1).string_value(), "foo");
  ASSERT_FALSE(iterator->NextRow());
}

class RecordingReader : public RowReader {
 public:
  explicit RecordingReader(RowReader* delegate) : delegate_(delegate) {}

  absl::Status Read(const ReadArg& arg,
                    std::unique_ptr<RowCursor>* cursor) override {
    reads.push_back(arg);
    return delegate_->Read(arg, cursor);
  }

  std::vector<ReadArg> reads;

 private:
  RowReader* delegate_;
};

TEST_F(QueryableTableTest, ForUpdateLocksExactKeyAfterFilter) {
  bool select_for_update = true;
  RecordingReader recording_reader(reader());
  QueryableTable table{schema()->FindTable("test_table"),
                       &recording_reader, std::nullopt, nullptr, nullptr,
                       false, &select_for_update};
  auto iterator =
      table.CreateEvaluatorTableIterator(/*column_idxs=*/{0, 1}).value();
  EXPECT_TRUE(recording_reader.reads.empty());

  absl::flat_hash_map<int, std::unique_ptr<googlesql::ColumnFilter>> filters;
  auto key = googlesql::values::Int64(42);
  filters.emplace(0, std::make_unique<googlesql::ColumnFilter>(key, key));
  GOOGLESQL_ASSERT_OK(iterator->SetColumnFilterMap(std::move(filters)));
  ASSERT_TRUE(iterator->NextRow());
  GOOGLESQL_ASSERT_OK(iterator->Status());
  ASSERT_EQ(recording_reader.reads.size(), 1);
  EXPECT_TRUE(recording_reader.reads.front().lock_scanned_ranges_exclusive);
  ASSERT_EQ(recording_reader.reads.front().key_set.keys().size(), 1);
  EXPECT_EQ(recording_reader.reads.front()
                .key_set.keys().front().ColumnValue(0).int64_value(),
            42);
  EXPECT_TRUE(recording_reader.reads.front().key_set.ranges().empty());
}

TEST_F(QueryableTableTest, ForUpdateBroadFilterLocksFullRange) {
  bool select_for_update = true;
  RecordingReader recording_reader(reader());
  QueryableTable table{schema()->FindTable("test_table"),
                       &recording_reader, std::nullopt, nullptr, nullptr,
                       false, &select_for_update};
  auto iterator =
      table.CreateEvaluatorTableIterator(/*column_idxs=*/{0, 1}).value();

  absl::flat_hash_map<int, std::unique_ptr<googlesql::ColumnFilter>> filters;
  filters.emplace(0, std::make_unique<googlesql::ColumnFilter>(
                         googlesql::Value(), googlesql::values::Int64(42)));
  GOOGLESQL_ASSERT_OK(iterator->SetColumnFilterMap(std::move(filters)));
  ASSERT_TRUE(iterator->NextRow());
  ASSERT_EQ(recording_reader.reads.size(), 1);
  EXPECT_TRUE(recording_reader.reads.front().lock_scanned_ranges_exclusive);
  EXPECT_EQ(recording_reader.reads.front().key_set.DebugString(),
            KeySet::All().DebugString());
}

}  // namespace

}  // namespace backend
}  // namespace emulator
}  // namespace spanner
}  // namespace google

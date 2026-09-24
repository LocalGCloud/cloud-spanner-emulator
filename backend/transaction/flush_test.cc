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

#include "backend/transaction/flush.h"

#include <chrono>
#include <cstdint>
#include <filesystem>
#include <memory>
#include <string>
#include <vector>

#include "gmock/gmock.h"
#include "gtest/gtest.h"
#include "googlesql/base/testing/status_matchers.h"
#include "tests/common/proto_matchers.h"
#include "absl/status/statusor.h"
#include "absl/strings/match.h"
#include "absl/strings/str_cat.h"
#include "absl/time/time.h"
#include "backend/actions/ops.h"
#include "backend/storage/in_memory_storage.h"
#include "backend/storage/persistent_storage.h"
#include "tests/common/schema_constructor.h"

namespace google {
namespace spanner {
namespace emulator {
namespace backend {
namespace {

using googlesql::values::Int64;
using googlesql::values::String;

class FlushTest : public testing::Test {
 public:
  FlushTest()
      : storage_(std::make_unique<InMemoryStorage>()),
        type_factory_(std::make_unique<googlesql::TypeFactory>()),
        schema_(test::CreateSchemaFromDDL(
                    {
                        R"(
                          CREATE TABLE TestTable (
                            Int64Col    INT64 NOT NULL,
                            StringCol   STRING(MAX),
                          ) PRIMARY KEY (Int64Col)
                        )"},
                    type_factory_.get())
                    .value()),
        table_(schema_->FindTable("TestTable")),
        int64_col_(table_->FindColumn("Int64Col")),
        string_col_(table_->FindColumn("StringCol")) {}

 protected:
  std::unique_ptr<InMemoryStorage> storage_;

  // The type factory must outlive the type objects that it has made.
  std::unique_ptr<googlesql::TypeFactory> type_factory_;
  std::unique_ptr<const Schema> schema_;

  // Constants
  const Table* table_;
  const Column* int64_col_;
  const Column* string_col_;

  // Helper functions to use in tests.
  absl::Status Write(absl::Time timestamp, const Key& key,
                     const ValueList& values) {
    return storage_->Write(timestamp, table_->id(), key,
                           {int64_col_->id(), string_col_->id()}, values);
  }

  absl::StatusOr<std::vector<ValueList>> ReadAll(absl::Time timestamp) {
    std::unique_ptr<StorageIterator> itr;
    GOOGLESQL_RETURN_IF_ERROR(storage_->Read(timestamp, table_->id(), KeyRange::All(),
                                   {int64_col_->id(), string_col_->id()},
                                   &itr));

    std::vector<ValueList> rows;
    while (itr->Next()) {
      rows.emplace_back();
      for (int i = 0; i < itr->NumColumns(); i++) {
        rows.back().push_back(itr->ColumnValue(i));
      }
    }
    return rows;
  }

  auto IsOkAndHoldsRows(const std::vector<ValueList>& rows) {
    return googlesql_base::testing::IsOkAndHolds(testing::ElementsAreArray(rows));
  }
};

TEST_F(FlushTest, CanFlushWriteOpsToStorage) {
  absl::Time t0 = absl::Now();

  // Insert - {1, "value"}, {2, "value"}
  GOOGLESQL_ASSERT_OK(Write(t0, Key({Int64(1)}), {Int64(1), String("value")}));
  GOOGLESQL_ASSERT_OK(Write(t0, Key({Int64(2)}), {Int64(2), String("value")}));

  // Make sure we can read back the rows written to base storage.
  EXPECT_THAT(ReadAll(t0), IsOkAndHoldsRows({{Int64(1), String("value")},
                                             {Int64(2), String("value")}}));

  // Update base storage by flushing write ops at a later timestamp.
  absl::Time t1 = t0 + absl::Seconds(1);

  // Insert - {3, "value"}
  InsertOp insert_op{table_,
                     Key({Int64(3)}),
                     {int64_col_, string_col_},
                     {Int64(3), String("value")}};

  // Update - {1, "value"} -> {1, "new-value"}
  UpdateOp update_op{
      table_, Key({Int64(1)}), {string_col_}, {String("new-value")}};

  // Delete - {2, "value"}
  DeleteOp delete_op{table_, Key({Int64(2)})};

  GOOGLESQL_ASSERT_OK(FlushWriteOpsToStorage({insert_op, update_op, delete_op},
                                   storage_.get(), t1));

  // Make sure reads on base storage at t1 reflect the flushed write ops.
  EXPECT_THAT(ReadAll(t1), IsOkAndHoldsRows({{Int64(1), String("new-value")},
                                             {Int64(3), String("value")}}));
}

// A commit that writes rows to two tables, one of which the storage refuses,
// must leave no row of the commit on disk.
TEST(PersistentFlushTest, PersistentCommitIsAllOrNothing) {
  const std::filesystem::path data_dir =
      std::filesystem::temp_directory_path() /
      absl::StrCat("flush-test-",
                   std::chrono::steady_clock::now().time_since_epoch().count());
  auto type_factory = std::make_unique<googlesql::TypeFactory>();
  std::unique_ptr<const Schema> schema =
      test::CreateSchemaFromDDL(
          {R"(CREATE TABLE Accepted (K INT64 NOT NULL) PRIMARY KEY (K))",
           R"(CREATE TABLE Refused (K INT64 NOT NULL) PRIMARY KEY (K))"},
          type_factory.get())
          .value();
  const Table* accepted = schema->FindTable("Accepted");
  const Table* refused = schema->FindTable("Refused");
  const Column* accepted_key = accepted->FindColumn("K");
  const Column* refused_key = refused->FindColumn("K");
  {
    GOOGLESQL_ASSERT_OK_AND_ASSIGN(std::unique_ptr<PersistentStorage> storage,
                                   PersistentStorage::Create(data_dir.string()));
    // PersistentStorage keys start with the table ID, length-prefixed with 4
    // big-endian bytes.
    const std::string refused_id = refused->id();
    std::string refused_prefix(4, '\0');
    refused_prefix[3] = static_cast<char>(refused_id.size());
    refused_prefix += refused_id;
    storage->SetWriteHookForTesting(
        [&](const std::vector<std::string>& keys) -> absl::Status {
          for (const std::string& key : keys) {
            if (absl::StartsWith(key, refused_prefix)) {
              return absl::UnavailableError("injected write failure");
            }
          }
          return absl::OkStatus();
        });

    const absl::Time t0 = absl::Now();
    std::vector<WriteOp> ops = {
        InsertOp{accepted, Key({Int64(1)}), {accepted_key}, {Int64(1)}},
        InsertOp{refused, Key({Int64(2)}), {refused_key}, {Int64(2)}},
        InsertOp{accepted, Key({Int64(3)}), {accepted_key}, {Int64(3)}},
    };
    EXPECT_FALSE(FlushWriteOpsToStorage(ops, storage.get(), t0).ok());

    for (const int64_t key : {1, 3}) {
      std::vector<googlesql::Value> values;
      EXPECT_FALSE(storage
                       ->Lookup(t0, accepted->id(), Key({Int64(key)}),
                                {accepted_key->id()}, &values)
                       .ok())
          << "row " << key << " of a failed commit reached storage";
    }

    // Without the failure, the same commit writes every row.
    storage->SetWriteHookForTesting(nullptr);
    const absl::Time t1 = t0 + absl::Seconds(1);
    GOOGLESQL_ASSERT_OK(FlushWriteOpsToStorage(ops, storage.get(), t1));
    std::vector<googlesql::Value> values;
    GOOGLESQL_EXPECT_OK(storage->Lookup(t1, accepted->id(), Key({Int64(3)}),
                                        {accepted_key->id()}, &values));
    GOOGLESQL_EXPECT_OK(storage->Lookup(t1, refused->id(), Key({Int64(2)}),
                                        {refused_key->id()}, &values));
  }
  std::error_code ignored;
  std::filesystem::remove_all(data_dir, ignored);
}

}  // namespace
}  // namespace backend
}  // namespace emulator
}  // namespace spanner
}  // namespace google

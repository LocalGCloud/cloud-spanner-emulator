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

#include "backend/query/access_recording_reader.h"

#include <cstdint>
#include <memory>
#include <string>
#include <utility>

#include "googlesql/public/type.h"
#include "googlesql/public/value.h"
#include "absl/status/status.h"
#include "backend/access/read.h"
#include "googlesql/base/status_macros.h"

namespace google {
namespace spanner {
namespace emulator {
namespace backend {

namespace {

// Counts the rows returned by another cursor.
class CountingRowCursor : public RowCursor {
 public:
  CountingRowCursor(std::unique_ptr<RowCursor> cursor, int64_t* rows)
      : cursor_(std::move(cursor)), rows_(rows) {}

  bool Next() override {
    if (!cursor_->Next()) {
      return false;
    }
    ++*rows_;
    return true;
  }
  absl::Status Status() const override { return cursor_->Status(); }
  int NumColumns() const override { return cursor_->NumColumns(); }
  const std::string ColumnName(int i) const override {
    return cursor_->ColumnName(i);
  }
  const googlesql::Value ColumnValue(int i) const override {
    return cursor_->ColumnValue(i);
  }
  const googlesql::Type* ColumnType(int i) const override {
    return cursor_->ColumnType(i);
  }

 private:
  std::unique_ptr<RowCursor> cursor_;
  int64_t* rows_;
};

}  // namespace

absl::Status AccessRecordingReader::Read(const ReadArg& read_arg,
                                         std::unique_ptr<RowCursor>* cursor) {
  std::unique_ptr<RowCursor> inner_cursor;
  GOOGLESQL_RETURN_IF_ERROR(reader_->Read(read_arg, &inner_cursor));
  // Internal change stream tables are not user tables.
  if (read_arg.change_stream_for_partition_table.empty() &&
      read_arg.change_stream_for_data_table.empty()) {
    footprint_.AddRead(read_arg.table, read_arg.columns);
  }
  *cursor = std::make_unique<CountingRowCursor>(std::move(inner_cursor),
                                                &rows_scanned_);
  return absl::OkStatus();
}

}  // namespace backend
}  // namespace emulator
}  // namespace spanner
}  // namespace google

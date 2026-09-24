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

#include <vector>

#include "backend/common/variant.h"
#include "backend/storage/storage.h"
#include "backend/transaction/commit_timestamp.h"

namespace google {
namespace spanner {
namespace emulator {
namespace backend {

namespace {

StorageRowOp FlushInsert(const InsertOp& insert_op,
                         absl::Time commit_timestamp) {
  const Table* table = insert_op.table;
  StorageRowOp row_op{
      .table_id = table->id(),
      .key = MaybeSetCommitTimestamp(table->primary_key(), insert_op.key,
                                     commit_timestamp)};
  for (int i = 0; i < insert_op.columns.size(); i++) {
    row_op.column_ids.push_back(insert_op.columns[i]->id());
    row_op.values.push_back(MaybeSetCommitTimestamp(
        insert_op.columns[i], insert_op.values[i], commit_timestamp));
  }
  return row_op;
}

StorageRowOp FlushUpdate(const UpdateOp& update_op,
                         absl::Time commit_timestamp) {
  const Table* table = update_op.table;
  StorageRowOp row_op{
      .table_id = table->id(),
      .key = MaybeSetCommitTimestamp(table->primary_key(), update_op.key,
                                     commit_timestamp)};
  for (int i = 0; i < update_op.columns.size(); i++) {
    row_op.column_ids.push_back(update_op.columns[i]->id());
    row_op.values.push_back(MaybeSetCommitTimestamp(
        update_op.columns[i], update_op.values[i], commit_timestamp));
  }
  return row_op;
}

StorageRowOp FlushDelete(const DeleteOp& delete_op) {
  return StorageRowOp{.table_id = delete_op.table->id(),
                      .key = delete_op.key,
                      .is_delete = true};
}

}  // namespace

absl::Status FlushWriteOpsToStorage(const std::vector<WriteOp>& write_ops,
                                    Storage* base_storage,
                                    absl::Time commit_timestamp) {
  std::vector<StorageRowOp> row_ops;
  row_ops.reserve(write_ops.size());
  for (const auto& write_op : write_ops) {
    row_ops.push_back(std::visit(
        overloaded{
            [&](const InsertOp& insert_op) {
              return FlushInsert(insert_op, commit_timestamp);
            },
            [&](const UpdateOp& update_op) {
              return FlushUpdate(update_op, commit_timestamp);
            },
            [&](const DeleteOp& delete_op) { return FlushDelete(delete_op); },
        },
        write_op));
  }
  // Apply the whole commit as one unit, so persistent storage has all of it
  // or none of it after a crash.
  return base_storage->ApplyRowOps(commit_timestamp, row_ops);
}

}  // namespace backend
}  // namespace emulator
}  // namespace spanner
}  // namespace google

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

#include "backend/stats/operation_stats.h"

#include <time.h>

#include <cstdint>
#include <string>

#include "googlesql/public/value.h"
#include "absl/strings/string_view.h"
#include "absl/time/time.h"
#include "absl/types/span.h"
#include "backend/access/write.h"
#include "backend/datamodel/key.h"
#include "backend/datamodel/key_range.h"

namespace google {
namespace spanner {
namespace emulator {
namespace backend {

int64_t LogicalByteSize(const googlesql::Value& value) {
  if (value.is_null()) {
    return 0;
  }
  switch (value.type_kind()) {
    case googlesql::TYPE_BOOL:
      return 1;
    case googlesql::TYPE_INT32:
    case googlesql::TYPE_UINT32:
    case googlesql::TYPE_FLOAT:
    case googlesql::TYPE_DATE:
    case googlesql::TYPE_ENUM:
      return 4;
    case googlesql::TYPE_TIMESTAMP:
      return 12;
    case googlesql::TYPE_NUMERIC:
    case googlesql::TYPE_INTERVAL:
    case googlesql::TYPE_UUID:
      return 16;
    case googlesql::TYPE_BIGNUMERIC:
      return 32;
    case googlesql::TYPE_STRING:
      return value.string_value().size();
    case googlesql::TYPE_BYTES:
      return value.bytes_value().size();
    case googlesql::TYPE_JSON:
      return value.is_unparsed_json() ? value.json_value_unparsed().size()
                                      : value.json_string().size();
    case googlesql::TYPE_PROTO:
      return value.ToCord().size();
    case googlesql::TYPE_ARRAY: {
      int64_t size = 0;
      for (const googlesql::Value& element : value.elements()) {
        size += LogicalByteSize(element);
      }
      return size;
    }
    case googlesql::TYPE_STRUCT: {
      int64_t size = 0;
      for (const googlesql::Value& field : value.fields()) {
        size += LogicalByteSize(field);
      }
      return size;
    }
    default:
      return 8;
  }
}

absl::Duration ThreadCpuTime() {
  timespec now;
  if (clock_gettime(CLOCK_THREAD_CPUTIME_ID, &now) != 0) {
    return absl::ZeroDuration();
  }
  return absl::DurationFromTimespec(now);
}

void AccessFootprint::AddRead(absl::string_view table,
                              absl::Span<const std::string> columns) {
  absl::btree_set<std::string>& table_columns =
      read_columns[std::string(table)];
  table_columns.insert(columns.begin(), columns.end());
}

void AccessFootprint::AddMutation(const Mutation& mutation) {
  for (const MutationOp& op : mutation.ops()) {
    if (op.type == MutationOpType::kDelete) {
      deleted_tables.insert(op.table);
      for (const Key& key : op.key_set.keys()) {
        bytes_written += key.LogicalSizeInBytes();
      }
      for (const KeyRange& range : op.key_set.ranges()) {
        bytes_written += range.start_key().LogicalSizeInBytes() +
                         range.limit_key().LogicalSizeInBytes();
      }
      continue;
    }
    if (op.type == MutationOpType::kReplace) {
      deleted_tables.insert(op.table);
    }
    written_columns[op.table].insert(op.columns.begin(), op.columns.end());
    TableWrites& table_writes = writes[op.table];
    for (const ValueList& row : op.rows) {
      int64_t row_bytes = 0;
      for (const googlesql::Value& value : row) {
        row_bytes += LogicalByteSize(value);
      }
      ++table_writes.rows;
      table_writes.bytes += row_bytes;
      bytes_written += row_bytes;
    }
  }
}

void AccessFootprint::Merge(const AccessFootprint& other) {
  for (const auto& [table, columns] : other.read_columns) {
    read_columns[table].insert(columns.begin(), columns.end());
  }
  for (const auto& [table, columns] : other.written_columns) {
    written_columns[table].insert(columns.begin(), columns.end());
  }
  deleted_tables.insert(other.deleted_tables.begin(),
                        other.deleted_tables.end());
  for (const auto& [table, table_writes] : other.writes) {
    TableWrites& merged = writes[table];
    merged.rows += table_writes.rows;
    merged.bytes += table_writes.bytes;
  }
  bytes_written += other.bytes_written;
}

}  // namespace backend
}  // namespace emulator
}  // namespace spanner
}  // namespace google

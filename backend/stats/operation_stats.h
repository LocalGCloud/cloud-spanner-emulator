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

#ifndef THIRD_PARTY_CLOUD_SPANNER_EMULATOR_BACKEND_STATS_OPERATION_STATS_H_
#define THIRD_PARTY_CLOUD_SPANNER_EMULATOR_BACKEND_STATS_OPERATION_STATS_H_

#include <cstdint>
#include <string>

#include "googlesql/public/value.h"
#include "absl/container/btree_map.h"
#include "absl/container/btree_set.h"
#include "absl/strings/string_view.h"
#include "absl/time/time.h"
#include "absl/types/span.h"
#include "backend/access/write.h"

namespace google {
namespace spanner {
namespace emulator {
namespace backend {

// Returns the logical size of `value` in bytes, as counted by the byte columns
// of the SPANNER_SYS statistics tables. NULL values have no size.
int64_t LogicalByteSize(const googlesql::Value& value);

// Returns the CPU time consumed so far by the calling thread.
absl::Duration ThreadCpuTime();

// Rows inserted or updated in one table and their logical size.
struct TableWrites {
  int64_t rows = 0;
  int64_t bytes = 0;
};

// The tables and columns that one or more operations read and wrote. Tables
// and columns are keyed by name, so the footprint stays meaningful after the
// operation's schema objects are gone.
struct AccessFootprint {
  // Columns read, by table.
  absl::btree_map<std::string, absl::btree_set<std::string>> read_columns;

  // Columns assigned new values by inserts, updates and replaces, by table.
  absl::btree_map<std::string, absl::btree_set<std::string>> written_columns;

  // Tables with rows deleted or replaced.
  absl::btree_set<std::string> deleted_tables;

  // Rows inserted, updated or replaced, by table.
  absl::btree_map<std::string, TableWrites> writes;

  // Logical size of every value written and every key deleted.
  int64_t bytes_written = 0;

  void AddRead(absl::string_view table, absl::Span<const std::string> columns);
  void AddMutation(const Mutation& mutation);
  void Merge(const AccessFootprint& other);
  bool empty() const {
    return read_columns.empty() && written_columns.empty() &&
           deleted_tables.empty();
  }
};

}  // namespace backend
}  // namespace emulator
}  // namespace spanner
}  // namespace google

#endif  // THIRD_PARTY_CLOUD_SPANNER_EMULATOR_BACKEND_STATS_OPERATION_STATS_H_

//
// Copyright 2026 Google LLC
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

#include "backend/storage/sequence_state_store.h"

#include <algorithm>
#include <cstdint>
#include <optional>
#include <vector>

#include "googlesql/public/value.h"
#include "absl/base/no_destructor.h"
#include "absl/status/status.h"
#include "absl/status/statusor.h"
#include "absl/strings/string_view.h"
#include "absl/synchronization/mutex.h"
#include "absl/time/clock.h"
#include "absl/time/time.h"
#include "backend/common/ids.h"
#include "backend/datamodel/key.h"

namespace google {
namespace spanner {
namespace emulator {
namespace backend {

namespace {

constexpr char kSequenceStateTableId[] = "_emulator_sequence_state";
constexpr char kNextCounterColumnId[] = "next_counter";

Key SequenceKey(absl::string_view sequence_name) {
  return Key({googlesql::Value::String(sequence_name)});
}

const std::vector<ColumnID>& StateColumns() {
  static const absl::NoDestructor<std::vector<ColumnID>> columns(
      {kNextCounterColumnId});
  return *columns;
}

// Storage keeps every version, and Load reads the newest one, so writes need
// increasing timestamps even if the wall clock steps back.
absl::Time NextWriteTimestamp() {
  static absl::NoDestructor<absl::Mutex> mu;
  static absl::Time last = absl::InfinitePast();
  absl::MutexLock lock(mu.get());
  last = std::max(absl::Now(), last + absl::Microseconds(1));
  return last;
}

}  // namespace

absl::StatusOr<std::optional<int64_t>> SequenceStateStore::Load(
    absl::string_view sequence_name) const {
  std::vector<googlesql::Value> values;
  absl::Status status =
      storage_->Lookup(absl::InfiniteFuture(), kSequenceStateTableId,
                       SequenceKey(sequence_name), StateColumns(), &values);
  if (absl::IsNotFound(status)) {
    return std::nullopt;
  }
  if (!status.ok()) {
    return status;
  }
  if (values.empty() || !values[0].is_valid() || values[0].is_null()) {
    return std::nullopt;
  }
  return values[0].int64_value();
}

absl::Status SequenceStateStore::Save(absl::string_view sequence_name,
                                      int64_t next_counter) {
  return storage_->Write(NextWriteTimestamp(), kSequenceStateTableId,
                         SequenceKey(sequence_name), StateColumns(),
                         {googlesql::Value::Int64(next_counter)});
}

absl::Status SequenceStateStore::Remove(absl::string_view sequence_name) {
  return storage_->Write(NextWriteTimestamp(), kSequenceStateTableId,
                         SequenceKey(sequence_name), StateColumns(),
                         {googlesql::Value::NullInt64()});
}

}  // namespace backend
}  // namespace emulator
}  // namespace spanner
}  // namespace google

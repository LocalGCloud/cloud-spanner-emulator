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

#include "backend/storage/user_split_point_store.h"

#include <algorithm>
#include <memory>
#include <vector>

#include "googlesql/public/value.h"
#include "absl/base/no_destructor.h"
#include "absl/status/status.h"
#include "absl/status/statusor.h"
#include "absl/synchronization/mutex.h"
#include "absl/time/clock.h"
#include "absl/time/time.h"
#include "backend/common/ids.h"
#include "backend/datamodel/key.h"
#include "backend/datamodel/key_range.h"
#include "backend/storage/iterator.h"
#include "googlesql/base/status_macros.h"

namespace google {
namespace spanner {
namespace emulator {
namespace backend {

namespace {

constexpr char kUserSplitPointTableId[] = "_emulator_user_split_points";

const std::vector<ColumnID>& SplitPointColumns() {
  static const absl::NoDestructor<std::vector<ColumnID>> columns(
      {"initiator", "expire_time"});
  return *columns;
}

// Storage keeps every version and reads return the newest one, so writes need
// increasing timestamps even if the wall clock steps back.
absl::Time NextWriteTimestamp() {
  static absl::NoDestructor<absl::Mutex> mu;
  static absl::Time last = absl::InfinitePast();
  absl::MutexLock lock(mu.get());
  last = std::max(absl::Now(), last + absl::Microseconds(1));
  return last;
}

}  // namespace

absl::StatusOr<std::vector<UserSplitPoint>> UserSplitPointStore::LoadAll()
    const {
  std::unique_ptr<StorageIterator> itr;
  GOOGLESQL_RETURN_IF_ERROR(storage_->Read(absl::InfiniteFuture(),
                                           kUserSplitPointTableId,
                                           KeyRange::All(), SplitPointColumns(),
                                           &itr));
  std::vector<UserSplitPoint> split_points;
  while (itr->Next()) {
    const Key& key = itr->Key();
    split_points.push_back({
        .table_name = key.ColumnValue(0).string_value(),
        .index_name = key.ColumnValue(1).string_value(),
        .initiator = itr->ColumnValue(0).string_value(),
        .split_key = key.ColumnValue(2).string_value(),
        .expire_time = itr->ColumnValue(1).ToTime(),
    });
  }
  GOOGLESQL_RETURN_IF_ERROR(itr->Status());
  return split_points;
}

absl::Status UserSplitPointStore::Save(const UserSplitPoint& split_point) {
  return storage_->Write(
      NextWriteTimestamp(), kUserSplitPointTableId,
      Key({googlesql::Value::String(split_point.table_name),
           googlesql::Value::String(split_point.index_name),
           googlesql::Value::String(split_point.split_key)}),
      SplitPointColumns(),
      {googlesql::Value::String(split_point.initiator),
       googlesql::Value::Timestamp(split_point.expire_time)});
}

}  // namespace backend
}  // namespace emulator
}  // namespace spanner
}  // namespace google

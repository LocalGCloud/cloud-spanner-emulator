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

#ifndef THIRD_PARTY_CLOUD_SPANNER_EMULATOR_BACKEND_STORAGE_USER_SPLIT_POINT_STORE_H_
#define THIRD_PARTY_CLOUD_SPANNER_EMULATOR_BACKEND_STORAGE_USER_SPLIT_POINT_STORE_H_

#include <vector>

#include "absl/status/status.h"
#include "absl/status/statusor.h"
#include "backend/stats/system_stats_collector.h"
#include "backend/storage/storage.h"

namespace google {
namespace spanner {
namespace emulator {
namespace backend {

// Keeps the split points added with AddSplitPoints in a database's storage, so
// that they survive a restart with --data_dir.
//
// Each split point has one row, keyed by its table name, index name and split
// key, in a reserved internal table. Generated table IDs always contain ':',
// so it can't collide with a user table.
class UserSplitPointStore {
 public:
  explicit UserSplitPointStore(Storage* storage) : storage_(storage) {}

  // Returns the saved split points.
  absl::StatusOr<std::vector<UserSplitPoint>> LoadAll() const;

  // Saves `split_point`, replacing the one with the same table name, index
  // name and split key.
  absl::Status Save(const UserSplitPoint& split_point);

 private:
  Storage* storage_;
};

}  // namespace backend
}  // namespace emulator
}  // namespace spanner
}  // namespace google

#endif  // THIRD_PARTY_CLOUD_SPANNER_EMULATOR_BACKEND_STORAGE_USER_SPLIT_POINT_STORE_H_

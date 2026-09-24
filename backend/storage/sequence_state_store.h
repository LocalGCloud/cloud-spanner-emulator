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

#ifndef THIRD_PARTY_CLOUD_SPANNER_EMULATOR_BACKEND_STORAGE_SEQUENCE_STATE_STORE_H_
#define THIRD_PARTY_CLOUD_SPANNER_EMULATOR_BACKEND_STORAGE_SEQUENCE_STATE_STORE_H_

#include <cstdint>
#include <optional>

#include "absl/status/status.h"
#include "absl/status/statusor.h"
#include "absl/strings/string_view.h"
#include "backend/storage/storage.h"

namespace google {
namespace spanner {
namespace emulator {
namespace backend {

// Keeps sequence counters in a database's storage so that a sequence doesn't
// hand out values again after a restart with --data_dir or a backup restore.
//
// Each sequence has one row, keyed by its name, holding a counter from which
// the sequence can safely continue: every value handed out came from a lower
// counter. The rows live in a reserved internal table. Generated table IDs
// always contain ':', so it can't collide with a user table.
class SequenceStateStore {
 public:
  explicit SequenceStateStore(Storage* storage) : storage_(storage) {}

  // Returns the saved counter for `sequence_name`, or nullopt if none is saved.
  absl::StatusOr<std::optional<int64_t>> Load(
      absl::string_view sequence_name) const;

  // Saves `next_counter` for `sequence_name`, replacing any earlier value.
  absl::Status Save(absl::string_view sequence_name, int64_t next_counter);

  // Forgets the saved counter, so the sequence starts from its configured
  // start value after a restart.
  absl::Status Remove(absl::string_view sequence_name);

 private:
  Storage* storage_;
};

}  // namespace backend
}  // namespace emulator
}  // namespace spanner
}  // namespace google

#endif  // THIRD_PARTY_CLOUD_SPANNER_EMULATOR_BACKEND_STORAGE_SEQUENCE_STATE_STORE_H_

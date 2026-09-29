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

#ifndef THIRD_PARTY_CLOUD_SPANNER_EMULATOR_FRONTEND_PERSISTENCE_DATA_DIR_LOCK_H_
#define THIRD_PARTY_CLOUD_SPANNER_EMULATOR_FRONTEND_PERSISTENCE_DATA_DIR_LOCK_H_

#include <memory>
#include <string>

#include "absl/status/statusor.h"

namespace google {
namespace spanner {
namespace emulator {
namespace frontend {

// An exclusive lock on a --data_dir, so that two emulator processes never
// write the same persisted state. The lock is an advisory flock() on the file
// <data_dir>/.lock, which records the PID of the holder. The operating system
// releases it when the holder exits, even if it crashes.
class DataDirLock {
 public:
  // Creates `data_dir` if it does not exist and locks it. Returns
  // FAILED_PRECONDITION if another process, or another DataDirLock in this
  // process, holds the lock.
  static absl::StatusOr<std::unique_ptr<DataDirLock>> Acquire(
      const std::string& data_dir);

  // Releases the lock.
  ~DataDirLock();

  DataDirLock(const DataDirLock&) = delete;
  DataDirLock& operator=(const DataDirLock&) = delete;

 private:
  explicit DataDirLock(int fd) : fd_(fd) {}

  const int fd_;
};

}  // namespace frontend
}  // namespace emulator
}  // namespace spanner
}  // namespace google

#endif  // THIRD_PARTY_CLOUD_SPANNER_EMULATOR_FRONTEND_PERSISTENCE_DATA_DIR_LOCK_H_

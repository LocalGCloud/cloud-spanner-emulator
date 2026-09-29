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

#include "frontend/persistence/data_dir_lock.h"

#include <fcntl.h>
#include <sys/file.h>
#include <unistd.h>

#include <cerrno>
#include <cstring>
#include <filesystem>  // NOLINT
#include <memory>
#include <string>
#include <system_error>  // NOLINT

#include "absl/status/status.h"
#include "absl/status/statusor.h"
#include "absl/strings/str_cat.h"

namespace google {
namespace spanner {
namespace emulator {
namespace frontend {

absl::StatusOr<std::unique_ptr<DataDirLock>> DataDirLock::Acquire(
    const std::string& data_dir) {
  std::error_code error;
  std::filesystem::create_directories(data_dir, error);
  if (error) {
    return absl::InternalError(absl::StrCat(
        "Failed to create --data_dir ", data_dir, ": ", error.message()));
  }
  const std::string path = absl::StrCat(data_dir, "/.lock");
  const int fd = open(path.c_str(), O_RDWR | O_CREAT | O_NOFOLLOW | O_CLOEXEC,
                      0644);
  if (fd < 0) {
    return absl::InternalError(absl::StrCat("Failed to open lock file ", path,
                                            ": ", std::strerror(errno)));
  }
  if (flock(fd, LOCK_EX | LOCK_NB) != 0) {
    const int lock_errno = errno;
    close(fd);
    if (lock_errno == EWOULDBLOCK) {
      return absl::FailedPreconditionError(absl::StrCat(
          "--data_dir ", data_dir,
          " is in use by another emulator process (see the PID in ", path,
          "). Stop that process or use a different --data_dir."));
    }
    return absl::InternalError(absl::StrCat("Failed to lock ", path, ": ",
                                            std::strerror(lock_errno)));
  }
  // Record the holder for the error message of a second process. Failing to
  // write it does not weaken the lock.
  const std::string pid = absl::StrCat(getpid(), "\n");
  if (ftruncate(fd, 0) == 0) {
    (void)pwrite(fd, pid.data(), pid.size(), 0);
  }
  return std::unique_ptr<DataDirLock>(new DataDirLock(fd));
}

DataDirLock::~DataDirLock() { close(fd_); }

}  // namespace frontend
}  // namespace emulator
}  // namespace spanner
}  // namespace google

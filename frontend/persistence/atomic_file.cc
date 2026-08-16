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

#include "frontend/persistence/atomic_file.h"

#include <cerrno>
#include <cstddef>
#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>

#include <filesystem>
#include <system_error>
#include <string>

#include "absl/status/status.h"
#include "absl/status/statusor.h"
#include "absl/strings/str_cat.h"

namespace google {
namespace spanner {
namespace emulator {
namespace frontend {
namespace {

absl::Status FileError(absl::StatusCode code, const std::string& action,
                       const std::string& path, int error_number) {
  return absl::Status(code,
                      absl::StrCat(action, " ", path, ": ",
                                   std::generic_category()
                                       .message(error_number)));
}

void CloseIgnoringError(int fd) {
  while (close(fd) < 0 && errno == EINTR) {
  }
}

}  // namespace

absl::StatusOr<std::string> ReadRegularFileNoFollow(
    const std::string& path) {
  struct stat path_status;
  if (lstat(path.c_str(), &path_status) != 0) {
    return FileError(absl::StatusCode::kDataLoss, "Failed to inspect", path,
                     errno);
  }
  if (!S_ISREG(path_status.st_mode)) {
    return absl::DataLossError(
        absl::StrCat("Persistent file is not a regular file: ", path));
  }

  const int fd = open(path.c_str(), O_RDONLY | O_CLOEXEC | O_NOFOLLOW);
  if (fd < 0) {
    return FileError(absl::StatusCode::kDataLoss, "Failed to open", path,
                     errno);
  }
  struct stat opened_status;
  if (fstat(fd, &opened_status) != 0 || !S_ISREG(opened_status.st_mode)) {
    const int error_number = errno;
    CloseIgnoringError(fd);
    return error_number == 0
               ? absl::DataLossError(absl::StrCat(
                     "Persistent file is not a regular file: ", path))
               : FileError(absl::StatusCode::kDataLoss, "Failed to inspect",
                           path, error_number);
  }

  std::string contents;
  char buffer[8192];
  while (true) {
    const ssize_t bytes_read = read(fd, buffer, sizeof(buffer));
    if (bytes_read > 0) {
      contents.append(buffer, static_cast<std::size_t>(bytes_read));
      continue;
    }
    if (bytes_read == 0) break;
    if (errno == EINTR) continue;
    const int error_number = errno;
    CloseIgnoringError(fd);
    return FileError(absl::StatusCode::kDataLoss, "Failed to read", path,
                     error_number);
  }
  if (close(fd) != 0) {
    return FileError(absl::StatusCode::kDataLoss, "Failed to close", path,
                     errno);
  }
  return contents;
}

absl::Status WriteFileAtomicallyNoFollow(const std::string& path,
                                         const std::string& contents) {
  struct stat target_status;
  if (lstat(path.c_str(), &target_status) == 0) {
    if (!S_ISREG(target_status.st_mode)) {
      return absl::FailedPreconditionError(absl::StrCat(
          "Persistence target is not a regular file (symbolic links are "
          "forbidden): ",
          path));
    }
  } else if (errno != ENOENT) {
    return FileError(absl::StatusCode::kInternal,
                     "Failed to inspect persistence target", path, errno);
  }
  const std::string temporary_path = path + ".tmp";
  struct stat temporary_status;
  if (lstat(temporary_path.c_str(), &temporary_status) == 0) {
    if (!S_ISREG(temporary_status.st_mode)) {
      return absl::FailedPreconditionError(absl::StrCat(
          "Temporary persistence path is not a regular file (symbolic links "
          "are forbidden): ",
          temporary_path));
    }
    if (unlink(temporary_path.c_str()) != 0) {
      return FileError(absl::StatusCode::kInternal,
                       "Failed to remove stale temporary file",
                       temporary_path, errno);
    }
  } else if (errno != ENOENT) {
    return FileError(absl::StatusCode::kInternal,
                     "Failed to inspect temporary file", temporary_path,
                     errno);
  }

  const int fd = open(temporary_path.c_str(),
                      O_WRONLY | O_CREAT | O_EXCL | O_CLOEXEC | O_NOFOLLOW,
                      0600);
  if (fd < 0) {
    return FileError(absl::StatusCode::kInternal, "Failed to create",
                     temporary_path, errno);
  }

  std::size_t offset = 0;
  while (offset < contents.size()) {
    const ssize_t bytes_written =
        write(fd, contents.data() + offset, contents.size() - offset);
    if (bytes_written > 0) {
      offset += static_cast<std::size_t>(bytes_written);
      continue;
    }
    if (bytes_written < 0 && errno == EINTR) continue;
    const int error_number = errno;
    CloseIgnoringError(fd);
    unlink(temporary_path.c_str());
    return FileError(absl::StatusCode::kInternal, "Failed to write",
                     temporary_path, error_number);
  }
  if (fsync(fd) != 0) {
    const int error_number = errno;
    CloseIgnoringError(fd);
    unlink(temporary_path.c_str());
    return FileError(absl::StatusCode::kInternal, "Failed to sync",
                     temporary_path, error_number);
  }
  if (close(fd) != 0) {
    const int error_number = errno;
    unlink(temporary_path.c_str());
    return FileError(absl::StatusCode::kInternal, "Failed to close",
                     temporary_path, error_number);
  }
  const std::filesystem::path parent =
      std::filesystem::path(path).parent_path();
  const int directory_fd =
      open(parent.c_str(), O_RDONLY | O_CLOEXEC | O_DIRECTORY | O_NOFOLLOW);
  if (directory_fd < 0) {
    const int error_number = errno;
    unlink(temporary_path.c_str());
    return FileError(absl::StatusCode::kInternal,
                     "Failed to open persistence directory", parent.string(),
                     error_number);
  }
  // Validate that the parent can be synchronized before publishing the new
  // file. Otherwise callers could receive a failure after rename and roll
  // their in-memory state back while the new state is already visible.
  if (fsync(directory_fd) != 0) {
    const int error_number = errno;
    CloseIgnoringError(directory_fd);
    unlink(temporary_path.c_str());
    return FileError(absl::StatusCode::kInternal,
                     "Failed to sync persistence directory", parent.string(),
                     error_number);
  }
  if (rename(temporary_path.c_str(), path.c_str()) != 0) {
    const int error_number = errno;
    CloseIgnoringError(directory_fd);
    unlink(temporary_path.c_str());
    return FileError(absl::StatusCode::kInternal, "Failed to replace", path,
                     error_number);
  }
  if (fsync(directory_fd) != 0) {
    const int error_number = errno;
    CloseIgnoringError(directory_fd);
    return FileError(absl::StatusCode::kDataLoss,
                     "Published persistence file but failed to sync directory",
                     parent.string(), error_number);
  }
  if (close(directory_fd) != 0) {
    return FileError(absl::StatusCode::kDataLoss,
                     "Published persistence file but failed to close directory",
                     parent.string(), errno);
  }
  return absl::OkStatus();
}

}  // namespace frontend
}  // namespace emulator
}  // namespace spanner
}  // namespace google

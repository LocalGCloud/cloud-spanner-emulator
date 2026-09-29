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

#include <sys/wait.h>
#include <unistd.h>

#include <filesystem>  // NOLINT
#include <fstream>
#include <memory>
#include <string>

#include "gmock/gmock.h"
#include "gtest/gtest.h"
#include "googlesql/base/testing/status_matchers.h"
#include "absl/status/status.h"
#include "absl/strings/str_cat.h"

namespace google {
namespace spanner {
namespace emulator {
namespace frontend {
namespace {

using ::googlesql_base::testing::StatusIs;
using ::testing::HasSubstr;

std::string TestDataDir(const std::string& name) {
  const std::string dir = absl::StrCat(testing::TempDir(), "/", name);
  std::filesystem::remove_all(dir);
  return dir;
}

TEST(DataDirLockTest, SecondLockOfTheSameDirectoryFails) {
  const std::string data_dir = TestDataDir("second_lock");
  GOOGLESQL_ASSERT_OK_AND_ASSIGN(std::unique_ptr<DataDirLock> lock,
                       DataDirLock::Acquire(data_dir));
  EXPECT_TRUE(std::filesystem::exists(data_dir + "/.lock"));
  std::ifstream lock_file(data_dir + "/.lock");
  std::string holder;
  std::getline(lock_file, holder);
  EXPECT_EQ(holder, absl::StrCat(getpid()));

  EXPECT_THAT(DataDirLock::Acquire(data_dir),
              StatusIs(absl::StatusCode::kFailedPrecondition,
                       HasSubstr("in use by another emulator process")));

  // Releasing the lock lets the directory be locked again.
  lock.reset();
  GOOGLESQL_EXPECT_OK(DataDirLock::Acquire(data_dir));
}

TEST(DataDirLockTest, AnotherProcessCannotLockTheDirectory) {
  const std::string data_dir = TestDataDir("other_process");
  GOOGLESQL_ASSERT_OK_AND_ASSIGN(std::unique_ptr<DataDirLock> lock,
                       DataDirLock::Acquire(data_dir));

  auto child_exit_code = [&data_dir]() {
    const pid_t child = fork();
    if (child == 0) {
      absl::StatusOr<std::unique_ptr<DataDirLock>> child_lock =
          DataDirLock::Acquire(data_dir);
      if (child_lock.ok()) _exit(0);
      _exit(child_lock.status().code() ==
                    absl::StatusCode::kFailedPrecondition
                ? 1
                : 2);
    }
    int status = 0;
    waitpid(child, &status, 0);
    return WIFEXITED(status) ? WEXITSTATUS(status) : -1;
  };
  EXPECT_EQ(child_exit_code(), 1);

  // The lock is released with its holder.
  lock.reset();
  EXPECT_EQ(child_exit_code(), 0);
}

TEST(DataDirLockTest, CreatesTheDataDirectory) {
  const std::string data_dir = TestDataDir("created") + "/nested";
  GOOGLESQL_EXPECT_OK(DataDirLock::Acquire(data_dir));
  EXPECT_TRUE(std::filesystem::is_directory(data_dir));
}

}  // namespace
}  // namespace frontend
}  // namespace emulator
}  // namespace spanner
}  // namespace google

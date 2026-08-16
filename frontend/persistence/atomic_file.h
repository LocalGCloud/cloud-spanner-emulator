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

#ifndef THIRD_PARTY_CLOUD_SPANNER_EMULATOR_FRONTEND_PERSISTENCE_ATOMIC_FILE_H_
#define THIRD_PARTY_CLOUD_SPANNER_EMULATOR_FRONTEND_PERSISTENCE_ATOMIC_FILE_H_

#include <string>

#include "absl/status/status.h"
#include "absl/status/statusor.h"

namespace google {
namespace spanner {
namespace emulator {
namespace frontend {

// Reads a regular file without following a symbolic link at the final path.
absl::StatusOr<std::string> ReadRegularFileNoFollow(const std::string& path);

// Replaces path atomically through path + ".tmp". Neither the temporary file
// nor the final file is ever opened through a symbolic link.
absl::Status WriteFileAtomicallyNoFollow(const std::string& path,
                                         const std::string& contents);

}  // namespace frontend
}  // namespace emulator
}  // namespace spanner
}  // namespace google

#endif  // THIRD_PARTY_CLOUD_SPANNER_EMULATOR_FRONTEND_PERSISTENCE_ATOMIC_FILE_H_

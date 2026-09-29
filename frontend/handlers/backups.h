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

#ifndef THIRD_PARTY_CLOUD_SPANNER_EMULATOR_FRONTEND_HANDLERS_BACKUPS_H_
#define THIRD_PARTY_CLOUD_SPANNER_EMULATOR_FRONTEND_HANDLERS_BACKUPS_H_

#include "absl/status/status.h"
#include "absl/time/time.h"

namespace google {
namespace spanner {
namespace emulator {
namespace frontend {

class ServerEnv;

// Runs schedules due at now. The binary invokes this from its polling worker.
absl::Status RunDueBackupSchedules(ServerEnv* env, absl::Time now);

}  // namespace frontend
}  // namespace emulator
}  // namespace spanner
}  // namespace google

#endif  // THIRD_PARTY_CLOUD_SPANNER_EMULATOR_FRONTEND_HANDLERS_BACKUPS_H_

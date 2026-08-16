//
// Copyright 2026 Google LLC
//
// Licensed under the Apache License, Version 2.0 (the "License");
// you may not use this file except in compliance with the License.
// You may obtain a copy of the License at
//
//     http://www.apache.org/licenses/LICENSE-2.0
//
// Unless required by applicable law or agreed to in writing, software
// distributed under the License is distributed on an "AS IS" BASIS,
// WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
// See the License for the specific language governing permissions and
// limitations under the License.
//

#ifndef THIRD_PARTY_CLOUD_SPANNER_EMULATOR_FRONTEND_PERSISTENCE_SCHEMA_CHANGE_BATCH_H_
#define THIRD_PARTY_CLOUD_SPANNER_EMULATOR_FRONTEND_PERSISTENCE_SCHEMA_CHANGE_BATCH_H_

#include <string>
#include <vector>

namespace google {
namespace spanner {
namespace emulator {
namespace frontend {

// One successfully committed schema-change request and the descriptor bundle
// against which its statements were parsed. Replaying batches in order avoids
// applying historical statements with an unrelated later descriptor bundle.
struct PersistedSchemaChangeBatch {
  std::vector<std::string> statements;
  std::string proto_descriptor_bytes;
  // RFC3339 UTC commit timestamp of the batch. Replaying with the original
  // timestamp restores time-based schema state such as change stream
  // creation times. Empty means the timestamp is unknown (legacy state).
  std::string schema_change_timestamp;
};

}  // namespace frontend
}  // namespace emulator
}  // namespace spanner
}  // namespace google

#endif  // THIRD_PARTY_CLOUD_SPANNER_EMULATOR_FRONTEND_PERSISTENCE_SCHEMA_CHANGE_BATCH_H_

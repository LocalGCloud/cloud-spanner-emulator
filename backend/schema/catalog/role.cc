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

#include "backend/schema/catalog/role.h"

#include <string>

#include "absl/status/status.h"

namespace google {
namespace spanner {
namespace emulator {
namespace backend {

absl::Status Role::Validate(SchemaValidationContext* context) const {
  return absl::OkStatus();
}

absl::Status Role::ValidateUpdate(
    const SchemaNode* old, SchemaValidationContext* context) const {
  return absl::OkStatus();
}

std::string Role::DebugString() const { return "R:" + name_; }

absl::Status Role::DeepClone(SchemaGraphEditor* editor,
                             const SchemaNode* orig) {
  return absl::OkStatus();
}

}  // namespace backend
}  // namespace emulator
}  // namespace spanner
}  // namespace google

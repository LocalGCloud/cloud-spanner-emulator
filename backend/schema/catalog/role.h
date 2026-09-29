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

#ifndef THIRD_PARTY_CLOUD_SPANNER_EMULATOR_BACKEND_SCHEMA_CATALOG_ROLE_H_
#define THIRD_PARTY_CLOUD_SPANNER_EMULATOR_BACKEND_SCHEMA_CATALOG_ROLE_H_

#include <memory>
#include <optional>
#include <string>
#include <utility>

#include "absl/status/status.h"
#include "backend/schema/graph/schema_node.h"

namespace google {
namespace spanner {
namespace emulator {
namespace backend {

class Role final : public SchemaNode {
 public:
  explicit Role(std::string name) : name_(std::move(name)) {}

  const std::string& Name() const { return name_; }

  std::optional<SchemaNameInfo> GetSchemaNameInfo() const override {
    return SchemaNameInfo{.name = name_, .kind = "Role", .global = true};
  }

  absl::Status Validate(SchemaValidationContext* context) const override;
  absl::Status ValidateUpdate(const SchemaNode* old,
                              SchemaValidationContext* context) const override;
  std::string DebugString() const override;

 private:
  std::unique_ptr<SchemaNode> ShallowClone() const override {
    return std::make_unique<Role>(*this);
  }
  absl::Status DeepClone(SchemaGraphEditor* editor,
                         const SchemaNode* orig) override;

  std::string name_;
};

}  // namespace backend
}  // namespace emulator
}  // namespace spanner
}  // namespace google

#endif  // THIRD_PARTY_CLOUD_SPANNER_EMULATOR_BACKEND_SCHEMA_CATALOG_ROLE_H_

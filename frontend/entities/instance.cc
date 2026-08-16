//
// Copyright 2020 Google LLC
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

#include "frontend/entities/instance.h"

#include <memory>
#include <utility>

#include "absl/strings/string_view.h"
#include "frontend/converters/time.h"
#include "google/spanner/admin/instance/v1/spanner_instance_admin.pb.h"

namespace google {
namespace spanner {
namespace emulator {
namespace frontend {

void Instance::ToProto(admin::instance::v1::Instance* instance) const {
  absl::ReaderMutexLock lock(mu_);
  instance->Clear();
  instance->set_name(name_);
  instance->set_config(config_);
  instance->set_display_name(display_name_);
  instance->set_node_count(node_count_);
  instance->set_processing_units(processing_units_);
  instance->mutable_labels()->insert(labels_.begin(), labels_.end());
  // Instances are always in ready state.
  instance->set_state(admin::instance::v1::Instance::READY);
  *instance->mutable_create_time() = TimestampToProto(create_time_).value();
  *instance->mutable_update_time() = TimestampToProto(update_time_).value();
}

void Instance::UpdateDisplayName(const std::string& display_name,
                                 absl::Time update_time) {
  absl::MutexLock lock(mu_);
  display_name_ = display_name;
  update_time_ = update_time;
}

void Instance::UpdateNodeCount(int32_t node_count, absl::Time update_time) {
  absl::MutexLock lock(mu_);
  node_count_ = node_count;
  processing_units_ = node_count * 1000;
  update_time_ = update_time;
}

void Instance::UpdateProcessingUnits(int32_t processing_units,
                                     absl::Time update_time) {
  absl::MutexLock lock(mu_);
  processing_units_ = processing_units;
  node_count_ = processing_units / 1000;
  update_time_ = update_time;
}

void Instance::UpdateLabels(Labels labels, absl::Time update_time) {
  absl::MutexLock lock(mu_);
  labels_ = std::move(labels);
  update_time_ = update_time;
}

void Instance::UpdateConfig(const std::string& config,
                            absl::Time update_time) {
  absl::MutexLock lock(mu_);
  config_ = config;
  update_time_ = update_time;
}

}  // namespace frontend
}  // namespace emulator
}  // namespace spanner
}  // namespace google

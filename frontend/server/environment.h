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

#ifndef THIRD_PARTY_CLOUD_SPANNER_EMULATOR_FRONTEND_SERVER_ENV_H_
#define THIRD_PARTY_CLOUD_SPANNER_EMULATOR_FRONTEND_SERVER_ENV_H_

#include <map>
#include <memory>
#include <optional>
#include <string>
#include <utility>
#include <vector>

#include "absl/status/status.h"
#include "absl/status/statusor.h"
#include "absl/strings/string_view.h"
#include "absl/strings/str_cat.h"
#include "absl/synchronization/mutex.h"
#include "common/clock.h"
#include "common/config.h"
#include "frontend/common/uris.h"
#include "frontend/collections/database_manager.h"
#include "frontend/collections/instance_manager.h"
#include "frontend/collections/instance_partition_manager.h"
#include "frontend/collections/multiplexed_session_transaction_manager.h"
#include "frontend/collections/operation_manager.h"
#include "frontend/collections/session_manager.h"
#include "frontend/persistence/backup_catalog.h"
#include "frontend/persistence/metadata_store.h"
#include "google/iam/v1/policy.pb.h"
#include "google/spanner/admin/instance/v1/spanner_instance_admin.pb.h"

namespace google {
namespace spanner {
namespace emulator {
namespace frontend {

// ServerEnv encapsulates global objects for Cloud Spanner Emulator.
class ServerEnv {
 public:
  ServerEnv()
      : clock_(new Clock()),
        database_manager_(new DatabaseManager(clock_.get(),
                                              config::data_dir())),
        instance_manager_(new InstanceManager()),
        instance_partition_manager_(new InstancePartitionManager()),
        operation_manager_(new OperationManager()),
        session_manager_(new SessionManager(clock_.get())),
        mux_txn_manager_(new MultiplexedSessionTransactionManager()),
        backup_catalog_(new BackupCatalog(config::data_dir())) {
    std::string data_dir = config::data_dir();
    if (!data_dir.empty()) {
      metadata_store_ = std::make_unique<MetadataStore>(data_dir);
    }
  }

  Clock* clock() { return clock_.get(); }
  DatabaseManager* database_manager() { return database_manager_.get(); }
  InstanceManager* instance_manager() { return instance_manager_.get(); }
  InstancePartitionManager* instance_partition_manager() {
    return instance_partition_manager_.get();
  }
  OperationManager* operation_manager() { return operation_manager_.get(); }
  SessionManager* session_manager() { return session_manager_.get(); }
  MultiplexedSessionTransactionManager* mux_txn_manager() {
    return mux_txn_manager_.get();
  }
  MetadataStore* metadata_store() { return metadata_store_.get(); }
  BackupCatalog* backup_catalog() { return backup_catalog_.get(); }

  // Validates a canonical IAM resource name and requires the referenced
  // emulator resource to exist.
  absl::Status ValidateIamResource(const std::string& resource) {
    absl::string_view project_id;
    absl::string_view instance_id;
    absl::string_view resource_id;
    if (ParseInstanceUri(resource, &project_id, &instance_id).ok() &&
        MakeInstanceUri(project_id, instance_id) == resource) {
      return instance_manager_->GetInstance(resource).status();
    }
    if (ParseDatabaseUri(resource, &project_id, &instance_id, &resource_id)
            .ok() &&
        MakeDatabaseUri(MakeInstanceUri(project_id, instance_id), resource_id) ==
            resource) {
      return database_manager_->GetDatabase(resource).status();
    }
    if (ParseInstancePartitionUri(resource, &project_id, &instance_id,
                                  &resource_id)
            .ok() &&
        MakeInstancePartitionUri(MakeInstanceUri(project_id, instance_id),
                                 resource_id) == resource) {
      return instance_partition_manager_->GetInstancePartition(resource)
          .status();
    }
    if (ParseInstanceConfigUri(resource, &project_id, &resource_id).ok() &&
        MakeInstanceConfigUri(project_id, resource_id) == resource) {
      if (resource_id == "emulator-config") return absl::OkStatus();
      return GetCustomInstanceConfig(resource).status();
    }

    const size_t backup = resource.rfind("/backups/");
    if (backup != std::string::npos && backup > 0 &&
        resource.find('/', backup + 9) == std::string::npos &&
        backup + 9 < resource.size()) {
      const std::string parent = resource.substr(0, backup);
      if (ParseInstanceUri(parent, &project_id, &instance_id).ok() &&
          MakeInstanceUri(project_id, instance_id) == parent) {
        return backup_catalog_->GetBackup(resource).status();
      }
    }
    const size_t schedule = resource.rfind("/backupSchedules/");
    if (schedule != std::string::npos && schedule > 0 &&
        resource.find('/', schedule + 17) == std::string::npos &&
        schedule + 17 < resource.size()) {
      const std::string parent = resource.substr(0, schedule);
      if (ParseDatabaseUri(parent, &project_id, &instance_id, &resource_id)
              .ok() &&
          MakeDatabaseUri(MakeInstanceUri(project_id, instance_id),
                          resource_id) == parent) {
        return backup_catalog_->GetBackupSchedule(resource).status();
      }
    }
    return absl::InvalidArgumentError(
        absl::StrCat("Unsupported or malformed IAM resource: ", resource));
  }

  void SetIamPolicy(const std::string& resource,
                    const ::google::iam::v1::Policy& policy) {
    absl::MutexLock lock(&iam_policies_mu_);
    iam_policies_[resource] = policy;
  }

  std::optional<::google::iam::v1::Policy> GetIamPolicy(
      const std::string& resource) const {
    absl::ReaderMutexLock lock(&iam_policies_mu_);
    auto it = iam_policies_.find(resource);
    if (it == iam_policies_.end()) {
      return std::nullopt;
    }
    return it->second;
  }

  void RemoveIamPolicy(const std::string& resource) {
    absl::MutexLock lock(&iam_policies_mu_);
    iam_policies_.erase(resource);
  }

  void RemoveIamPolicies(const std::string& resource) {
    absl::MutexLock lock(&iam_policies_mu_);
    const std::string nested_prefix = resource + "/";
    for (auto it = iam_policies_.begin(); it != iam_policies_.end();) {
      if (it->first == resource || it->first.rfind(nested_prefix, 0) == 0) {
        it = iam_policies_.erase(it);
      } else {
        ++it;
      }
    }
  }

  absl::Status CreateInstanceConfig(
      ::google::spanner::admin::instance::v1::InstanceConfig config) {
    static constexpr absl::string_view kBuiltInConfigSuffix =
        "/instanceConfigs/emulator-config";
    if (config.name().size() >= kBuiltInConfigSuffix.size() &&
        config.name().compare(config.name().size() -
                                  kBuiltInConfigSuffix.size(),
                              kBuiltInConfigSuffix.size(),
                              kBuiltInConfigSuffix) == 0) {
      return absl::AlreadyExistsError(
          "The built-in emulator instance config is reserved");
    }
    absl::MutexLock lock(&instance_configs_mu_);
    if (instance_configs_.contains(config.name())) {
      return absl::AlreadyExistsError("Instance config already exists");
    }
    instance_configs_.emplace(config.name(), std::move(config));
    return absl::OkStatus();
  }

  absl::StatusOr<::google::spanner::admin::instance::v1::InstanceConfig>
  GetCustomInstanceConfig(const std::string& name) const {
    absl::ReaderMutexLock lock(&instance_configs_mu_);
    auto it = instance_configs_.find(name);
    if (it == instance_configs_.end()) {
      return absl::NotFoundError("Instance config not found");
    }
    return it->second;
  }

  std::vector<::google::spanner::admin::instance::v1::InstanceConfig>
  ListCustomInstanceConfigs(const std::string& parent) const {
    absl::ReaderMutexLock lock(&instance_configs_mu_);
    std::vector<::google::spanner::admin::instance::v1::InstanceConfig> result;
    const std::string prefix = parent + "/instanceConfigs/";
    for (const auto& [name, config] : instance_configs_) {
      if (name.rfind(prefix, 0) == 0) result.push_back(config);
    }
    return result;
  }

  absl::Status UpdateInstanceConfig(
      const ::google::spanner::admin::instance::v1::InstanceConfig& config) {
    absl::MutexLock lock(&instance_configs_mu_);
    auto it = instance_configs_.find(config.name());
    if (it == instance_configs_.end()) {
      return absl::NotFoundError("Instance config not found");
    }
    it->second = config;
    return absl::OkStatus();
  }

  absl::Status DeleteInstanceConfig(const std::string& name) {
    absl::MutexLock lock(&instance_configs_mu_);
    if (instance_configs_.erase(name) == 0) {
      return absl::NotFoundError("Instance config not found");
    }
    return absl::OkStatus();
  }

  // Serializes control-plane validation, live mutation, durable persistence,
  // and rollback as one transaction across related resource types.
  absl::Mutex& admin_transaction_mutex() {
    return admin_transaction_mu_;
  }

 private:
  std::unique_ptr<Clock> clock_;
  std::unique_ptr<DatabaseManager> database_manager_;
  std::unique_ptr<InstanceManager> instance_manager_;
  std::unique_ptr<InstancePartitionManager> instance_partition_manager_;
  std::unique_ptr<OperationManager> operation_manager_;
  std::unique_ptr<SessionManager> session_manager_;
  std::unique_ptr<MultiplexedSessionTransactionManager> mux_txn_manager_;
  std::unique_ptr<MetadataStore> metadata_store_;
  std::unique_ptr<BackupCatalog> backup_catalog_;
  mutable absl::Mutex iam_policies_mu_;
  std::map<std::string, ::google::iam::v1::Policy> iam_policies_
      ABSL_GUARDED_BY(iam_policies_mu_);
  absl::Mutex admin_transaction_mu_;
  mutable absl::Mutex instance_configs_mu_;
  std::map<std::string, ::google::spanner::admin::instance::v1::InstanceConfig>
      instance_configs_ ABSL_GUARDED_BY(instance_configs_mu_);
};

}  // namespace frontend
}  // namespace emulator
}  // namespace spanner
}  // namespace google

#endif  // THIRD_PARTY_CLOUD_SPANNER_EMULATOR_FRONTEND_SERVER_ENV_H_

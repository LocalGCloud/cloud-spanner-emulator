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

#ifndef THIRD_PARTY_CLOUD_SPANNER_EMULATOR_FRONTEND_PERSISTENCE_METADATA_STORE_H_
#define THIRD_PARTY_CLOUD_SPANNER_EMULATOR_FRONTEND_PERSISTENCE_METADATA_STORE_H_

#include <functional>
#include <cstdint>
#include <map>
#include <optional>
#include <set>
#include <string>
#include <vector>

#include "absl/status/status.h"
#include "absl/synchronization/mutex.h"
#include "absl/time/time.h"
#include "google/iam/v1/policy.pb.h"
#include "google/longrunning/operations.pb.h"
#include "google/spanner/admin/instance/v1/spanner_instance_admin.pb.h"
#include "frontend/persistence/schema_change_batch.h"

namespace google {
namespace spanner {
namespace emulator {
namespace frontend {
class BackupCatalog;

// MetadataStore persists emulator metadata (instances, databases, DDL, ID
// counters) to a JSON file under --data_dir. This enables the emulator to
// restore its full state after a restart.
//
// The metadata file is written atomically via write-tmp-then-rename to prevent
// corruption on crash. The file is read once on startup and written on every
// metadata mutation.
//
// Thread-safe: all public methods acquire a mutex.
class MetadataStore {
 public:
  struct IdCounters {
    int64_t table_id = 0;
    int64_t column_id = 0;
    int64_t change_stream_id = 0;
  };

  struct DatabaseInfo {
    std::string dialect;  // "GOOGLE_STANDARD_SQL" or "POSTGRESQL"
    std::vector<std::string> ddl_statements;
    std::vector<PersistedSchemaChangeBatch> schema_change_batches;
    IdCounters id_counters;
    bool enable_drop_protection = false;
    std::string proto_descriptor_bytes;
    std::string create_time;
  };
  // A DDL request durably recorded before it mutates persistent storage.
  // Startup replays any surviving intent and atomically promotes its committed
  // schema batch and terminal operation.
  struct PendingDdlOperation {
    std::string operation_name;
    std::vector<std::string> statements;
    std::string proto_descriptor_bytes;
    bool has_rollback_checkpoint = false;
  };


  struct InstanceInfo {
    std::string display_name;
    int32_t node_count = 0;
    std::string config;
    int32_t processing_units = 1000;
    std::map<std::string, std::string> labels;
    std::string create_time;
    std::string update_time;
    std::map<std::string, DatabaseInfo> databases;
  };

  explicit MetadataStore(const std::string& data_dir);

  // Loads metadata from {data_dir}/metadata.json. A missing file is an empty
  // first-run state; an unreadable or malformed existing file is DATA_LOSS.
  absl::Status Load() ABSL_LOCKS_EXCLUDED(mu_);

  // Saves metadata to {data_dir}/metadata.json atomically.
  absl::Status Save() ABSL_LOCKS_EXCLUDED(mu_);

  // Installs a callback invoked under the store mutex before each save.
  // Intended only for deterministic persistence-failure tests.
  void SetSaveHookForTesting(std::function<absl::Status()> hook)
      ABSL_LOCKS_EXCLUDED(mu_);

  void AddInstance(const std::string& name, const std::string& display_name,
                   const std::string& config, int32_t processing_units,
                   const std::map<std::string, std::string>& labels,
                   const std::string& create_time,
                   const std::string& update_time = "")
      ABSL_LOCKS_EXCLUDED(mu_);
  void UpdateInstance(const std::string& name, const std::string& config,
                      const std::string& display_name, int32_t processing_units,
                      const std::map<std::string, std::string>& labels,
                      const std::string& update_time)
      ABSL_LOCKS_EXCLUDED(mu_);
  void UpdateInstanceNodeCount(const std::string& name, int32_t node_count)
      ABSL_LOCKS_EXCLUDED(mu_);
  void RemoveInstance(const std::string& name) ABSL_LOCKS_EXCLUDED(mu_);

  // Database operations.
  void AddDatabase(
      const std::string& instance_name, const std::string& db_name,
      const std::string& dialect,
      const std::vector<std::string>& ddl_statements,
      const std::string& proto_descriptor_bytes = "",
      const std::string& create_time = "") ABSL_LOCKS_EXCLUDED(mu_);
  void RemoveDatabase(const std::string& instance_name,
                      const std::string& db_name) ABSL_LOCKS_EXCLUDED(mu_);
  // Appends one committed schema-change request, its matching descriptor
  // bundle, and its original commit timestamp to the replay history.
  void UpdateDdl(
      const std::string& instance_name, const std::string& db_name,
      const std::vector<std::string>& statements,
      const std::string& proto_descriptor_bytes = "",
      const std::string& schema_change_timestamp = "")
      ABSL_LOCKS_EXCLUDED(mu_);
  void UpdateIdCounters(const std::string& instance_name,
                        const std::string& db_name, const IdCounters& counters)
      ABSL_LOCKS_EXCLUDED(mu_);
  void UpdateDropProtection(const std::string& instance_name,
                            const std::string& db_name, bool enabled)
      ABSL_LOCKS_EXCLUDED(mu_);
  void SetPendingDdlOperation(
      const std::string& database,
      const PendingDdlOperation& operation) ABSL_LOCKS_EXCLUDED(mu_);
  void RemovePendingDdlOperation(const std::string& database)
      ABSL_LOCKS_EXCLUDED(mu_);
  std::map<std::string, PendingDdlOperation> AllPendingDdlOperations() const
      ABSL_LOCKS_EXCLUDED(mu_);


  // IAM policies keyed by the full instance, database, or backup resource.
  void SetIamPolicy(const std::string& resource,
                    const ::google::iam::v1::Policy& policy)
      ABSL_LOCKS_EXCLUDED(mu_);
  void RemoveIamPolicy(const std::string& resource) ABSL_LOCKS_EXCLUDED(mu_);
  std::optional<::google::iam::v1::Policy> GetIamPolicy(
      const std::string& resource) const ABSL_LOCKS_EXCLUDED(mu_);
  std::map<std::string, ::google::iam::v1::Policy> AllIamPolicies() const
      ABSL_LOCKS_EXCLUDED(mu_);

  // Serialized custom InstanceConfig protos keyed by full resource name.
  void SetInstanceConfig(const std::string& name,
                         const std::string& encoded_config)
      ABSL_LOCKS_EXCLUDED(mu_);
  void RemoveInstanceConfig(const std::string& name) ABSL_LOCKS_EXCLUDED(mu_);
  std::map<std::string, std::string> instance_configs() const
      ABSL_LOCKS_EXCLUDED(mu_);

  void SetInstancePartition(
      const ::google::spanner::admin::instance::v1::InstancePartition&
          partition) ABSL_LOCKS_EXCLUDED(mu_);
  void RemoveInstancePartition(const std::string& name)
      ABSL_LOCKS_EXCLUDED(mu_);
  std::map<std::string,
           ::google::spanner::admin::instance::v1::InstancePartition>
  instance_partitions() const ABSL_LOCKS_EXCLUDED(mu_);

  // Terminal operations awaiting idempotent promotion to BackupCatalog.
  // Storing them here lets a resource mutation and its LRO share one atomic
  // metadata commit.
  void SetPendingOperation(
      const ::google::longrunning::Operation& operation)
      ABSL_LOCKS_EXCLUDED(mu_);
  void RemovePendingOperation(const std::string& name)
      ABSL_LOCKS_EXCLUDED(mu_);
  std::map<std::string, ::google::longrunning::Operation>
  AllPendingOperations() const ABSL_LOCKS_EXCLUDED(mu_);

  // Idempotently promotes journaled terminal operations into the durable
  // catalog, then clears the journal in one metadata save.
  absl::Status ReconcilePendingOperations(BackupCatalog* catalog)
      ABSL_LOCKS_EXCLUDED(mu_);

  // Backup and schedule deletion intents bridge the metadata IAM store and
  // BackupCatalog. Startup completes any intent left by a crash between the
  // two atomic file replacements.
  void SetPendingBackupDeletion(const std::string& resource)
      ABSL_LOCKS_EXCLUDED(mu_);
  void RemovePendingBackupDeletion(const std::string& resource)
      ABSL_LOCKS_EXCLUDED(mu_);
  std::set<std::string> AllPendingBackupDeletions() const
      ABSL_LOCKS_EXCLUDED(mu_);
  absl::Status ReconcilePendingBackupDeletions(BackupCatalog* catalog)
      ABSL_LOCKS_EXCLUDED(mu_);

  // Accessors for restore. Returns a copy for thread safety.
  std::map<std::string, InstanceInfo> instances() const
      ABSL_LOCKS_EXCLUDED(mu_);
  bool has_metadata() const ABSL_LOCKS_EXCLUDED(mu_);

 private:
  std::string metadata_path_;
  std::function<absl::Status()> save_hook_for_testing_ ABSL_GUARDED_BY(mu_);
  mutable absl::Mutex mu_;
  std::map<std::string, InstanceInfo> instances_ ABSL_GUARDED_BY(mu_);
  std::map<std::string, ::google::iam::v1::Policy> iam_policies_
      ABSL_GUARDED_BY(mu_);
  std::map<std::string, std::string> instance_configs_ ABSL_GUARDED_BY(mu_);
  std::map<std::string,
           ::google::spanner::admin::instance::v1::InstancePartition>
      instance_partitions_ ABSL_GUARDED_BY(mu_);
  std::map<std::string, ::google::longrunning::Operation> pending_operations_
      ABSL_GUARDED_BY(mu_);
  std::set<std::string> pending_backup_deletions_ ABSL_GUARDED_BY(mu_);
  std::map<std::string, PendingDdlOperation> pending_ddl_operations_
      ABSL_GUARDED_BY(mu_);
  bool has_metadata_ ABSL_GUARDED_BY(mu_) = false;
};

}  // namespace frontend
}  // namespace emulator
}  // namespace spanner
}  // namespace google

#endif  // THIRD_PARTY_CLOUD_SPANNER_EMULATOR_FRONTEND_PERSISTENCE_METADATA_STORE_H_

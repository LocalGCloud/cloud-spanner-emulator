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

#ifndef THIRD_PARTY_CLOUD_SPANNER_EMULATOR_FRONTEND_PERSISTENCE_BACKUP_CATALOG_H_
#define THIRD_PARTY_CLOUD_SPANNER_EMULATOR_FRONTEND_PERSISTENCE_BACKUP_CATALOG_H_

#include <cstdint>
#include <memory>
#include <map>
#include <string>
#include <vector>

#include "absl/status/status.h"
#include "absl/status/statusor.h"
#include "absl/synchronization/mutex.h"
#include "google/longrunning/operations.pb.h"
#include "google/spanner/admin/database/v1/backup.pb.h"
#include "google/spanner/admin/database/v1/backup_schedule.pb.h"
#include "frontend/persistence/schema_change_batch.h"

namespace google {
namespace spanner {
namespace emulator {
namespace frontend {

namespace database_api = ::google::spanner::admin::database::v1;

// Durable control metadata for native Spanner backups. Snapshot payloads live
// in immutable LevelDB directories beside this catalog and are never copied
// into the JSON metadata file.
class BackupCatalog {
 public:
  struct IdCounters {
    int64_t table_id = 0;
    int64_t column_id = 0;
    int64_t change_stream_id = 0;
  };

  struct BackupEntry {
    database_api::Backup backup;
    std::vector<std::string> ddl_statements;
    std::vector<PersistedSchemaChangeBatch> schema_change_batches;
    std::string proto_descriptor_bytes;
    database_api::DatabaseDialect dialect =
        database_api::DatabaseDialect::GOOGLE_STANDARD_SQL;
    IdCounters id_counters;
    std::string operation_name;
    std::string source_instance_config;
  };
  // Holds the catalog mutex for the lifetime of a validated source snapshot.
  // This serializes LevelDB validation opens and prevents deletion while a
  // caller copies the immutable snapshot.
  class ABSL_SCOPED_LOCKABLE SnapshotLease {
   public:
    SnapshotLease(const SnapshotLease&) = delete;
    SnapshotLease& operator=(const SnapshotLease&) = delete;
    ~SnapshotLease() ABSL_UNLOCK_FUNCTION() { mutex_->Unlock(); }

    const BackupEntry& entry() const { return entry_; }
    const std::string& snapshot_directory() const {
      return snapshot_directory_;
    }

   private:
    friend class BackupCatalog;
    explicit SnapshotLease(absl::Mutex* mutex)
        ABSL_EXCLUSIVE_LOCK_FUNCTION(*mutex)
        : mutex_(mutex) {
      mutex_->Lock();
    }

    absl::Mutex* mutex_;
    BackupEntry entry_;
    std::string snapshot_directory_;
  };

  absl::StatusOr<std::unique_ptr<SnapshotLease>> AcquireSnapshot(
      const std::string& backup_name) const ABSL_NO_THREAD_SAFETY_ANALYSIS;


  explicit BackupCatalog(std::string data_dir);

  absl::Status Load() ABSL_LOCKS_EXCLUDED(mu_);

  absl::Status CreateBackup(
      BackupEntry entry, const google::longrunning::Operation& operation)
      ABSL_LOCKS_EXCLUDED(mu_);
  absl::StatusOr<BackupEntry> GetBackup(const std::string& name) const
      ABSL_LOCKS_EXCLUDED(mu_);
  std::vector<BackupEntry> ListBackups(const std::string& parent) const
      ABSL_LOCKS_EXCLUDED(mu_);
  std::vector<BackupEntry> AllBackups() const ABSL_LOCKS_EXCLUDED(mu_);
  std::vector<google::longrunning::Operation> AllOperations() const
      ABSL_LOCKS_EXCLUDED(mu_);
  absl::Status SaveOperation(
      const google::longrunning::Operation& operation)
      ABSL_LOCKS_EXCLUDED(mu_);
  absl::Status DeleteOperation(const std::string& operation_name)
      ABSL_LOCKS_EXCLUDED(mu_);
  absl::Status UpdateBackup(const database_api::Backup& backup)
      ABSL_LOCKS_EXCLUDED(mu_);
  absl::Status DeleteBackup(const std::string& name) ABSL_LOCKS_EXCLUDED(mu_);

  absl::Status CreateBackupSchedule(database_api::BackupSchedule schedule)
      ABSL_LOCKS_EXCLUDED(mu_);
  absl::StatusOr<database_api::BackupSchedule> GetBackupSchedule(
      const std::string& name) const ABSL_LOCKS_EXCLUDED(mu_);
  std::vector<database_api::BackupSchedule> ListBackupSchedules(
      const std::string& parent) const ABSL_LOCKS_EXCLUDED(mu_);
  absl::Status UpdateBackupSchedule(
      const database_api::BackupSchedule& schedule) ABSL_LOCKS_EXCLUDED(mu_);
  absl::Status DeleteBackupSchedule(const std::string& name)
      ABSL_LOCKS_EXCLUDED(mu_);

  bool persistent() const { return !data_dir_.empty(); }
  std::string SnapshotDirectory(const std::string& backup_name) const;

  absl::Status ValidateSnapshot(const std::string& backup_name) const;

 private:
  absl::Status SaveLocked() const ABSL_EXCLUSIVE_LOCKS_REQUIRED(mu_);
  absl::Status CleanupStaleSnapshotsLocked(
      const std::map<std::string, BackupEntry>& retained_backups)
      ABSL_EXCLUSIVE_LOCKS_REQUIRED(mu_);
  absl::Status ValidateSnapshotLocked(const std::string& backup_name) const
      ABSL_EXCLUSIVE_LOCKS_REQUIRED(mu_);
  static std::string ResourceParent(const std::string& resource_name);
  static std::string EncodePathComponent(const std::string& value);

  const std::string data_dir_;
  const std::string catalog_path_;
  mutable absl::Mutex mu_;
  std::map<std::string, BackupEntry> backups_ ABSL_GUARDED_BY(mu_);
  std::map<std::string, database_api::BackupSchedule> schedules_
      ABSL_GUARDED_BY(mu_);
  std::map<std::string, google::longrunning::Operation> operations_
      ABSL_GUARDED_BY(mu_);
};

}  // namespace frontend
}  // namespace emulator
}  // namespace spanner
}  // namespace google

#endif  // THIRD_PARTY_CLOUD_SPANNER_EMULATOR_FRONTEND_PERSISTENCE_BACKUP_CATALOG_H_

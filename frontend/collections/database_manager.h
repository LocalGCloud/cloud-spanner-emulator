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

#ifndef THIRD_PARTY_CLOUD_SPANNER_EMULATOR_FRONTEND_DATABASE_MANAGER_H_
#define THIRD_PARTY_CLOUD_SPANNER_EMULATOR_FRONTEND_DATABASE_MANAGER_H_

#include <map>
#include <memory>
#include <string>
#include <vector>
#include <utility>

#include "absl/base/thread_annotations.h"
#include "absl/container/flat_hash_map.h"
#include "absl/flags/flag.h"
#include "absl/status/status.h"
#include "absl/status/statusor.h"
#include "absl/synchronization/mutex.h"
#include "backend/database/database.h"
#include "backend/schema/updater/schema_updater.h"
#include "common/clock.h"
#include "frontend/entities/database.h"
#include "absl/time/time.h"
#include "absl/status/status.h"

// If set, and it's greater than limits::kMaxDatabasesPerInstance, then
// override the maximum number of database that can be created per instance.
ABSL_DECLARE_FLAG(int, override_max_databases_per_instance);

namespace google {
namespace spanner {
namespace emulator {
namespace frontend {

// DatabaseManager manages the set of active databases in the emulator.
class DatabaseManager {
 public:
  explicit DatabaseManager(Clock* clock, std::string data_dir = "")
      : clock_(clock), data_dir_(std::move(data_dir)) {}
  // Holds a manager-visible database creation reservation. The database is
  // invisible to reads until Publish(), while competing create/drop requests
  // are rejected for the reserved resource.
  class Creation {
   public:
    ~Creation();
    Creation(const Creation&) = delete;
    Creation& operator=(const Creation&) = delete;

    absl::StatusOr<std::shared_ptr<Database>> Build(
        const backend::SchemaChangeOperation& schema_change_operation,
        const backend::Database::IdCounterValues& id_counters);
    absl::StatusOr<std::shared_ptr<Database>> Build(
        const backend::SchemaChangeOperation& schema_change_operation,
        const backend::Database::IdCounterValues& id_counters,
        absl::Time create_time);
    absl::StatusOr<std::shared_ptr<Database>> Build(
        const std::vector<backend::SchemaChangeOperation>&
            schema_change_operations,
        const backend::Database::IdCounterValues& id_counters,
        absl::Time create_time);
    absl::Status Publish();

   private:
    friend class DatabaseManager;
    Creation(DatabaseManager* manager, std::string database_uri,
             std::string instance_uri)
        : manager_(manager),
          database_uri_(std::move(database_uri)),
          instance_uri_(std::move(instance_uri)) {}

    DatabaseManager* manager_;
    std::string database_uri_;
    std::string instance_uri_;
    std::shared_ptr<Database> database_;
    bool published_ = false;
  };

  absl::StatusOr<std::unique_ptr<Creation>> ReserveDatabase(
      const std::string& database_uri) ABSL_LOCKS_EXCLUDED(mu_);


  // Creates a database with a schema initialized from `create_statements`.
  absl::StatusOr<std::shared_ptr<Database>> CreateDatabase(
      const std::string& database_uri,
      const backend::SchemaChangeOperation& schema_change_operation)
      ABSL_LOCKS_EXCLUDED(mu_);

  // Overload that seeds ID generators from persisted counter values.
  // Used during restore to ensure IDs match existing data in LevelDB.
  absl::StatusOr<std::shared_ptr<Database>> CreateDatabase(
      const std::string& database_uri,
      const backend::SchemaChangeOperation& schema_change_operation,
      const backend::Database::IdCounterValues& id_counters)
      ABSL_LOCKS_EXCLUDED(mu_);

  // Restore overload that preserves the database's output-only create time.
  absl::StatusOr<std::shared_ptr<Database>> CreateDatabase(
      const std::string& database_uri,
      const backend::SchemaChangeOperation& schema_change_operation,
      const backend::Database::IdCounterValues& id_counters,
      absl::Time create_time) ABSL_LOCKS_EXCLUDED(mu_);

  // Moves legacy <data_dir>/<database-id> directories into resource-scoped
  // locations. A legacy directory must map to exactly one persisted resource.
  static absl::Status MigrateLegacyStorageDirectories(
      const std::string& data_dir,
      const std::vector<std::string>& database_uris);

  // Atomically records that a persistent database root has corresponding
  // durable metadata. Startup uses this marker to distinguish incomplete
  // creation roots from metadata loss.
  static absl::Status MarkDatabaseMetadataCommitted(
      const std::string& data_dir, const std::string& database_uri);

  // Removes restore staging/database roots that still carry an in-progress
  // marker but have no corresponding persisted database resource.
  static absl::Status CleanupOrphanedRestoreDirectories(
      const std::string& data_dir,
      const std::vector<std::string>& database_uris);

  // Persists or rolls back a database-deletion intent. Startup reconciliation
  // removes marked roots absent from metadata and clears markers for resources
  // whose metadata deletion did not commit.
  static absl::Status MarkDatabaseForDeletion(
      const std::string& data_dir, const std::string& database_uri);
  static absl::Status CancelDatabaseDeletion(
      const std::string& data_dir, const std::string& database_uri);
  static absl::Status ReconcileDeletedDatabaseDirectories(
      const std::string& data_dir,
      const std::vector<std::string>& database_uris);

  // Clears restore markers for persisted databases after pending terminal
  // operations have been promoted to the durable operation catalog.
  static absl::Status CompleteRecoveredRestoreDirectories(
      const std::string& data_dir,
      const std::vector<std::string>& database_uris);

  // Durable DDL uses a database-local storage checkpoint so startup can
  // restore the pre-mutation state before replaying a pending operation.
  static absl::StatusOr<std::string> DdlRollbackCheckpointDirectory(
      const std::string& data_dir, const std::string& database_uri,
      const std::string& operation_name);
  static absl::Status RestoreDdlRollbackCheckpoint(
      const std::string& data_dir, const std::string& database_uri,
      const std::string& operation_name);
  static absl::Status RemoveDdlRollbackCheckpoints(
      const std::string& data_dir, const std::string& database_uri);

  // Returns a database with the given URI.
  absl::StatusOr<std::shared_ptr<Database>> GetDatabase(
      const std::string& database_uri) const ABSL_LOCKS_EXCLUDED(mu_);

  // Returns a database even when it is quarantined for restart recovery.
  // Reserved for destructive admin cleanup that must still inspect protection.
  absl::StatusOr<std::shared_ptr<Database>>
  GetDatabaseIncludingRecoveryRequired(
      const std::string& database_uri) const ABSL_LOCKS_EXCLUDED(mu_);

  // Deletes a database with the given URI.
  absl::Status DeleteDatabase(const std::string& database_uri)
      ABSL_LOCKS_EXCLUDED(mu_);

  // Lists all databases associated with the given instance URI.
  absl::StatusOr<std::vector<std::shared_ptr<Database>>> ListDatabases(
      const std::string& instance_uri) const ABSL_LOCKS_EXCLUDED(mu_);

 private:
  // System-wide clock.
  Clock* clock_;

  // Persistent root used to reject recreation while durable deletion cleanup
  // remains unfinished. Empty for in-memory managers.
  std::string data_dir_;

  // Mutex to guard state below.
  mutable absl::Mutex mu_;

  // Map from database URI to database objects.
  std::map<std::string, std::shared_ptr<Database>> database_map_
      ABSL_GUARDED_BY(mu_);
  // Database URIs reserved by in-flight create/restore operations.
  absl::flat_hash_map<std::string, std::string> database_reservations_
      ABSL_GUARDED_BY(mu_);


  // Count of databases per instance.
  absl::flat_hash_map<std::string, int> num_databases_per_instance_
      ABSL_GUARDED_BY(mu_);
  absl::flat_hash_map<std::string, int>
      num_database_reservations_per_instance_ ABSL_GUARDED_BY(mu_);
};

}  // namespace frontend
}  // namespace emulator
}  // namespace spanner
}  // namespace google

#endif  // THIRD_PARTY_CLOUD_SPANNER_EMULATOR_FRONTEND_DATABASE_MANAGER_H_

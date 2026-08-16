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

#include <atomic>
#include <algorithm>
#include <chrono>
#include <cctype>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <memory>
#include <string>
#include <optional>
#include <system_error>
#include <vector>
#include <utility>
#include "absl/log/absl_log.h"

#include "absl/status/status.h"
#include "absl/time/time.h"
#include "absl/strings/match.h"
#include "absl/strings/str_cat.h"
#include "absl/synchronization/mutex.h"
#include "backend/database/database.h"
#include "backend/schema/printer/print_ddl.h"
#include "common/config.h"
#include "frontend/collections/database_manager.h"
#include "frontend/collections/operation_manager.h"
#include "frontend/common/uris.h"
#include "frontend/converters/time.h"
#include "frontend/entities/database.h"
#include "frontend/entities/instance.h"
#include "frontend/persistence/backup_catalog.h"
#include "frontend/persistence/metadata_store.h"
#include "frontend/server/handler.h"
#include "google/longrunning/operations.pb.h"
#include "google/iam/v1/policy.pb.h"
#include "google/protobuf/empty.pb.h"
#include "google/spanner/admin/database/v1/backup.pb.h"
#include "google/spanner/admin/database/v1/backup_schedule.pb.h"
#include "google/spanner/admin/database/v1/spanner_database_admin.pb.h"
#include "googlesql/base/status_macros.h"

namespace database_api = ::google::spanner::admin::database::v1;
namespace iam_api = ::google::iam::v1;
namespace instance_api = ::google::spanner::admin::instance::v1;
namespace operations_api = ::google::longrunning;
namespace protobuf_api = ::google::protobuf;

namespace google {
namespace spanner {
namespace emulator {
namespace frontend {
namespace {

constexpr int32_t kMaximumPageSize = 1000;
constexpr absl::Duration kMinimumBackupRetention = absl::Hours(6);
constexpr absl::Duration kMaximumBackupRetention = absl::Hours(24 * 366);

absl::Status ValidateResourceId(const std::string& id,
                                const std::string& description) {
  if (id.empty()) {
    return absl::InvalidArgumentError(
        absl::StrCat(description, " must not be empty"));
  }
  if (id.size() > 100) {
    return absl::InvalidArgumentError(
        absl::StrCat(description, " is too long"));
  }
  for (unsigned char ch : id) {
    if (!(std::isalnum(ch) || ch == '-' || ch == '_')) {
      return absl::InvalidArgumentError(
          absl::StrCat(description, " contains an invalid character: ", id));
    }
  }
  return absl::OkStatus();
}

absl::Status ValidateInstance(const std::string& parent, ServerEnv* env) {
  absl::string_view project_id;
  absl::string_view instance_id;
  GOOGLESQL_RETURN_IF_ERROR(
      ParseInstanceUri(parent, &project_id, &instance_id));
  if (MakeInstanceUri(project_id, instance_id) != parent) {
    return absl::InvalidArgumentError("Instance name must be canonical");
  }
  return env->instance_manager()->GetInstance(parent).status();
}

absl::Status ValidateDatabase(const std::string& name, ServerEnv* env) {
  absl::string_view project_id;
  absl::string_view instance_id;
  absl::string_view database_id;
  GOOGLESQL_RETURN_IF_ERROR(
      ParseDatabaseUri(name, &project_id, &instance_id, &database_id));
  if (MakeDatabaseUri(MakeInstanceUri(project_id, instance_id), database_id) !=
      name) {
    return absl::InvalidArgumentError("Database name must be canonical");
  }
  return env->database_manager()->GetDatabase(name).status();
}

std::string MakeBackupName(const std::string& parent,
                           const std::string& backup_id) {
  return absl::StrCat(parent, "/backups/", backup_id);
}

std::string MakeBackupScheduleName(const std::string& parent,
                                   const std::string& schedule_id) {
  return absl::StrCat(parent, "/backupSchedules/", schedule_id);
}

absl::Status ValidateBackupName(const std::string& name) {
  const size_t marker = name.rfind("/backups/");
  if (marker == std::string::npos || marker == 0 ||
      marker + std::string("/backups/").size() >= name.size()) {
    return absl::InvalidArgumentError(
        absl::StrCat("Invalid backup resource name: ", name));
  }
  const std::string parent = name.substr(0, marker);
  absl::string_view project_id;
  absl::string_view instance_id;
  absl::Status parse_status =
      ParseInstanceUri(parent, &project_id, &instance_id);
  if (!parse_status.ok() ||
      MakeInstanceUri(project_id, instance_id) != parent) {
    return absl::InvalidArgumentError(
        absl::StrCat("Invalid backup resource name: ", name));
  }
  return ValidateResourceId(name.substr(marker + 9), "Backup ID");
}

absl::Status ValidateBackupScheduleName(const std::string& name) {
  const size_t marker = name.rfind("/backupSchedules/");
  if (marker == std::string::npos || marker == 0 ||
      marker + std::string("/backupSchedules/").size() >= name.size()) {
    return absl::InvalidArgumentError(
        absl::StrCat("Invalid backup schedule resource name: ", name));
  }
  const std::string parent = name.substr(0, marker);
  absl::string_view project_id;
  absl::string_view instance_id;
  absl::string_view database_id;
  absl::Status parse_status =
      ParseDatabaseUri(parent, &project_id, &instance_id, &database_id);
  if (!parse_status.ok() ||
      MakeDatabaseUri(MakeInstanceUri(project_id, instance_id), database_id) !=
          parent) {
    return absl::InvalidArgumentError(
        absl::StrCat("Invalid backup schedule resource name: ", name));
  }
  return ValidateResourceId(name.substr(marker + 17), "Backup schedule ID");
}

absl::StatusOr<int64_t> DirectorySize(const std::string& directory) {
  int64_t bytes = 0;
  std::error_code error;
  for (std::filesystem::recursive_directory_iterator iterator(directory, error),
       end;
       iterator != end && !error; iterator.increment(error)) {
    if (iterator->is_regular_file(error)) {
      bytes += static_cast<int64_t>(iterator->file_size(error));

    }
  }
  if (error) {
    return absl::InternalError(
        absl::StrCat("Failed to inspect backup snapshot ", directory, ": ",
                     error.message()));
  }
  return bytes;
}

absl::Status ValidateBackupExpiration(absl::Time expire_time,
                                      absl::Time create_time) {
  if (expire_time < create_time + kMinimumBackupRetention ||
      expire_time > create_time + kMaximumBackupRetention) {
    return absl::InvalidArgumentError(
        "Backup expire_time must be between 6 hours and 366 days after "
        "create_time");
  }
  return absl::OkStatus();
}
class DirectoryCleanup {
 public:
  explicit DirectoryCleanup(std::filesystem::path path)
      : path_(std::move(path)) {}
  DirectoryCleanup(const DirectoryCleanup&) = delete;
  DirectoryCleanup& operator=(const DirectoryCleanup&) = delete;
  ~DirectoryCleanup() {
    if (armed_) {
      std::error_code ignored;
      std::filesystem::remove_all(path_, ignored);
    }
  }
  void Disarm() { armed_ = false; }

 private:
  std::filesystem::path path_;
  bool armed_ = true;
};

absl::Status CopySnapshot(const std::string& source,
                          const std::string& destination) {
  if (!std::filesystem::exists(source)) {
    return absl::DataLossError(
        absl::StrCat("Backup snapshot is missing: ", source));
  }
  if (std::filesystem::exists(destination)) {
    return absl::AlreadyExistsError(
        absl::StrCat("Snapshot destination already exists: ", destination));
  }

  std::error_code error;
  std::filesystem::create_directories(
      std::filesystem::path(destination).parent_path(), error);
  if (error) {
    return absl::InternalError(
        absl::StrCat("Failed to create snapshot parent: ", error.message()));
  }

  static std::atomic<uint64_t> temporary_sequence{0};
  const std::filesystem::path temporary_directory = absl::StrCat(
      destination, ".tmp-",
      std::chrono::steady_clock::now().time_since_epoch().count(), "-",
      temporary_sequence.fetch_add(1));
  std::filesystem::copy(source, temporary_directory,
                        std::filesystem::copy_options::recursive, error);
  if (!error) {
    std::filesystem::rename(temporary_directory, destination, error);
  }
  if (error) {
    const std::string message = error.message();
    std::error_code ignored;
    std::filesystem::remove_all(temporary_directory, ignored);
    std::error_code exists_error;
    if (std::filesystem::exists(destination, exists_error) && !exists_error) {
      return absl::AlreadyExistsError(
          absl::StrCat("Snapshot destination already exists: ", destination));
    }
    return absl::InternalError(
        absl::StrCat("Failed to copy backup snapshot: ", message));
  }
  return absl::OkStatus();
}

std::string OperationName(const std::shared_ptr<Operation>& operation) {
  operations_api::Operation proto;
  operation->ToProto(&proto);
  return proto.name();
}

absl::Status PersistRestoredDatabase(
    ServerEnv* env, const std::string& parent, const std::string& database_id,
    const BackupCatalog::BackupEntry& entry,
    const database_api::Database& restored) {
  MetadataStore* metadata = env->metadata_store();
  if (metadata == nullptr) return absl::OkStatus();
  GOOGLESQL_ASSIGN_OR_RETURN(const absl::Time create_time,
                             TimestampFromProto(restored.create_time()));
  if (entry.schema_change_batches.empty()) {
    metadata->AddDatabase(
        parent, database_id,
        entry.dialect == database_api::DatabaseDialect::POSTGRESQL
            ? "POSTGRESQL"
            : "GOOGLE_STANDARD_SQL",
        entry.ddl_statements, entry.proto_descriptor_bytes,
        absl::FormatTime(absl::RFC3339_full, create_time,
                         absl::UTCTimeZone()));
  } else {
    const PersistedSchemaChangeBatch& initial =
        entry.schema_change_batches.front();
    metadata->AddDatabase(
        parent, database_id,
        entry.dialect == database_api::DatabaseDialect::POSTGRESQL
            ? "POSTGRESQL"
            : "GOOGLE_STANDARD_SQL",
        initial.statements, initial.proto_descriptor_bytes,
        absl::FormatTime(absl::RFC3339_full, create_time,
                         absl::UTCTimeZone()));
    for (std::size_t index = 1;
         index < entry.schema_change_batches.size(); ++index) {
      const PersistedSchemaChangeBatch& batch =
          entry.schema_change_batches[index];
      metadata->UpdateDdl(parent, database_id, batch.statements,
                          batch.proto_descriptor_bytes,
                          batch.schema_change_timestamp);
    }
  }
  metadata->UpdateIdCounters(
      parent, database_id,
      MetadataStore::IdCounters{
          .table_id = entry.id_counters.table_id,
          .column_id = entry.id_counters.column_id,
          .change_stream_id = entry.id_counters.change_stream_id,
      });
  return absl::OkStatus();
}


}  // namespace

absl::Status CreateBackup(RequestContext* ctx,
                          const database_api::CreateBackupRequest* request,
                          operations_api::Operation* response) {
  absl::MutexLock admin_transaction_lock(
      &ctx->env()->admin_transaction_mutex());
  GOOGLESQL_RETURN_IF_ERROR(ValidateInstance(request->parent(), ctx->env()));
  GOOGLESQL_RETURN_IF_ERROR(
      ValidateResourceId(request->backup_id(), "Backup ID"));
  if (!request->has_backup() || request->backup().database().empty()) {
    return absl::InvalidArgumentError("Backup database must be provided");
  }
  if (request->backup().has_version_time()) {
    return absl::InvalidArgumentError(
        "Historical backup version_time is not supported by the emulator");
  }
  if (!request->backup().has_expire_time()) {
    return absl::InvalidArgumentError("Backup expire_time must be provided");
  }

  const std::string backup_name =
      MakeBackupName(request->parent(), request->backup_id());
  GOOGLESQL_RETURN_IF_ERROR(ValidateBackupName(backup_name));

  absl::string_view project_id;
  absl::string_view instance_id;
  absl::string_view database_id;
  GOOGLESQL_RETURN_IF_ERROR(ParseDatabaseUri(
      request->backup().database(), &project_id, &instance_id, &database_id));
  if (MakeInstanceUri(project_id, instance_id) != request->parent() ||
      MakeDatabaseUri(MakeInstanceUri(project_id, instance_id), database_id) !=
          request->backup().database()) {
    return absl::InvalidArgumentError(
        "Backup database must be canonical and belong to the parent instance");
  }

  GOOGLESQL_ASSIGN_OR_RETURN(
      std::shared_ptr<Database> database,
      ctx->env()->database_manager()->GetDatabase(
          request->backup().database()));
  GOOGLESQL_ASSIGN_OR_RETURN(
      std::shared_ptr<Instance> source_instance,
      ctx->env()->instance_manager()->GetInstance(request->parent()));
  BackupCatalog* catalog = ctx->env()->backup_catalog();
  if (!catalog->persistent()) {
    return absl::FailedPreconditionError(
        "Native backups require emulator --data_dir persistent storage");
  }
  auto existing = catalog->GetBackup(backup_name);
  if (existing.ok()) {
    return absl::AlreadyExistsError(
        absl::StrCat("Backup already exists: ", backup_name));
  }
  if (existing.status().code() != absl::StatusCode::kNotFound) {
    return existing.status();
  }

  GOOGLESQL_ASSIGN_OR_RETURN(
      absl::Time expire_time,
      TimestampFromProto(request->backup().expire_time()));
  if (expire_time <= ctx->env()->clock()->Now()) {
    return absl::InvalidArgumentError(
        "Backup expire_time must be after the capture time");
  }
  GOOGLESQL_RETURN_IF_ERROR(
      ValidateBackupExpiration(expire_time, ctx->env()->clock()->Now()));

  absl::MutexLock schema_change_lock(&database->schema_change_mutex());
  const std::string snapshot_directory =
      catalog->SnapshotDirectory(backup_name);
  GOOGLESQL_ASSIGN_OR_RETURN(
      const absl::Time capture_time,
      database->backend()->CreateBackupCheckpoint(snapshot_directory));
  DirectoryCleanup snapshot_cleanup(
      std::filesystem::path(snapshot_directory).parent_path());

  GOOGLESQL_RETURN_IF_ERROR(
      ValidateBackupExpiration(expire_time, capture_time));
  GOOGLESQL_ASSIGN_OR_RETURN(auto capture_timestamp,
                             TimestampToProto(capture_time));

  BackupCatalog::BackupEntry entry;
  entry.backup = request->backup();
  entry.backup.set_name(backup_name);
  entry.backup.set_database_dialect(database->backend()->dialect());
  entry.backup.set_state(database_api::Backup::READY);
  *entry.backup.mutable_create_time() = capture_timestamp;
  *entry.backup.mutable_version_time() = capture_timestamp;
  GOOGLESQL_ASSIGN_OR_RETURN(int64_t size_bytes,
                             DirectorySize(snapshot_directory));
  entry.backup.set_size_bytes(size_bytes);
  GOOGLESQL_ASSIGN_OR_RETURN(
      entry.proto_descriptor_bytes,
      database->backend()
          ->GetLatestSchema()
          ->proto_bundle()
          ->GetProtoDescriptorBytes());

  bool found_persisted_ddl = false;
  if (auto* metadata = ctx->env()->metadata_store(); metadata != nullptr) {
    auto persisted_instances = metadata->instances();
    auto instance_it = persisted_instances.find(request->parent());
    if (instance_it != persisted_instances.end()) {
      auto database_it =
          instance_it->second.databases.find(std::string(database_id));
      if (database_it != instance_it->second.databases.end()) {
        entry.ddl_statements = database_it->second.ddl_statements;
        entry.schema_change_batches =
            database_it->second.schema_change_batches;
        found_persisted_ddl = true;
      }
    }
  }
  if (!found_persisted_ddl) {
    GOOGLESQL_ASSIGN_OR_RETURN(
        entry.ddl_statements,
        backend::PrintDDLStatements(database->backend()->GetLatestSchema()));
    entry.schema_change_batches.push_back(
        {.statements = entry.ddl_statements,
         .proto_descriptor_bytes = entry.proto_descriptor_bytes});
  }
  entry.dialect = database->backend()->dialect();
  const backend::Database::IdCounterValues counters =
      database->backend()->GetIdCounterValues();
  entry.id_counters.table_id = counters.table_id;
  entry.id_counters.column_id = counters.column_id;
  entry.id_counters.change_stream_id = counters.change_stream_id;
  instance_api::Instance source_instance_proto;
  source_instance->ToProto(&source_instance_proto);
  entry.source_instance_config = source_instance_proto.config();

  GOOGLESQL_ASSIGN_OR_RETURN(
      std::shared_ptr<Operation> operation,
      ctx->env()->operation_manager()->CreateOperation(
          backup_name, OperationManager::kAutoGeneratedId));
  entry.operation_name = OperationName(operation);
  operation->SetResponse(entry.backup);
  operations_api::Operation persisted_operation;
  operation->ToProto(&persisted_operation);
  absl::Status catalog_status =
      catalog->CreateBackup(entry, persisted_operation);
  if (!catalog_status.ok()) {
    ctx->env()->operation_manager()->DeleteOperation(entry.operation_name);
    return catalog_status;
  }

  snapshot_cleanup.Disarm();
  *response = std::move(persisted_operation);
  return absl::OkStatus();
}
REGISTER_GRPC_HANDLER(DatabaseAdmin, CreateBackup);

absl::Status GetBackup(RequestContext* ctx,
                       const database_api::GetBackupRequest* request,
                       database_api::Backup* response) {
  GOOGLESQL_RETURN_IF_ERROR(ValidateBackupName(request->name()));
  GOOGLESQL_ASSIGN_OR_RETURN(
      BackupCatalog::BackupEntry entry,
      ctx->env()->backup_catalog()->GetBackup(request->name()));
  *response = entry.backup;
  return absl::OkStatus();
}
REGISTER_GRPC_HANDLER(DatabaseAdmin, GetBackup);

absl::Status ListBackups(RequestContext* ctx,
                         const database_api::ListBackupsRequest* request,
                         database_api::ListBackupsResponse* response) {
  GOOGLESQL_RETURN_IF_ERROR(ValidateInstance(request->parent(), ctx->env()));
  int32_t page_size = request->page_size();
  if (page_size <= 0 || page_size > kMaximumPageSize) {
    page_size = kMaximumPageSize;
  }
  for (const auto& entry :
       ctx->env()->backup_catalog()->ListBackups(request->parent())) {
    if (!request->page_token().empty() &&
        entry.backup.name() < request->page_token()) {
      continue;
    }
    if (response->backups_size() >= page_size) {
      response->set_next_page_token(entry.backup.name());
      break;
    }
    *response->add_backups() = entry.backup;
  }
  return absl::OkStatus();
}
REGISTER_GRPC_HANDLER(DatabaseAdmin, ListBackups);

absl::Status UpdateBackup(RequestContext* ctx,
                          const database_api::UpdateBackupRequest* request,
                          database_api::Backup* response) {
  absl::MutexLock admin_transaction_lock(
      &ctx->env()->admin_transaction_mutex());
  if (!request->has_backup()) {
    return absl::InvalidArgumentError("Backup must be provided");
  }
  GOOGLESQL_RETURN_IF_ERROR(ValidateBackupName(request->backup().name()));
  if (request->update_mask().paths().empty()) {
    return absl::InvalidArgumentError("Backup update_mask must be provided");
  }
  GOOGLESQL_ASSIGN_OR_RETURN(
      BackupCatalog::BackupEntry entry,
      ctx->env()->backup_catalog()->GetBackup(request->backup().name()));
  for (const std::string& path : request->update_mask().paths()) {
    if (path != "expire_time") {
      return absl::InvalidArgumentError(
          absl::StrCat("Unsupported backup update field: ", path));
    }
  }
  if (!request->backup().has_expire_time()) {
    return absl::InvalidArgumentError("Backup expire_time must be provided");
  }
  *entry.backup.mutable_expire_time() = request->backup().expire_time();
  GOOGLESQL_ASSIGN_OR_RETURN(
      const absl::Time create_time,
      TimestampFromProto(entry.backup.create_time()));
  GOOGLESQL_ASSIGN_OR_RETURN(
      const absl::Time expire_time,
      TimestampFromProto(entry.backup.expire_time()));
  GOOGLESQL_RETURN_IF_ERROR(
      ValidateBackupExpiration(expire_time, create_time));
  GOOGLESQL_RETURN_IF_ERROR(
      ctx->env()->backup_catalog()->UpdateBackup(entry.backup));
  *response = entry.backup;
  return absl::OkStatus();
}
REGISTER_GRPC_HANDLER(DatabaseAdmin, UpdateBackup);

absl::Status DeleteBackup(RequestContext* ctx,
                          const database_api::DeleteBackupRequest* request,
                          protobuf_api::Empty* response) {
  GOOGLESQL_RETURN_IF_ERROR(ValidateBackupName(request->name()));
  absl::MutexLock admin_transaction_lock(
      &ctx->env()->admin_transaction_mutex());
  GOOGLESQL_RETURN_IF_ERROR(
      ctx->env()->backup_catalog()->GetBackup(request->name()).status());

  MetadataStore* metadata = ctx->env()->metadata_store();
  const auto previous_metadata_policy =
      metadata == nullptr ? std::optional<iam_api::Policy>()
                          : metadata->GetIamPolicy(request->name());

  if (metadata != nullptr) {
    metadata->RemoveIamPolicy(request->name());
    metadata->SetPendingBackupDeletion(request->name());
    absl::Status metadata_status = metadata->Save();
    if (!metadata_status.ok()) {
      metadata->RemovePendingBackupDeletion(request->name());
      if (previous_metadata_policy.has_value()) {
        metadata->SetIamPolicy(request->name(), *previous_metadata_policy);
      }
      return metadata_status;
    }
  }

  absl::Status delete_status =
      ctx->env()->backup_catalog()->DeleteBackup(request->name());
  if (!delete_status.ok()) {
    if (metadata != nullptr) {
      metadata->RemovePendingBackupDeletion(request->name());
      if (previous_metadata_policy.has_value()) {
        metadata->SetIamPolicy(request->name(), *previous_metadata_policy);
      }
      absl::Status rollback_status = metadata->Save();
      if (!rollback_status.ok()) {
        return absl::DataLossError(absl::StrCat(
            delete_status.message(),
            "; failed to roll back backup deletion intent: ",
            rollback_status.message()));
      }
    }
    return delete_status;
  }

  ctx->env()->RemoveIamPolicies(request->name());
  if (metadata != nullptr) {
    metadata->RemovePendingBackupDeletion(request->name());
    GOOGLESQL_RETURN_IF_ERROR(metadata->Save());
  }
  return absl::OkStatus();
}
REGISTER_GRPC_HANDLER(DatabaseAdmin, DeleteBackup);

absl::Status CopyBackup(RequestContext* ctx,
                        const database_api::CopyBackupRequest* request,
                        operations_api::Operation* response) {
  absl::MutexLock admin_transaction_lock(
      &ctx->env()->admin_transaction_mutex());
  GOOGLESQL_RETURN_IF_ERROR(ValidateInstance(request->parent(), ctx->env()));
  GOOGLESQL_RETURN_IF_ERROR(
      ValidateResourceId(request->backup_id(), "Backup ID"));
  GOOGLESQL_RETURN_IF_ERROR(ValidateBackupName(request->source_backup()));
  if (!request->has_expire_time()) {
    return absl::InvalidArgumentError("Copied backup expire_time is required");
  }

  BackupCatalog* catalog = ctx->env()->backup_catalog();
  const std::string name =
      MakeBackupName(request->parent(), request->backup_id());
  GOOGLESQL_RETURN_IF_ERROR(ValidateBackupName(name));
  auto existing = catalog->GetBackup(name);
  if (existing.ok()) {
    return absl::AlreadyExistsError(
        absl::StrCat("Backup already exists: ", name));
  }
  if (existing.status().code() != absl::StatusCode::kNotFound) {
    return existing.status();
  }

  GOOGLESQL_ASSIGN_OR_RETURN(
      std::unique_ptr<BackupCatalog::SnapshotLease> source_lease,
      catalog->AcquireSnapshot(request->source_backup()));
  BackupCatalog::BackupEntry source = source_lease->entry();
  if (source.backup.state() != database_api::Backup::READY) {
    return absl::FailedPreconditionError(
        "Source backup must be in READY state");
  }
  GOOGLESQL_ASSIGN_OR_RETURN(
      absl::Time source_create_time,
      TimestampFromProto(source.backup.create_time()));
  GOOGLESQL_ASSIGN_OR_RETURN(
      absl::Time expire_time, TimestampFromProto(request->expire_time()));
  GOOGLESQL_RETURN_IF_ERROR(
      ValidateBackupExpiration(expire_time, source_create_time));

  const std::string destination_directory = catalog->SnapshotDirectory(name);
  GOOGLESQL_RETURN_IF_ERROR(CopySnapshot(
      source_lease->snapshot_directory(), destination_directory));
  source_lease.reset();
  DirectoryCleanup destination_cleanup(
      std::filesystem::path(destination_directory).parent_path());

  BackupCatalog::BackupEntry copy = source;
  copy.backup.set_name(name);
  *copy.backup.mutable_expire_time() = request->expire_time();
  GOOGLESQL_ASSIGN_OR_RETURN(*copy.backup.mutable_create_time(),
                             TimestampToProto(ctx->env()->clock()->Now()));
  GOOGLESQL_ASSIGN_OR_RETURN(
      std::shared_ptr<Operation> operation,
      ctx->env()->operation_manager()->CreateOperation(
          name, OperationManager::kAutoGeneratedId));
  copy.operation_name = OperationName(operation);
  operation->SetResponse(copy.backup);
  operations_api::Operation persisted_operation;
  operation->ToProto(&persisted_operation);
  absl::Status status = catalog->CreateBackup(copy, persisted_operation);
  if (!status.ok()) {
    ctx->env()->operation_manager()->DeleteOperation(copy.operation_name);
    return status;
  }

  destination_cleanup.Disarm();
  *response = std::move(persisted_operation);
  return absl::OkStatus();
}
REGISTER_GRPC_HANDLER(DatabaseAdmin, CopyBackup);

absl::Status RestoreDatabase(
    RequestContext* ctx, const database_api::RestoreDatabaseRequest* request,
    operations_api::Operation* response) {
  absl::MutexLock admin_transaction_lock(
      &ctx->env()->admin_transaction_mutex());
  GOOGLESQL_RETURN_IF_ERROR(ValidateInstance(request->parent(), ctx->env()));
  GOOGLESQL_RETURN_IF_ERROR(ValidateDatabaseId(request->database_id()));
  if (!request->has_backup()) {
    return absl::InvalidArgumentError("Restore backup source is required");
  }
  GOOGLESQL_RETURN_IF_ERROR(ValidateBackupName(request->backup()));
  MetadataStore* metadata = ctx->env()->metadata_store();
  if (metadata == nullptr) {
    return absl::FailedPreconditionError(
        "Native restore requires persistent metadata storage");
  }

  BackupCatalog* catalog = ctx->env()->backup_catalog();
  GOOGLESQL_ASSIGN_OR_RETURN(
      std::unique_ptr<BackupCatalog::SnapshotLease> source_lease,
      catalog->AcquireSnapshot(request->backup()));
  BackupCatalog::BackupEntry entry = source_lease->entry();
  if (entry.backup.state() != database_api::Backup::READY) {
    return absl::FailedPreconditionError(
        "Restore source backup must be in READY state");
  }

  absl::string_view target_project_id;
  absl::string_view target_instance_id;
  GOOGLESQL_RETURN_IF_ERROR(ParseInstanceUri(
      request->parent(), &target_project_id, &target_instance_id));
  const size_t backup_marker = entry.backup.name().rfind("/backups/");
  if (backup_marker == std::string::npos) {
    return absl::DataLossError("Backup has an invalid resource name");
  }
  const std::string backup_parent =
      entry.backup.name().substr(0, backup_marker);
  absl::string_view backup_project_id;
  absl::string_view backup_instance_id;
  GOOGLESQL_RETURN_IF_ERROR(ParseInstanceUri(
      backup_parent, &backup_project_id, &backup_instance_id));
  if (target_project_id != backup_project_id) {
    return absl::InvalidArgumentError(
        "Restored database must be in the same project as the backup");
  }

  GOOGLESQL_ASSIGN_OR_RETURN(
      std::shared_ptr<Instance> target_instance,
      ctx->env()->instance_manager()->GetInstance(request->parent()));
  instance_api::Instance target_instance_proto;
  target_instance->ToProto(&target_instance_proto);
  if (entry.source_instance_config.empty()) {
    return absl::DataLossError(
        "Backup is missing its source instance configuration");
  }
  if (target_instance_proto.config() != entry.source_instance_config) {
    return absl::FailedPreconditionError(
        "Restore destination instance configuration does not match the "
        "source backup configuration");
  }

  const std::string database_uri =
      MakeDatabaseUri(request->parent(), request->database_id());
  GOOGLESQL_ASSIGN_OR_RETURN(
      const std::string storage_directory,
      backend::Database::PersistentStorageDirectory(config::data_dir(),
                                                    database_uri));
  const std::filesystem::path database_root =
      std::filesystem::path(storage_directory).parent_path();
  std::optional<DirectoryCleanup> database_cleanup;
  GOOGLESQL_ASSIGN_OR_RETURN(
      std::unique_ptr<DatabaseManager::Creation> creation,
      ctx->env()->database_manager()->ReserveDatabase(database_uri));
  const std::filesystem::path staging_root =
      database_root.string() + ".restoring";
  const std::filesystem::path restore_marker =
      database_root / ".restore-in-progress";
  std::error_code filesystem_error;
  if (std::filesystem::exists(database_root, filesystem_error)) {
    if (filesystem_error) {
      return absl::InternalError(absl::StrCat(
          "Failed to inspect restore destination: ",
          filesystem_error.message()));
    }
    const bool owned_incomplete_restore =
        std::filesystem::exists(restore_marker, filesystem_error);
    if (filesystem_error) {
      return absl::InternalError(absl::StrCat(
          "Failed to inspect restore ownership marker: ",
          filesystem_error.message()));
    }
    if (!owned_incomplete_restore) {
      return absl::AlreadyExistsError(
          absl::StrCat("Restore destination storage already exists: ",
                       database_uri));
    }
    std::filesystem::remove_all(database_root, filesystem_error);
    if (filesystem_error) {
      return absl::InternalError(absl::StrCat(
          "Failed to recover incomplete restore destination: ",
          filesystem_error.message()));
    }
  } else if (filesystem_error) {
    return absl::InternalError(absl::StrCat(
        "Failed to inspect restore destination: ",
        filesystem_error.message()));
  }

  std::filesystem::remove_all(staging_root, filesystem_error);
  if (filesystem_error) {
    return absl::InternalError(absl::StrCat(
        "Failed to clean incomplete restore staging directory: ",
        filesystem_error.message()));
  }
  std::filesystem::create_directories(staging_root, filesystem_error);
  if (filesystem_error) {
    return absl::InternalError(absl::StrCat(
        "Failed to create restore staging directory: ",
        filesystem_error.message()));
  }
  DirectoryCleanup staging_cleanup(staging_root);
  {
    std::ofstream marker(staging_root / ".restore-in-progress");
    marker << database_uri;
    if (!marker) {
      return absl::InternalError(
          "Failed to write restore ownership marker");
    }
  }
  GOOGLESQL_RETURN_IF_ERROR(CopySnapshot(
      source_lease->snapshot_directory(),
      (staging_root / "storage").string()));
  source_lease.reset();

  std::filesystem::create_directories(database_root.parent_path(),
                                      filesystem_error);
  if (!filesystem_error) {
    std::filesystem::rename(staging_root, database_root, filesystem_error);
  }
  if (filesystem_error) {
    return absl::InternalError(absl::StrCat(
        "Failed to publish restore snapshot: ", filesystem_error.message()));
  }
  staging_cleanup.Disarm();
  database_cleanup.emplace(database_root);

  backend::Database::IdCounterValues counters{
      .table_id = entry.id_counters.table_id,
      .column_id = entry.id_counters.column_id,
      .change_stream_id = entry.id_counters.change_stream_id,
  };
  std::vector<backend::SchemaChangeOperation> schema_change_operations;
  if (entry.schema_change_batches.empty()) {
    schema_change_operations.push_back(
        {.statements = entry.ddl_statements,
         .proto_descriptor_bytes = entry.proto_descriptor_bytes,
         .database_dialect = entry.dialect});
  } else {
    for (const auto& batch : entry.schema_change_batches) {
      schema_change_operations.push_back(
          {.statements = batch.statements,
           .proto_descriptor_bytes = batch.proto_descriptor_bytes,
           .database_dialect = entry.dialect});
    }
  }
  GOOGLESQL_ASSIGN_OR_RETURN(
      std::shared_ptr<Database> restored,
      creation->Build(schema_change_operations, counters,
                      ctx->env()->clock()->Now()));
  database_api::Database restored_proto;
  GOOGLESQL_RETURN_IF_ERROR(restored->ToProto(&restored_proto));

  GOOGLESQL_ASSIGN_OR_RETURN(
      std::shared_ptr<Operation> operation,
      ctx->env()->operation_manager()->CreateOperation(
          database_uri, OperationManager::kAutoGeneratedId));
  operation->SetResponse(restored_proto);
  operations_api::Operation persisted_operation;
  operation->ToProto(&persisted_operation);
  bool operation_persisted = false;
  bool metadata_persisted = false;
  auto rollback = [&](const absl::Status& failure) {
    absl::Status operation_rollback = absl::OkStatus();
    if (operation_persisted) {
      operation_rollback =
          catalog->DeleteOperation(persisted_operation.name());
    }
    metadata->RemovePendingOperation(persisted_operation.name());
    metadata->RemoveDatabase(request->parent(), request->database_id());
    absl::Status metadata_rollback = absl::OkStatus();
    if (metadata_persisted) {
      metadata_rollback = metadata->Save();
    }
    if (!metadata_rollback.ok()) {
      // The durable metadata still names the restored database and pending
      // terminal operation. Preserve both the root and restore marker so a
      // restart can reconcile them, and restore the same record in memory so
      // a later metadata save cannot erase the only durable recovery path.
      absl::Status metadata_restore = PersistRestoredDatabase(
          ctx->env(), request->parent(), request->database_id(), entry,
          restored_proto);
      metadata->SetPendingOperation(persisted_operation);
      database_cleanup->Disarm();
      return absl::DataLossError(absl::StrCat(
          failure.message(), "; failed to roll back restored state; metadata: ",
          metadata_rollback.message(),
          metadata_restore.ok()
              ? ""
              : absl::StrCat("; failed to restore in-memory metadata: ",
                             metadata_restore.message()),
          "; restart required to reconcile durable restore"));
    }
    if (!operation_rollback.ok()) {
      // The catalog still contains the terminal operation. Preserve the
      // matching database metadata and root so startup can replay that
      // operation instead of leaving it attached to a deleted snapshot.
      absl::Status metadata_restore = PersistRestoredDatabase(
          ctx->env(), request->parent(), request->database_id(), entry,
          restored_proto);
      metadata->SetPendingOperation(persisted_operation);
      absl::Status recovery_save = metadata->Save();
      database_cleanup->Disarm();
      return absl::DataLossError(absl::StrCat(
          failure.message(), "; failed to roll back restored operation: ",
          operation_rollback.message(),
          metadata_restore.ok()
              ? ""
              : absl::StrCat("; failed to restore in-memory metadata: ",
                             metadata_restore.message()),
          recovery_save.ok()
              ? "; restart required to reconcile durable restore"
              : absl::StrCat("; failed to persist restore recovery state: ",
                             recovery_save.message())));
    }
    ctx->env()->operation_manager()->DeleteOperation(
        persisted_operation.name());
    restored.reset();
    return failure;
  };

  GOOGLESQL_RETURN_IF_ERROR(PersistRestoredDatabase(
      ctx->env(), request->parent(), request->database_id(), entry,
      restored_proto));
  metadata->SetPendingOperation(persisted_operation);
  absl::Status metadata_status = metadata->Save();
  if (!metadata_status.ok()) {
    return rollback(metadata_status);
  }
  metadata_persisted = true;
  absl::Status metadata_marker_status =
      DatabaseManager::MarkDatabaseMetadataCommitted(config::data_dir(),
                                                     database_uri);
  if (!metadata_marker_status.ok()) {
    return rollback(metadata_marker_status);
  }
  absl::Status operation_status =
      catalog->SaveOperation(persisted_operation);
  if (!operation_status.ok()) {
    return rollback(operation_status);
  }
  operation_persisted = true;

  absl::Status publish_status = creation->Publish();
  if (!publish_status.ok()) {
    return rollback(publish_status);
  }
  database_cleanup->Disarm();
  metadata->RemovePendingOperation(persisted_operation.name());
  absl::Status journal_status = metadata->Save();
  if (!journal_status.ok()) {
    // The durable pending record is intentionally safe to replay: the catalog
    // already contains the exact terminal operation.
    ABSL_LOG(WARNING) << "Failed to clear promoted restore operation journal "
                      << persisted_operation.name() << ": " << journal_status;
  }
  std::filesystem::remove(restore_marker, filesystem_error);
  if (filesystem_error) {
    ABSL_LOG(WARNING) << "Failed to remove completed restore marker "
                      << restore_marker << ": " << filesystem_error.message();
  }
  *response = std::move(persisted_operation);
  return absl::OkStatus();
}
REGISTER_GRPC_HANDLER(DatabaseAdmin, RestoreDatabase);

absl::Status CreateBackupSchedule(
    RequestContext* ctx,
    const database_api::CreateBackupScheduleRequest* request,
    database_api::BackupSchedule* response) {
  absl::MutexLock admin_transaction_lock(
      &ctx->env()->admin_transaction_mutex());
  GOOGLESQL_RETURN_IF_ERROR(ValidateDatabase(request->parent(), ctx->env()));
  GOOGLESQL_RETURN_IF_ERROR(
      ValidateResourceId(request->backup_schedule_id(), "Backup schedule ID"));
  if (!request->has_backup_schedule()) {
    return absl::InvalidArgumentError("Backup schedule must be provided");
  }
  database_api::BackupSchedule schedule = request->backup_schedule();
  schedule.set_name(
      MakeBackupScheduleName(request->parent(), request->backup_schedule_id()));
  GOOGLESQL_RETURN_IF_ERROR(ValidateBackupScheduleName(schedule.name()));
  GOOGLESQL_ASSIGN_OR_RETURN(*schedule.mutable_update_time(),
                             TimestampToProto(ctx->env()->clock()->Now()));
  GOOGLESQL_RETURN_IF_ERROR(
      ctx->env()->backup_catalog()->CreateBackupSchedule(schedule));
  *response = schedule;
  return absl::OkStatus();
}
REGISTER_GRPC_HANDLER(DatabaseAdmin, CreateBackupSchedule);

absl::Status GetBackupSchedule(
    RequestContext* ctx, const database_api::GetBackupScheduleRequest* request,
    database_api::BackupSchedule* response) {
  GOOGLESQL_RETURN_IF_ERROR(ValidateBackupScheduleName(request->name()));
  GOOGLESQL_ASSIGN_OR_RETURN(
      *response,
      ctx->env()->backup_catalog()->GetBackupSchedule(request->name()));
  return absl::OkStatus();
}
REGISTER_GRPC_HANDLER(DatabaseAdmin, GetBackupSchedule);

absl::Status ListBackupSchedules(
    RequestContext* ctx,
    const database_api::ListBackupSchedulesRequest* request,
    database_api::ListBackupSchedulesResponse* response) {
  GOOGLESQL_RETURN_IF_ERROR(ValidateDatabase(request->parent(), ctx->env()));
  int32_t page_size = request->page_size();
  if (page_size <= 0 || page_size > kMaximumPageSize) {
    page_size = kMaximumPageSize;
  }
  for (const auto& schedule :
       ctx->env()->backup_catalog()->ListBackupSchedules(request->parent())) {
    if (!request->page_token().empty() &&
        schedule.name() < request->page_token()) {
      continue;
    }
    if (response->backup_schedules_size() >= page_size) {
      response->set_next_page_token(schedule.name());
      break;
    }
    *response->add_backup_schedules() = schedule;
  }
  return absl::OkStatus();
}
REGISTER_GRPC_HANDLER(DatabaseAdmin, ListBackupSchedules);

absl::Status UpdateBackupSchedule(
    RequestContext* ctx,
    const database_api::UpdateBackupScheduleRequest* request,
    database_api::BackupSchedule* response) {
  absl::MutexLock admin_transaction_lock(
      &ctx->env()->admin_transaction_mutex());
  if (!request->has_backup_schedule()) {
    return absl::InvalidArgumentError("Backup schedule must be provided");
  }
  GOOGLESQL_RETURN_IF_ERROR(
      ValidateBackupScheduleName(request->backup_schedule().name()));
  if (request->update_mask().paths().empty()) {
    return absl::InvalidArgumentError(
        "Backup schedule update_mask must be provided");
  }
  GOOGLESQL_ASSIGN_OR_RETURN(database_api::BackupSchedule current,
                             ctx->env()->backup_catalog()->GetBackupSchedule(
                                 request->backup_schedule().name()));
  const database_api::BackupSchedule& requested = request->backup_schedule();
  for (const std::string& path : request->update_mask().paths()) {
    if (path == "spec" && requested.has_spec()) {
      *current.mutable_spec() = requested.spec();
    } else if (path == "retention_duration" &&
               requested.has_retention_duration()) {
      *current.mutable_retention_duration() = requested.retention_duration();
    } else if (path == "encryption_config" &&
               requested.has_encryption_config()) {
      *current.mutable_encryption_config() = requested.encryption_config();
    } else if (path == "full_backup_spec" && requested.has_full_backup_spec()) {
      *current.mutable_full_backup_spec() = requested.full_backup_spec();
    } else if (path == "incremental_backup_spec" &&
               requested.has_incremental_backup_spec()) {
      *current.mutable_incremental_backup_spec() =
          requested.incremental_backup_spec();
    } else {
      return absl::InvalidArgumentError(
          absl::StrCat("Unsupported backup schedule update field: ", path));
    }
  }
  GOOGLESQL_ASSIGN_OR_RETURN(*current.mutable_update_time(),
                             TimestampToProto(ctx->env()->clock()->Now()));
  GOOGLESQL_RETURN_IF_ERROR(
      ctx->env()->backup_catalog()->UpdateBackupSchedule(current));
  *response = current;
  return absl::OkStatus();
}
REGISTER_GRPC_HANDLER(DatabaseAdmin, UpdateBackupSchedule);

absl::Status DeleteBackupSchedule(
    RequestContext* ctx,
    const database_api::DeleteBackupScheduleRequest* request,
    protobuf_api::Empty* response) {
  GOOGLESQL_RETURN_IF_ERROR(ValidateBackupScheduleName(request->name()));
  absl::MutexLock admin_transaction_lock(
      &ctx->env()->admin_transaction_mutex());
  GOOGLESQL_RETURN_IF_ERROR(
      ctx->env()->backup_catalog()->GetBackupSchedule(request->name()).status());

  MetadataStore* metadata = ctx->env()->metadata_store();
  const std::optional<iam_api::Policy> previous_persisted_policy =
      metadata == nullptr ? std::nullopt
                          : metadata->GetIamPolicy(request->name());
  if (metadata != nullptr) {
    metadata->RemoveIamPolicy(request->name());
    metadata->SetPendingBackupDeletion(request->name());
    absl::Status metadata_status = metadata->Save();
    if (!metadata_status.ok()) {
      metadata->RemovePendingBackupDeletion(request->name());
      if (previous_persisted_policy.has_value()) {
        metadata->SetIamPolicy(request->name(), *previous_persisted_policy);
      }
      return metadata_status;
    }
  }

  absl::Status delete_status =
      ctx->env()->backup_catalog()->DeleteBackupSchedule(request->name());
  if (!delete_status.ok()) {
    if (metadata != nullptr) {
      metadata->RemovePendingBackupDeletion(request->name());
      if (previous_persisted_policy.has_value()) {
        metadata->SetIamPolicy(request->name(), *previous_persisted_policy);
      }
      absl::Status rollback_status = metadata->Save();
      if (!rollback_status.ok()) {
        return absl::DataLossError(absl::StrCat(
            delete_status.message(),
            "; failed to roll back backup schedule deletion intent: ",
            rollback_status.message()));
      }
    }
    return delete_status;
  }

  ctx->env()->RemoveIamPolicy(request->name());
  if (metadata != nullptr) {
    metadata->RemovePendingBackupDeletion(request->name());
    GOOGLESQL_RETURN_IF_ERROR(metadata->Save());
  }
  return absl::OkStatus();
}
REGISTER_GRPC_HANDLER(DatabaseAdmin, DeleteBackupSchedule);

}  // namespace frontend
}  // namespace emulator
}  // namespace spanner
}  // namespace google

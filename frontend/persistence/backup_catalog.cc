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

#include "frontend/persistence/backup_catalog.h"

#include <cstdio>
#include <filesystem>
#include <memory>
#include <iomanip>
#include <optional>
#include <set>
#include <sstream>
#include <string>
#include <system_error>
#include <utility>
#include <vector>

#include "absl/status/status.h"
#include "absl/strings/escaping.h"
#include "absl/strings/match.h"
#include "absl/strings/str_cat.h"
#include "frontend/persistence/atomic_file.h"
#include "nlohmann/json.hpp"
#include "googlesql/base/status_macros.h"
#include "leveldb/db.h"

namespace google {
namespace spanner {
namespace emulator {
namespace frontend {
namespace {

using json = nlohmann::json;
constexpr char kSnapshotOwnerMarker[] = ".emulator-backup-owner";

std::string SerializeProto(const google::protobuf::MessageLite& message) {
  return absl::Base64Escape(message.SerializeAsString());
}

template <typename Message>
absl::StatusOr<Message> ParseProto(const json& value,
                                   const std::string& description) {
  if (!value.is_string()) {
    return absl::DataLossError(absl::StrCat(description, " is not a string"));
  }
  std::string serialized;
  if (!absl::Base64Unescape(value.get<std::string>(), &serialized)) {
    return absl::DataLossError(
        absl::StrCat(description, " is not valid base64"));
  }
  Message message;
  if (!message.ParseFromString(serialized)) {
    return absl::DataLossError(
        absl::StrCat(description, " is not a valid protobuf"));
  }
  return message;
}

// Orders the backups of an incremental chain by version_time, as the Backup
// API documents.
bool OlderInChain(const database_api::Backup& left,
                  const database_api::Backup& right) {
  if (left.version_time().seconds() != right.version_time().seconds()) {
    return left.version_time().seconds() < right.version_time().seconds();
  }
  if (left.version_time().nanos() != right.version_time().nanos()) {
    return left.version_time().nanos() < right.version_time().nanos();
  }
  return left.name() < right.name();
}

}  // namespace

BackupCatalog::BackupCatalog(std::string data_dir)
    : data_dir_(std::move(data_dir)),
      catalog_path_(data_dir_.empty()
                        ? std::string()
                        : absl::StrCat(data_dir_, "/backup_catalog.json")) {}

absl::Status BackupCatalog::Load() {
  absl::MutexLock lock(mu_);
  backups_.clear();
  schedules_.clear();
  schedule_cursors_.clear();
  schedule_chains_.clear();
  operations_.clear();
  std::map<std::string, BackupEntry> loaded_backups;
  std::map<std::string, database_api::BackupSchedule> loaded_schedules;
  std::map<std::string, int64_t> loaded_cursors;
  std::map<std::string, BackupChain> loaded_chains;
  std::map<std::string, google::longrunning::Operation> loaded_operations;
  if (!persistent()) return absl::OkStatus();
  auto regular_file_exists =
      [](const std::string& path,
         absl::string_view description) -> absl::StatusOr<bool> {
    std::error_code error;
    const std::filesystem::file_status status =
        std::filesystem::symlink_status(path, error);
    if (error == std::errc::no_such_file_or_directory) return false;
    if (error) {
      return absl::DataLossError(absl::StrCat(
          "Failed to inspect ", description, " ", path, ": ",
          error.message()));
    }
    if (!std::filesystem::exists(status)) return false;
    if (std::filesystem::is_symlink(status) ||
        !std::filesystem::is_regular_file(status)) {
      return absl::DataLossError(
          absl::StrCat(description, " is not a regular file: ", path));
    }
    return true;
  };
  auto catalog_exists_or =
      regular_file_exists(catalog_path_, "backup catalog");
  if (!catalog_exists_or.ok()) return catalog_exists_or.status();
  bool catalog_exists = *catalog_exists_or;
  const std::string temporary_path = catalog_path_ + ".tmp";
  auto temporary_exists_or =
      regular_file_exists(temporary_path, "temporary backup catalog");
  if (!temporary_exists_or.ok()) return temporary_exists_or.status();
  bool temporary_exists = *temporary_exists_or;
  std::error_code catalog_error;
  if (!catalog_exists && temporary_exists) {
    std::filesystem::rename(temporary_path, catalog_path_, catalog_error);
    if (catalog_error) {
      return absl::DataLossError(absl::StrCat(
          "Failed to recover temporary backup catalog ", temporary_path, ": ",
          catalog_error.message()));
    }
    catalog_exists = true;
    temporary_exists = false;
  }
  if (!catalog_exists) {
    absl::Status cleanup_status =
        CleanupStaleSnapshotsLocked(loaded_backups);
    if (!cleanup_status.ok()) return cleanup_status;
    return absl::OkStatus();
  }

  auto contents = ReadRegularFileNoFollow(catalog_path_);
  if (!contents.ok()) return contents.status();

  json root;
  try {
    root = json::parse(*contents);
  } catch (const json::exception& error) {
    return absl::DataLossError(absl::StrCat("Failed to parse backup catalog ",
                                            catalog_path_, ": ", error.what()));
  }

  if (!root.is_object()) {
    return absl::DataLossError("Backup catalog root is not an object");
  }
  if (!root.contains("version") ||
      !root["version"].is_number_integer()) {
    return absl::DataLossError(
        "Backup catalog version must be present and integer");
  }
  const int version = root["version"].get<int>();
  if (version < 1 || version > 6) {
    return absl::DataLossError(
        absl::StrCat("Unsupported backup catalog version ", version));
  }
  if (!root.contains("backups") || !root["backups"].is_object()) {
    return absl::DataLossError(
        "Backup catalog backups field must be present and an object");
  }
  if (version >= 2 &&
      (!root.contains("schedules") || !root["schedules"].is_object())) {
    return absl::DataLossError(
        "Backup catalog schedules field must be present and an object");
  }
  if (version >= 3 &&
      (!root.contains("operations") || !root["operations"].is_object())) {
    return absl::DataLossError(
        "Backup catalog operations field must be present and an object");
  }
  if (version >= 5 &&
      (!root.contains("scheduleCursors") ||
       !root["scheduleCursors"].is_object())) {
    return absl::DataLossError(
        "Backup catalog schedule cursors field must be an object");
  }
  if (version >= 6 &&
      (!root.contains("scheduleChains") ||
       !root["scheduleChains"].is_object())) {
    return absl::DataLossError(
        "Backup catalog schedule chains field must be an object");
  }

  try {
    if (root.contains("backups")) {
      if (!root["backups"].is_object()) {
        return absl::DataLossError(
            "Backup catalog backups field is not an object");
      }
      for (const auto& [name, value] : root["backups"].items()) {
        if (!value.is_object() || !value.contains("proto") ||
            !value["proto"].is_string() || !value.contains("dialect") ||
            !value["dialect"].is_number_integer() ||
            !value.contains("idCounters") ||
            !value["idCounters"].is_object() ||
            !value.contains("operationName") ||
            !value["operationName"].is_string() ||
            !value.contains("sourceInstanceConfig") ||
            !value["sourceInstanceConfig"].is_string()) {
          return absl::DataLossError(
              absl::StrCat("Incomplete backup catalog entry for ", name));
        }
        const auto& persisted_counters = value["idCounters"];
        if (!persisted_counters.contains("tableId") ||
            !persisted_counters["tableId"].is_number_integer() ||
            !persisted_counters.contains("columnId") ||
            !persisted_counters["columnId"].is_number_integer() ||
            !persisted_counters.contains("changeStreamId") ||
            !persisted_counters["changeStreamId"].is_number_integer()) {
          return absl::DataLossError(absl::StrCat(
              "Incomplete backup ID counters for ", name));
        }
        if (version >= 4) {
          if (!value.contains("ddlBatches") ||
              !value["ddlBatches"].is_array()) {
            return absl::DataLossError(absl::StrCat(
                "Missing backup schema-change batches for ", name));
          }
        } else if (!value.contains("ddlStatements") ||
                   !value["ddlStatements"].is_array() ||
                   !value.contains("protoDescriptors") ||
                   !value["protoDescriptors"].is_string()) {
          return absl::DataLossError(absl::StrCat(
              "Incomplete legacy backup schema for ", name));
        }
        BackupEntry entry;
        auto backup = ParseProto<database_api::Backup>(
            value.at("proto"), absl::StrCat("Backup ", name));
        if (!backup.ok()) {
          return backup.status();
        }
        if (backup->name() != name) {
          return absl::DataLossError(
              absl::StrCat("Backup catalog key/name mismatch for ", name));
        }
        entry.backup = std::move(*backup);
        if (version < 6) {
          // Earlier catalogs predate encryption and size reporting, and all
          // of their backups are outside incremental chains.
          entry.backup.mutable_encryption_info()->set_encryption_type(
              database_api::EncryptionInfo::GOOGLE_DEFAULT_ENCRYPTION);
          *entry.backup.mutable_oldest_version_time() =
              entry.backup.version_time();
          entry.backup.set_freeable_size_bytes(entry.backup.size_bytes());
          entry.backup.set_exclusive_size_bytes(entry.backup.size_bytes());
        }
        if (value.contains("ddlBatches")) {
          if (!value["ddlBatches"].is_array()) {
            return absl::DataLossError(absl::StrCat(
                "Malformed backup schema-change batches for ", name));
          }
          for (const auto& batch_json : value["ddlBatches"]) {
            if (!batch_json.is_object() ||
                !batch_json.contains("statements") ||
                !batch_json["statements"].is_array() ||
                !batch_json.contains("protoDescriptors") ||
                !batch_json["protoDescriptors"].is_string()) {
              return absl::DataLossError(absl::StrCat(
                  "Malformed backup schema-change batch for ", name));
            }
            PersistedSchemaChangeBatch batch;
            for (const auto& statement : batch_json["statements"]) {
              if (!statement.is_string()) {
                return absl::DataLossError(absl::StrCat(
                    "Malformed backup schema-change statement for ", name));
              }
              batch.statements.push_back(statement.get<std::string>());
            }
            if (!absl::Base64Unescape(
                    batch_json["protoDescriptors"].get<std::string>(),
                    &batch.proto_descriptor_bytes)) {
              return absl::DataLossError(absl::StrCat(
                  "Malformed backup schema-change descriptors for ", name));
            }
            if (batch_json.contains("schemaChangeTimestamp")) {
              if (!batch_json["schemaChangeTimestamp"].is_string()) {
                return absl::DataLossError(absl::StrCat(
                    "Malformed backup schema-change timestamp for ", name));
              }
              batch.schema_change_timestamp =
                  batch_json["schemaChangeTimestamp"].get<std::string>();
            }
            entry.ddl_statements.insert(entry.ddl_statements.end(),
                                        batch.statements.begin(),
                                        batch.statements.end());
            entry.proto_descriptor_bytes = batch.proto_descriptor_bytes;
            entry.schema_change_batches.push_back(std::move(batch));
          }
        } else {
          if (version >= 4) {
            return absl::DataLossError(absl::StrCat(
                "Missing backup schema-change batches for ", name));
          }
          entry.ddl_statements =
              value.value("ddlStatements", std::vector<std::string>{});
          if (value.contains("protoDescriptors")) {
            if (!value["protoDescriptors"].is_string() ||
                !absl::Base64Unescape(
                    value["protoDescriptors"].get<std::string>(),
                    &entry.proto_descriptor_bytes)) {
              return absl::DataLossError(absl::StrCat(
                  "Malformed backup proto descriptors for ", name));
            }
          }
          entry.schema_change_batches.push_back(
              {.statements = entry.ddl_statements,
               .proto_descriptor_bytes = entry.proto_descriptor_bytes});
        }
        entry.dialect = static_cast<database_api::DatabaseDialect>(value.value(
            "dialect",
            static_cast<int>(
                database_api::DatabaseDialect::GOOGLE_STANDARD_SQL)));
        if (entry.dialect !=
                database_api::DatabaseDialect::GOOGLE_STANDARD_SQL &&
            entry.dialect != database_api::DatabaseDialect::POSTGRESQL) {
          return absl::DataLossError(
              absl::StrCat("Invalid backup dialect for ", name));
        }
        const json counters = value.value("idCounters", json::object());
        entry.id_counters.table_id = counters.value("tableId", int64_t{0});
        entry.id_counters.column_id = counters.value("columnId", int64_t{0});
        entry.id_counters.change_stream_id =
            counters.value("changeStreamId", int64_t{0});
        if (entry.id_counters.table_id < 0 ||
            entry.id_counters.column_id < 0 ||
            entry.id_counters.change_stream_id < 0) {
          return absl::DataLossError(
              absl::StrCat("Negative backup ID counter for ", name));
        }
        entry.operation_name = value.value("operationName", "");
        entry.source_instance_config =
            value.value("sourceInstanceConfig", "");
        loaded_backups.emplace(name, std::move(entry));
      }
    }

    if (root.contains("schedules")) {
      if (!root["schedules"].is_object()) {
        return absl::DataLossError(
            "Backup catalog schedules field is not an object");
      }
      for (const auto& [name, value] : root["schedules"].items()) {
        auto schedule = ParseProto<database_api::BackupSchedule>(
            value, absl::StrCat("Backup schedule ", name));
        if (!schedule.ok()) {
          return schedule.status();
        }
        if (schedule->name() != name) {
          return absl::DataLossError(absl::StrCat(
              "Backup schedule catalog key/name mismatch for ", name));
        }
        loaded_schedules.emplace(name, std::move(*schedule));
      }
    }
    if (root.contains("scheduleCursors")) {
      if (!root["scheduleCursors"].is_object()) {
        return absl::DataLossError(
            "Backup catalog schedule cursors field is not an object");
      }
      for (const auto& [name, value] : root["scheduleCursors"].items()) {
        if (!loaded_schedules.contains(name) || !value.is_number_integer() ||
            value.get<int64_t>() < 0) {
          return absl::DataLossError(
              absl::StrCat("Invalid backup schedule cursor for ", name));
        }
        loaded_cursors.emplace(name, value.get<int64_t>());
      }
    }
    for (const auto& [name, schedule] : loaded_schedules) {
      if (!loaded_cursors.contains(name)) {
        if (version >= 5) {
          return absl::DataLossError(
              absl::StrCat("Missing backup schedule cursor for ", name));
        }
        loaded_cursors.emplace(name, 0);
      }
    }
    if (root.contains("scheduleChains")) {
      for (const auto& [name, value] : root["scheduleChains"].items()) {
        if (!loaded_schedules.contains(name) || !value.is_object() ||
            !value.contains("id") || !value["id"].is_string() ||
            value["id"].get<std::string>().empty() ||
            !value.contains("backupCount") ||
            !value["backupCount"].is_number_integer() ||
            value["backupCount"].get<int64_t>() < 1 ||
            !value.contains("oldestVersionTime") ||
            !value.contains("newestBackup") ||
            !value["newestBackup"].is_string()) {
          return absl::DataLossError(
              absl::StrCat("Invalid backup schedule chain for ", name));
        }
        auto oldest_version_time = ParseProto<google::protobuf::Timestamp>(
            value["oldestVersionTime"],
            absl::StrCat("Backup schedule chain ", name, " oldest time"));
        if (!oldest_version_time.ok()) return oldest_version_time.status();
        loaded_chains.emplace(
            name, BackupChain{
                      .id = value["id"].get<std::string>(),
                      .backup_count = value["backupCount"].get<int64_t>(),
                      .oldest_version_time = std::move(*oldest_version_time),
                      .newest_backup = value["newestBackup"].get<std::string>(),
                  });
      }
    }

    if (root.contains("operations")) {
      if (!root["operations"].is_object()) {
        return absl::DataLossError(
            "Backup catalog operations field is not an object");
      }
      for (const auto& [name, value] : root["operations"].items()) {
        auto operation = ParseProto<google::longrunning::Operation>(
            value, absl::StrCat("Operation ", name));
        if (!operation.ok()) {
          return operation.status();
        }
        if (operation->name() != name || !operation->done()) {
          return absl::DataLossError(
              absl::StrCat("Invalid terminal operation record for ", name));
        }
        loaded_operations.emplace(name, std::move(*operation));
      }
    }

    // Older catalogs kept only the backup-to-operation link. Reconstruct its
    // terminal response in memory so those local catalogs remain readable.
    for (const auto& [name, entry] : loaded_backups) {
      if (entry.operation_name.empty() ||
          loaded_operations.contains(entry.operation_name)) {
        continue;
      }
      google::longrunning::Operation operation;
      operation.set_name(entry.operation_name);
      operation.set_done(true);
      operation.mutable_response()->PackFrom(entry.backup);
      loaded_operations.emplace(operation.name(), std::move(operation));
    }
  } catch (const json::exception& error) {
    return absl::DataLossError(absl::StrCat("Invalid backup catalog ",
                                            catalog_path_, ": ", error.what()));
  }

  for (const auto& [name, entry] : loaded_backups) {
    if (entry.backup.state() != database_api::Backup::READY) {
      continue;
    }
    absl::Status snapshot_status = ValidateSnapshotLocked(name);
    if (!snapshot_status.ok()) {
      return snapshot_status;
    }
  }
  absl::Status cleanup_status =
      CleanupStaleSnapshotsLocked(loaded_backups);
  if (!cleanup_status.ok()) return cleanup_status;
  if (temporary_exists) {
    std::filesystem::remove_all(temporary_path, catalog_error);
    if (catalog_error) {
      return absl::DataLossError(absl::StrCat(
          "Failed to clean stale temporary backup catalog ", temporary_path,
          ": ", catalog_error.message()));
    }
  }
  backups_ = std::move(loaded_backups);
  schedules_ = std::move(loaded_schedules);
  schedule_cursors_ = std::move(loaded_cursors);
  schedule_chains_ = std::move(loaded_chains);
  operations_ = std::move(loaded_operations);
  return absl::OkStatus();
}

absl::Status BackupCatalog::CreateBackup(
    BackupEntry entry, const google::longrunning::Operation& operation,
    std::optional<ScheduleRun> schedule_run) {
  const std::string name = entry.backup.name();
  if (operation.name().empty() || !operation.done() ||
      operation.name() != entry.operation_name) {
    return absl::InvalidArgumentError(
        "Backup operation must be terminal and match operation_name");
  }

  absl::MutexLock lock(mu_);
  if (schedule_run.has_value()) {
    const auto cursor = schedule_cursors_.find(schedule_run->name);
    if (cursor == schedule_cursors_.end() ||
        cursor->second != schedule_run->due_seconds ||
        schedules_.at(schedule_run->name).SerializeAsString() !=
            schedule_run->schedule_serialized ||
        schedule_run->next_due_seconds <= schedule_run->due_seconds ||
        entry.backup.backup_schedules_size() != 1 ||
        entry.backup.backup_schedules(0) != schedule_run->name) {
      return absl::AbortedError("Backup schedule changed during capture");
    }
  }
  if (backups_.contains(name)) {
    return absl::AlreadyExistsError(
        absl::StrCat("Backup already exists: ", name));
  }
  if (operations_.contains(operation.name())) {
    return absl::AlreadyExistsError(
        absl::StrCat("Operation already exists: ", operation.name()));
  }
  const std::string chain_id = entry.backup.incremental_backup_chain_id();
  const bool extends_chain = schedule_run.has_value() && !chain_id.empty();
  std::optional<BackupChain> previous_chain;
  backups_.emplace(name, std::move(entry));
  operations_.emplace(operation.name(), operation);
  if (schedule_run.has_value()) {
    schedule_cursors_.at(schedule_run->name) =
        schedule_run->next_due_seconds;
  }
  if (extends_chain) {
    auto current = schedule_chains_.find(schedule_run->name);
    if (current != schedule_chains_.end()) previous_chain = current->second;
    BackupChain& chain = schedule_chains_[schedule_run->name];
    chain.backup_count = chain.id == chain_id ? chain.backup_count + 1 : 1;
    chain.id = chain_id;
    chain.oldest_version_time = backups_.at(name).backup.oldest_version_time();
    chain.newest_backup = name;
    RefreshChainLocked(chain_id);
  }
  absl::Status status = SaveLocked();
  if (!status.ok()) {
    backups_.erase(name);
    operations_.erase(operation.name());
    if (schedule_run.has_value()) {
      schedule_cursors_.at(schedule_run->name) = schedule_run->due_seconds;
    }
    if (extends_chain) {
      if (previous_chain.has_value()) {
        schedule_chains_[schedule_run->name] = *std::move(previous_chain);
      } else {
        schedule_chains_.erase(schedule_run->name);
      }
      RefreshChainLocked(chain_id);
    }
  }
  return status;
}

absl::StatusOr<BackupCatalog::BackupEntry> BackupCatalog::GetBackup(
    const std::string& name) const {
  absl::ReaderMutexLock lock(mu_);
  auto it = backups_.find(name);
  if (it == backups_.end()) {
    return absl::NotFoundError(absl::StrCat("Backup not found: ", name));
  }
  return it->second;
}

std::vector<BackupCatalog::BackupEntry> BackupCatalog::ListBackups(
    const std::string& parent) const {
  absl::ReaderMutexLock lock(mu_);
  std::vector<BackupEntry> result;
  for (const auto& [name, entry] : backups_) {
    if (ResourceParent(name) == parent) result.push_back(entry);
  }
  return result;
}

std::vector<BackupCatalog::BackupEntry> BackupCatalog::AllBackups() const {
  absl::ReaderMutexLock lock(mu_);
  std::vector<BackupEntry> result;
  result.reserve(backups_.size());
  for (const auto& [name, entry] : backups_) result.push_back(entry);
  return result;
}

std::vector<google::longrunning::Operation> BackupCatalog::AllOperations()
    const {
  absl::ReaderMutexLock lock(mu_);
  std::vector<google::longrunning::Operation> result;
  result.reserve(operations_.size());
  for (const auto& [name, operation] : operations_) {
    result.push_back(operation);
  }
  return result;
}

absl::Status BackupCatalog::SaveOperation(
    const google::longrunning::Operation& operation) {
  if (operation.name().empty() || !operation.done()) {
    return absl::InvalidArgumentError(
        "Persisted operation must have a name and be terminal");
  }
  absl::MutexLock lock(mu_);
  auto existing = operations_.find(operation.name());
  if (existing != operations_.end()) {
    if (existing->second.SerializeAsString() == operation.SerializeAsString()) {
      return absl::OkStatus();
    }
    return absl::AlreadyExistsError(
        absl::StrCat("A different operation already exists: ",
                     operation.name()));
  }
  operations_.emplace(operation.name(), operation);
  absl::Status status = SaveLocked();
  if (!status.ok()) {
    operations_.erase(operation.name());
  }
  return status;
}

absl::Status BackupCatalog::UpdateBackup(const database_api::Backup& backup) {
  absl::MutexLock lock(mu_);
  auto it = backups_.find(backup.name());
  if (it == backups_.end()) {
    return absl::NotFoundError(
        absl::StrCat("Backup not found: ", backup.name()));
  }
  database_api::Backup previous = it->second.backup;
  it->second.backup = backup;
  absl::Status status = SaveLocked();
  if (!status.ok()) it->second.backup = std::move(previous);
  return status;
}

absl::Status BackupCatalog::DeleteBackup(const std::string& name) {
  absl::MutexLock lock(mu_);
  auto it = backups_.find(name);
  if (it == backups_.end()) {
    return absl::NotFoundError(absl::StrCat("Backup not found: ", name));
  }
  BackupEntry previous = it->second;

  std::filesystem::path snapshot_root =
      std::filesystem::path(SnapshotDirectory(name)).parent_path();
  std::filesystem::path staged_root =
      absl::StrCat(snapshot_root.string(), ".deleting");
  bool staged = false;
  if (persistent()) {
    std::error_code error;
    if (std::filesystem::exists(staged_root, error)) {
      std::filesystem::remove_all(staged_root, error);
    }
    if (error) {
      return absl::InternalError(absl::StrCat(
          "Failed to clean staged backup deletion ", staged_root.string(),
          ": ", error.message()));
    }
    std::filesystem::rename(snapshot_root, staged_root, error);
    if (error) {
      return absl::InternalError(absl::StrCat(
          "Failed to stage backup snapshot deletion ", name, ": ",
          error.message()));
    }
    staged = true;
  }

  backups_.erase(it);
  const ChainRemoval chain_removal = RemoveFromChainLocked(previous.backup);
  absl::Status status = SaveLocked();
  if (!status.ok()) {
    backups_.emplace(name, std::move(previous));
    UndoChainRemovalLocked(chain_removal);
    if (staged) {
      std::error_code rollback_error;
      std::filesystem::rename(staged_root, snapshot_root, rollback_error);
      if (rollback_error) {
        return absl::DataLossError(absl::StrCat(
            status.message(), "; failed to restore staged snapshot: ",
            rollback_error.message()));
      }
    }
    return status;
  }

  if (staged) {
    std::error_code error;
    std::filesystem::remove_all(staged_root, error);
    if (error) {
      backups_.emplace(name, previous);
      UndoChainRemovalLocked(chain_removal);
      absl::Status rollback_status = SaveLocked();
      std::error_code rename_error;
      std::filesystem::rename(staged_root, snapshot_root, rename_error);
      if (!rollback_status.ok() || rename_error) {
        return absl::DataLossError(absl::StrCat(
            "Failed to delete backup snapshot ", name,
            " and rollback catalog state"));
      }
      return absl::InternalError(absl::StrCat(
          "Failed to delete backup snapshot ", name, ": ", error.message()));
    }
  }
  return absl::OkStatus();
}

void BackupCatalog::RefreshChainLocked(const std::string& chain_id) {
  database_api::Backup* newest = nullptr;
  for (auto& [name, entry] : backups_) {
    if (entry.backup.incremental_backup_chain_id() != chain_id) continue;
    entry.backup.set_freeable_size_bytes(0);
    if (newest == nullptr || OlderInChain(*newest, entry.backup)) {
      newest = &entry.backup;
    }
  }
  if (newest != nullptr) {
    newest->set_freeable_size_bytes(newest->exclusive_size_bytes());
  }
}

BackupCatalog::ChainRemoval BackupCatalog::RemoveFromChainLocked(
    const database_api::Backup& backup) {
  ChainRemoval removal{.chain_id = backup.incremental_backup_chain_id()};
  if (removal.chain_id.empty()) return removal;
  database_api::Backup* younger = nullptr;
  for (auto& [name, entry] : backups_) {
    if (entry.backup.incremental_backup_chain_id() == removal.chain_id &&
        OlderInChain(backup, entry.backup) &&
        (younger == nullptr || OlderInChain(entry.backup, *younger))) {
      younger = &entry.backup;
    }
  }
  if (younger != nullptr) {
    // Production keeps a deleted backup's data while younger backups in the
    // chain need it, so the next younger backup now accounts for it.
    removal.younger_backup = younger->name();
    removal.younger_exclusive_size_bytes = younger->exclusive_size_bytes();
    younger->set_exclusive_size_bytes(younger->exclusive_size_bytes() +
                                      backup.exclusive_size_bytes());
  }
  for (auto chain = schedule_chains_.begin();
       chain != schedule_chains_.end();) {
    if (chain->second.newest_backup == backup.name()) {
      removal.ended_chains.insert(*chain);
      chain = schedule_chains_.erase(chain);
    } else {
      ++chain;
    }
  }
  RefreshChainLocked(removal.chain_id);
  return removal;
}

void BackupCatalog::UndoChainRemovalLocked(const ChainRemoval& removal) {
  if (removal.chain_id.empty()) return;
  if (!removal.younger_backup.empty()) {
    backups_.at(removal.younger_backup)
        .backup.set_exclusive_size_bytes(
            removal.younger_exclusive_size_bytes);
  }
  schedule_chains_.insert(removal.ended_chains.begin(),
                          removal.ended_chains.end());
  RefreshChainLocked(removal.chain_id);
}

absl::Status BackupCatalog::DeleteOperation(
    const std::string& operation_name) {
  absl::MutexLock lock(mu_);
  auto operation_it = operations_.find(operation_name);
  std::optional<google::longrunning::Operation> previous_operation;
  if (operation_it != operations_.end()) {
    previous_operation = operation_it->second;
    operations_.erase(operation_it);
  }

  std::vector<std::string> affected_backups;
  for (auto& [name, entry] : backups_) {
    if (entry.operation_name == operation_name) {
      affected_backups.push_back(name);
      entry.operation_name.clear();
    }
  }
  if (!previous_operation.has_value() && affected_backups.empty()) {
    return absl::OkStatus();
  }

  absl::Status status = SaveLocked();
  if (!status.ok()) {
    if (previous_operation.has_value()) {
      operations_.emplace(operation_name, std::move(*previous_operation));
    }
    for (const std::string& name : affected_backups) {
      backups_.at(name).operation_name = operation_name;
    }
  }
  return status;
}

absl::Status BackupCatalog::CreateBackupSchedule(
    database_api::BackupSchedule schedule, int64_t next_due_seconds) {
  const std::string name = schedule.name();
  absl::MutexLock lock(mu_);
  if (schedules_.contains(name)) {
    return absl::AlreadyExistsError(
        absl::StrCat("Backup schedule already exists: ", name));
  }
  schedules_.emplace(name, std::move(schedule));
  schedule_cursors_.emplace(name, next_due_seconds);
  absl::Status status = SaveLocked();
  if (!status.ok()) {
    schedules_.erase(name);
    schedule_cursors_.erase(name);
  }
  return status;
}

absl::StatusOr<database_api::BackupSchedule> BackupCatalog::GetBackupSchedule(
    const std::string& name) const {
  absl::ReaderMutexLock lock(mu_);
  auto it = schedules_.find(name);
  if (it == schedules_.end()) {
    return absl::NotFoundError(
        absl::StrCat("Backup schedule not found: ", name));
  }
  return it->second;
}

std::vector<database_api::BackupSchedule> BackupCatalog::ListBackupSchedules(
    const std::string& parent) const {
  absl::ReaderMutexLock lock(mu_);
  std::vector<database_api::BackupSchedule> result;
  for (const auto& [name, schedule] : schedules_) {
    if (ResourceParent(name) == parent) result.push_back(schedule);
  }
  return result;
}

std::vector<BackupCatalog::ScheduleEntry> BackupCatalog::AllBackupSchedules()
    const {
  absl::ReaderMutexLock lock(mu_);
  std::vector<ScheduleEntry> result;
  for (const auto& [name, schedule] : schedules_) {
    result.push_back({schedule, schedule_cursors_.at(name)});
  }
  return result;
}

std::optional<BackupCatalog::BackupChain> BackupCatalog::GetBackupChain(
    const std::string& schedule_name) const {
  absl::ReaderMutexLock lock(mu_);
  auto chain = schedule_chains_.find(schedule_name);
  if (chain == schedule_chains_.end()) return std::nullopt;
  return chain->second;
}

absl::Status BackupCatalog::UpdateBackupSchedule(
    const database_api::BackupSchedule& schedule,
    std::optional<int64_t> next_due_seconds) {
  absl::MutexLock lock(mu_);
  auto it = schedules_.find(schedule.name());
  if (it == schedules_.end()) {
    return absl::NotFoundError(
        absl::StrCat("Backup schedule not found: ", schedule.name()));
  }
  database_api::BackupSchedule previous = it->second;
  const int64_t previous_due = schedule_cursors_.at(schedule.name());
  it->second = schedule;
  if (next_due_seconds.has_value()) {
    schedule_cursors_.at(schedule.name()) = *next_due_seconds;
  }
  std::optional<BackupChain> previous_chain;
  if (auto chain = schedule_chains_.find(schedule.name());
      chain != schedule_chains_.end()) {
    previous_chain = std::move(chain->second);
    schedule_chains_.erase(chain);
  }
  absl::Status status = SaveLocked();
  if (!status.ok()) {
    it->second = std::move(previous);
    schedule_cursors_.at(schedule.name()) = previous_due;
    if (previous_chain.has_value()) {
      schedule_chains_.emplace(schedule.name(), *std::move(previous_chain));
    }
  }
  return status;
}

absl::Status BackupCatalog::AdvanceBackupSchedule(
    const std::string& name, int64_t expected_due_seconds,
    int64_t next_due_seconds, const std::string& expected_schedule) {
  absl::MutexLock lock(mu_);
  auto cursor = schedule_cursors_.find(name);
  auto schedule = schedules_.find(name);
  if (cursor == schedule_cursors_.end() || schedule == schedules_.end() ||
      cursor->second != expected_due_seconds ||
      schedule->second.SerializeAsString() != expected_schedule ||
      next_due_seconds <= expected_due_seconds) {
    return absl::AbortedError("Backup schedule changed during advance");
  }
  cursor->second = next_due_seconds;
  absl::Status status = SaveLocked();
  if (!status.ok()) cursor->second = expected_due_seconds;
  return status;
}

absl::Status BackupCatalog::DeleteBackupSchedule(const std::string& name) {
  absl::MutexLock lock(mu_);
  auto it = schedules_.find(name);
  if (it == schedules_.end()) {
    return absl::NotFoundError(
        absl::StrCat("Backup schedule not found: ", name));
  }
  database_api::BackupSchedule previous = it->second;
  const int64_t previous_due = schedule_cursors_.at(name);
  std::optional<BackupChain> previous_chain;
  if (auto chain = schedule_chains_.find(name);
      chain != schedule_chains_.end()) {
    previous_chain = std::move(chain->second);
    schedule_chains_.erase(chain);
  }
  schedules_.erase(it);
  schedule_cursors_.erase(name);
  absl::Status status = SaveLocked();
  if (!status.ok()) {
    schedules_.emplace(name, std::move(previous));
    schedule_cursors_.emplace(name, previous_due);
    if (previous_chain.has_value()) {
      schedule_chains_.emplace(name, *std::move(previous_chain));
    }
  }
  return status;
}

std::string BackupCatalog::SnapshotDirectory(
    const std::string& backup_name) const {
  return absl::StrCat(data_dir_, "/backups/", EncodePathComponent(backup_name),
                      "/storage");
}

absl::Status BackupCatalog::PrepareSnapshot(
    const std::string& backup_name) const {
  if (!persistent()) {
    return absl::FailedPreconditionError(
        "Native backups require emulator --data_dir persistent storage");
  }
  const std::filesystem::path root =
      std::filesystem::path(SnapshotDirectory(backup_name)).parent_path();
  std::error_code error;
  std::filesystem::create_directories(root.parent_path(), error);
  if (error) {
    return absl::InternalError(absl::StrCat(
        "Failed to prepare backup snapshot ", backup_name, ": ",
        error.message()));
  }
  if (std::filesystem::exists(root, error)) {
    return absl::AlreadyExistsError(
        absl::StrCat("Backup snapshot path already exists: ", backup_name));
  }
  if (error) {
    return absl::InternalError(absl::StrCat(
        "Failed to inspect backup snapshot ", backup_name, ": ",
        error.message()));
  }
  const std::string intent = absl::StrCat(root.string(), ".creating");
  GOOGLESQL_RETURN_IF_ERROR(
      WriteFileAtomicallyNoFollow(intent, backup_name));
  const bool created = std::filesystem::create_directory(root, error);
  if (error) {
    std::error_code inspection_error;
    if (!std::filesystem::exists(root, inspection_error) &&
        !inspection_error) {
      std::error_code ignored;
      std::filesystem::remove(intent, ignored);
    }
    return absl::InternalError(absl::StrCat(
        "Failed to prepare backup snapshot ", backup_name, ": ",
        error.message()));
  }
  if (!created) {
    std::filesystem::remove(intent, error);
    return absl::AlreadyExistsError(
        absl::StrCat("Backup snapshot path already exists: ", backup_name));
  }
  absl::Status status = WriteFileAtomicallyNoFollow(
      (root / kSnapshotOwnerMarker).string(), backup_name);
  if (!status.ok()) {
    std::error_code cleanup_error;
    std::filesystem::remove_all(root, cleanup_error);
    if (cleanup_error) {
      return absl::DataLossError(absl::StrCat(
          status.message(), "; failed to remove unfinished snapshot ",
          root.string(), ": ", cleanup_error.message()));
    }
    std::filesystem::remove(intent, cleanup_error);
    if (cleanup_error) {
      return absl::DataLossError(absl::StrCat(
          status.message(), "; failed to remove snapshot intent ", intent,
          ": ", cleanup_error.message()));
    }
    return status;
  }
  return absl::OkStatus();
}

// Thread-safety analysis cannot model ownership of mu_ transferred through
// SnapshotLease; the returned lease releases the lock.
absl::StatusOr<std::unique_ptr<BackupCatalog::SnapshotLease>>
BackupCatalog::AcquireSnapshot(const std::string& backup_name) const {
  std::unique_ptr<SnapshotLease> lease(new SnapshotLease(&mu_));
  auto entry = backups_.find(backup_name);
  if (entry == backups_.end()) {
    return absl::NotFoundError(
        absl::StrCat("Backup not found: ", backup_name));
  }
  GOOGLESQL_RETURN_IF_ERROR(ValidateSnapshotLocked(backup_name));
  lease->entry_ = entry->second;
  lease->snapshot_directory_ = SnapshotDirectory(backup_name);
  return lease;
}

absl::Status BackupCatalog::ValidateSnapshot(
    const std::string& backup_name) const {
  absl::MutexLock lock(mu_);
  return ValidateSnapshotLocked(backup_name);
}

absl::Status BackupCatalog::ValidateSnapshotLocked(
    const std::string& backup_name) const {
  const std::filesystem::path snapshot_directory =
      SnapshotDirectory(backup_name);
  const std::filesystem::path snapshot_root = snapshot_directory.parent_path();
  const std::filesystem::path staged_root =
      absl::StrCat(snapshot_root.string(), ".deleting");

  std::error_code error;
  const std::filesystem::file_status backups_status =
      std::filesystem::symlink_status(snapshot_root.parent_path(), error);
  if (error || std::filesystem::is_symlink(backups_status) ||
      !std::filesystem::is_directory(backups_status)) {
    return absl::DataLossError(absl::StrCat(
        "Backup snapshot hierarchy is unsafe: ", backup_name,
        error ? absl::StrCat(": ", error.message()) : ""));
  }
  const std::filesystem::file_status root_status =
      std::filesystem::symlink_status(snapshot_root, error);
  if (error == std::errc::no_such_file_or_directory) error.clear();
  if (error || std::filesystem::is_symlink(root_status)) {
    return absl::DataLossError(absl::StrCat(
        "Backup snapshot root is unsafe: ", backup_name,
        error ? absl::StrCat(": ", error.message()) : ""));
  }
  std::filesystem::file_status snapshot_status =
      std::filesystem::symlink_status(snapshot_directory, error);
  if (error == std::errc::no_such_file_or_directory) error.clear();
  if (error) {
    return absl::DataLossError(absl::StrCat(
        "Failed to inspect backup snapshot ", backup_name, ": ",
        error.message()));
  }
  bool snapshot_exists = std::filesystem::exists(snapshot_status);
  if (!snapshot_exists) {
    const std::filesystem::file_status staged_status =
        std::filesystem::symlink_status(staged_root, error);
    if (error == std::errc::no_such_file_or_directory) error.clear();
    if (error || std::filesystem::is_symlink(staged_status)) {
      return absl::DataLossError(absl::StrCat(
          "Backup staged snapshot root is unsafe: ", backup_name,
          error ? absl::StrCat(": ", error.message()) : ""));
    }
    if (std::filesystem::exists(staged_status)) {
      std::filesystem::rename(staged_root, snapshot_root, error);
      if (!error) {
        snapshot_status =
            std::filesystem::symlink_status(snapshot_directory, error);
        snapshot_exists = !error && std::filesystem::exists(snapshot_status);
      }
    }
  }
  if (error || !snapshot_exists ||
      std::filesystem::is_symlink(snapshot_status) ||
      !std::filesystem::is_directory(snapshot_status)) {
    return absl::DataLossError(absl::StrCat(
        "Backup snapshot is missing, unsafe, or unreadable: ", backup_name,
        error ? absl::StrCat(": ", error.message()) : ""));
  }
  leveldb::Options options;
  options.create_if_missing = false;
  options.paranoid_checks = true;
  leveldb::DB* raw_database = nullptr;
  leveldb::Status open_status =
      leveldb::DB::Open(options, snapshot_directory.string(), &raw_database);
  if (!open_status.ok()) {
    return absl::DataLossError(
        absl::StrCat("Backup snapshot is unreadable: ", backup_name, ": ",
                     open_status.ToString()));
  }
  std::unique_ptr<leveldb::DB> database(raw_database);
  std::unique_ptr<leveldb::Iterator> iterator(
      database->NewIterator(leveldb::ReadOptions()));
  for (iterator->SeekToFirst(); iterator->Valid(); iterator->Next()) {
  }
  if (!iterator->status().ok()) {
    return absl::DataLossError(
        absl::StrCat("Backup snapshot is unreadable: ", backup_name, ": ",
                     iterator->status().ToString()));
  }
  return absl::OkStatus();
}
absl::Status BackupCatalog::CleanupStaleSnapshotsLocked(
    const std::map<std::string, BackupEntry>& retained_backups) {
  if (!persistent()) return absl::OkStatus();

  const std::filesystem::path backups_root =
      std::filesystem::path(data_dir_) / "backups";
  std::error_code error;
  const std::filesystem::file_status backups_status =
      std::filesystem::symlink_status(backups_root, error);
  if (error == std::errc::no_such_file_or_directory) error.clear();
  if (error) {
    return absl::DataLossError(absl::StrCat(
        "Failed to inspect backup snapshot directory: ", error.message()));
  }
  if (!std::filesystem::exists(backups_status)) return absl::OkStatus();
  if (std::filesystem::is_symlink(backups_status) ||
      !std::filesystem::is_directory(backups_status)) {
    return absl::DataLossError(
        "Backup snapshot root must be a real directory");
  }
  std::set<std::filesystem::path> retained_roots;
  for (const auto& [name, entry] : retained_backups) {
    retained_roots.insert(
        std::filesystem::path(SnapshotDirectory(name)).parent_path());
  }

  std::vector<std::filesystem::path> candidates;
  std::filesystem::directory_iterator iterator(backups_root, error);
  const std::filesystem::directory_iterator end;
  for (; !error && iterator != end; iterator.increment(error)) {
    candidates.push_back(iterator->path());
  }
  if (error) {
    return absl::DataLossError(absl::StrCat(
        "Failed to enumerate backup snapshot directory: ", error.message()));
  }
  for (const std::filesystem::path& candidate : candidates) {
    const std::filesystem::file_status candidate_status =
        std::filesystem::symlink_status(candidate, error);
    if (error == std::errc::no_such_file_or_directory) {
      error.clear();
      continue;
    }
    if (error || std::filesystem::is_symlink(candidate_status)) {
      return absl::DataLossError(absl::StrCat(
          "Backup snapshot candidate is unsafe: ", candidate.string(),
          error ? absl::StrCat(": ", error.message()) : ""));
    }
    if (candidate.extension() == ".creating" &&
        std::filesystem::is_regular_file(candidate_status)) {
      auto owner = ReadRegularFileNoFollow(candidate.string());
      if (!owner.ok()) return owner.status();
      const std::filesystem::path owned_root =
          std::filesystem::path(SnapshotDirectory(*owner)).parent_path();
      if (candidate !=
          std::filesystem::path(absl::StrCat(owned_root.string(), ".creating"))) {
        return absl::DataLossError(absl::StrCat(
            "Backup snapshot intent does not match path: ",
            candidate.string()));
      }
      if (!retained_roots.contains(owned_root)) {
        const std::filesystem::file_status owned_status =
            std::filesystem::symlink_status(owned_root, error);
        if (error == std::errc::no_such_file_or_directory) error.clear();
        if (error || std::filesystem::is_symlink(owned_status)) {
          return absl::DataLossError(absl::StrCat(
              "Backup snapshot intent target is unsafe: ",
              owned_root.string()));
        }
        if (std::filesystem::exists(owned_status)) {
          if (!std::filesystem::is_directory(owned_status)) {
            return absl::DataLossError(absl::StrCat(
                "Backup snapshot intent target is not a directory: ",
                owned_root.string()));
          }
          std::filesystem::remove_all(owned_root, error);
          if (error) {
            return absl::DataLossError(absl::StrCat(
                "Failed to remove orphan backup snapshot ",
                owned_root.string(), ": ", error.message()));
          }
        }
      }
      std::filesystem::remove(candidate, error);
      if (error) {
        return absl::DataLossError(absl::StrCat(
            "Failed to remove backup snapshot intent ", candidate.string(),
            ": ", error.message()));
      }
      continue;
    }
    if (retained_roots.contains(candidate)) {
      continue;
    }
    if (!std::filesystem::is_directory(candidate_status)) continue;
    const std::filesystem::path marker = candidate / kSnapshotOwnerMarker;
    const std::filesystem::file_status marker_status =
        std::filesystem::symlink_status(marker, error);
    if (error == std::errc::no_such_file_or_directory) {
      error.clear();
      continue;
    }
    if (error || std::filesystem::is_symlink(marker_status) ||
        !std::filesystem::is_regular_file(marker_status)) {
      return absl::DataLossError(absl::StrCat(
          "Backup snapshot owner marker is unsafe: ", marker.string()));
    }
    auto owner = ReadRegularFileNoFollow(marker.string());
    if (!owner.ok()) return owner.status();
    const std::filesystem::path owned_root =
        std::filesystem::path(SnapshotDirectory(*owner)).parent_path();
    const bool staged_deletion = candidate.extension() == ".deleting";
    const std::filesystem::path expected_path =
        staged_deletion
            ? std::filesystem::path(
                  absl::StrCat(owned_root.string(), ".deleting"))
            : owned_root;
    if (expected_path != candidate) {
      return absl::DataLossError(absl::StrCat(
          "Backup snapshot owner marker does not match path: ",
          candidate.string()));
    }
    if (staged_deletion && retained_roots.contains(owned_root)) continue;
    std::error_code remove_error;
    std::filesystem::remove_all(candidate, remove_error);
    if (remove_error) {
      return absl::DataLossError(absl::StrCat(
          "Failed to remove stale backup snapshot ", candidate.string(), ": ",
          remove_error.message()));
    }
  }
  return absl::OkStatus();
}

absl::Status BackupCatalog::SaveLocked() const {
  if (!persistent()) return absl::OkStatus();

  json root;
  root["version"] = 6;
  root["backups"] = json::object();
  root["schedules"] = json::object();
  root["scheduleCursors"] = json::object();
  root["scheduleChains"] = json::object();
  root["operations"] = json::object();
  for (const auto& [name, entry] : backups_) {
    json batches = json::array();
    if (entry.schema_change_batches.empty()) {
      batches.push_back(
          {{"statements", entry.ddl_statements},
           {"protoDescriptors",
            absl::Base64Escape(entry.proto_descriptor_bytes)}});
    } else {
      for (const auto& batch : entry.schema_change_batches) {
        json batch_json = {
            {"statements", batch.statements},
            {"protoDescriptors",
             absl::Base64Escape(batch.proto_descriptor_bytes)}};
        if (!batch.schema_change_timestamp.empty()) {
          batch_json["schemaChangeTimestamp"] = batch.schema_change_timestamp;
        }
        batches.push_back(std::move(batch_json));
      }
    }
    root["backups"][name] = {
        {"proto", SerializeProto(entry.backup)},
        {"ddlBatches", std::move(batches)},
        {"dialect", static_cast<int>(entry.dialect)},
        {"idCounters",
         {{"tableId", entry.id_counters.table_id},
          {"columnId", entry.id_counters.column_id},
          {"changeStreamId", entry.id_counters.change_stream_id}}},
        {"operationName", entry.operation_name},
        {"sourceInstanceConfig", entry.source_instance_config},
    };
  }
  for (const auto& [name, schedule] : schedules_) {
    root["schedules"][name] = SerializeProto(schedule);
    root["scheduleCursors"][name] = schedule_cursors_.at(name);
  }
  for (const auto& [name, chain] : schedule_chains_) {
    root["scheduleChains"][name] = {
        {"id", chain.id},
        {"backupCount", chain.backup_count},
        {"oldestVersionTime", SerializeProto(chain.oldest_version_time)},
        {"newestBackup", chain.newest_backup},
    };
  }
  for (const auto& [name, operation] : operations_) {
    root["operations"][name] = SerializeProto(operation);
  }

  std::error_code error;
  std::filesystem::create_directories(data_dir_, error);
  if (error) {
    return absl::InternalError(absl::StrCat(
        "Failed to create backup catalog directory: ", error.message()));
  }
  return WriteFileAtomicallyNoFollow(catalog_path_, root.dump(2));
}

std::string BackupCatalog::ResourceParent(const std::string& resource_name) {
  const size_t separator = resource_name.find_last_of('/');
  if (separator == std::string::npos) return std::string();
  const size_t collection = resource_name.find_last_of('/', separator - 1);
  if (collection == std::string::npos) return std::string();
  return resource_name.substr(0, collection);
}

std::string BackupCatalog::EncodePathComponent(const std::string& value) {
  std::ostringstream encoded;
  encoded << std::hex << std::uppercase;
  for (unsigned char ch : value) {
    if ((ch >= 'a' && ch <= 'z') || (ch >= 'A' && ch <= 'Z') ||
        (ch >= '0' && ch <= '9') || ch == '-' || ch == '.' || ch == '_') {
      encoded << static_cast<char>(ch);
    } else {
      encoded << '%' << std::setw(2) << std::setfill('0')
              << static_cast<int>(ch);
    }
  }
  return encoded.str();
}

}  // namespace frontend
}  // namespace emulator
}  // namespace spanner
}  // namespace google

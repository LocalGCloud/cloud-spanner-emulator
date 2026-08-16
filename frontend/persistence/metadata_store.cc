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

#include "frontend/persistence/metadata_store.h"

#include <filesystem>
#include <string>

#include "absl/log/absl_log.h"
#include "absl/status/status.h"
#include "absl/strings/escaping.h"
#include "absl/strings/str_cat.h"
#include "frontend/persistence/atomic_file.h"
#include "frontend/persistence/backup_catalog.h"
#include "nlohmann/json.hpp"

namespace google {
namespace spanner {
namespace emulator {
namespace frontend {

using json = nlohmann::json;
namespace instance_api = ::google::spanner::admin::instance::v1;

MetadataStore::MetadataStore(const std::string& data_dir)
    : metadata_path_(absl::StrCat(data_dir, "/metadata.json")) {}

void MetadataStore::SetSaveHookForTesting(
    std::function<absl::Status()> hook) {
  absl::MutexLock lock(&mu_);
  save_hook_for_testing_ = std::move(hook);
}

absl::Status MetadataStore::Load() {
  absl::MutexLock lock(&mu_);
  has_metadata_ = false;
  instances_.clear();
  iam_policies_.clear();
  instance_configs_.clear();
  instance_partitions_.clear();
  pending_operations_.clear();
  pending_backup_deletions_.clear();
  pending_ddl_operations_.clear();

  std::map<std::string, InstanceInfo> loaded_instances;
  std::map<std::string, ::google::iam::v1::Policy> loaded_iam_policies;
  std::map<std::string, std::string> loaded_instance_configs;
  std::map<std::string, instance_api::InstancePartition>
      loaded_instance_partitions;
  std::map<std::string, ::google::longrunning::Operation>
      loaded_pending_operations;
  std::set<std::string> loaded_pending_backup_deletions;
  std::map<std::string, PendingDdlOperation> loaded_pending_ddl_operations;
  auto data_loss = [](const std::string& message) {
    return absl::DataLossError(message);
  };

  auto regular_file_exists =
      [&](const std::string& path,
          absl::string_view description) -> absl::StatusOr<bool> {
    std::error_code error;
    const std::filesystem::file_status status =
        std::filesystem::symlink_status(path, error);
    if (error == std::errc::no_such_file_or_directory) return false;
    if (error) {
      return data_loss(absl::StrCat("Failed to inspect ", description, " ",
                                    path, ": ", error.message()));
    }
    if (!std::filesystem::exists(status)) return false;
    if (std::filesystem::is_symlink(status) ||
        !std::filesystem::is_regular_file(status)) {
      return data_loss(absl::StrCat(
          description, " is not a regular file: ", path));
    }
    return true;
  };
  auto metadata_exists_or =
      regular_file_exists(metadata_path_, "metadata file");
  if (!metadata_exists_or.ok()) return metadata_exists_or.status();
  bool metadata_exists = *metadata_exists_or;
  const std::string tmp_path = metadata_path_ + ".tmp";
  auto tmp_exists_or =
      regular_file_exists(tmp_path, "temporary metadata file");
  if (!tmp_exists_or.ok()) return tmp_exists_or.status();
  bool tmp_exists = *tmp_exists_or;
  std::error_code filesystem_error;
  if (!metadata_exists && tmp_exists) {
    std::filesystem::rename(tmp_path, metadata_path_, filesystem_error);
    if (filesystem_error) {
      return data_loss(absl::StrCat(
          "Failed to recover temporary metadata file ", tmp_path, ": ",
          filesystem_error.message()));
    }
    metadata_exists = true;
    tmp_exists = false;
  }
  if (!metadata_exists) {
    return absl::OkStatus();
  }

  auto contents = ReadRegularFileNoFollow(metadata_path_);
  if (!contents.ok()) return contents.status();

  try {
    json j = json::parse(*contents);
    if (!j.is_object()) {
      return data_loss(
          absl::StrCat("Malformed metadata file ", metadata_path_,
                       ": root must be an object"));
    }
    if (!j.contains("version") || !j["version"].is_number_integer()) {
      return data_loss(absl::StrCat(
          "Malformed metadata file ", metadata_path_,
          ": version must be present and integer"));
    }
    const int version = j["version"].get<int>();
    if (version < 1 || version > 8) {
      return data_loss(absl::StrCat(
          "Unsupported metadata version ", version, " in ", metadata_path_));
    }
    if (!j.contains("instances") || !j["instances"].is_object()) {
      return data_loss(absl::StrCat(
          "Malformed metadata file ", metadata_path_,
          ": instances must be present and an object"));
    }
    if ((version >= 4 && !j.contains("iamPolicies")) ||
        (j.contains("iamPolicies") && !j["iamPolicies"].is_object())) {
      return data_loss(
          absl::StrCat("Malformed metadata file ", metadata_path_,
                       ": iamPolicies must be present and an object"));
    }
    if ((version >= 4 && !j.contains("instanceConfigs")) ||
        (j.contains("instanceConfigs") &&
         !j["instanceConfigs"].is_object())) {
      return data_loss(
          absl::StrCat("Malformed metadata file ", metadata_path_,
                       ": instanceConfigs must be present and an object"));
    }
    if ((version >= 4 && !j.contains("pendingOperations")) ||
        (j.contains("pendingOperations") &&
         !j["pendingOperations"].is_object())) {
      return data_loss(
          absl::StrCat("Malformed metadata file ", metadata_path_,
                       ": pendingOperations must be present and an object"));
    }
    if ((version >= 4 && !j.contains("pendingBackupDeletions")) ||
        (j.contains("pendingBackupDeletions") &&
         !j["pendingBackupDeletions"].is_array())) {
      return data_loss(absl::StrCat(
          "Malformed metadata file ", metadata_path_,
          ": pendingBackupDeletions must be present and an array"));
    }
    if ((version >= 5 && !j.contains("instancePartitions")) ||
        (j.contains("instancePartitions") &&
         !j["instancePartitions"].is_object())) {
      return data_loss(
          absl::StrCat("Malformed metadata file ", metadata_path_,
                       ": instancePartitions must be present and an object"));
    }
    if ((version >= 7 && !j.contains("pendingDdlOperations")) ||
        (j.contains("pendingDdlOperations") &&
         !j["pendingDdlOperations"].is_object())) {
      return data_loss(absl::StrCat(
          "Malformed metadata file ", metadata_path_,
          ": pendingDdlOperations must be present and an object"));
    }

    if (j.contains("iamPolicies")) {
      for (const auto& [resource, encoded_policy] :
           j["iamPolicies"].items()) {
        std::string serialized;
        ::google::iam::v1::Policy policy;
        if (!absl::Base64Unescape(encoded_policy.get<std::string>(),
                                  &serialized) ||
            !policy.ParseFromString(serialized)) {
          return data_loss(
              absl::StrCat("Malformed IAM policy for ", resource,
                           " in metadata file ", metadata_path_));
        }
        loaded_iam_policies.emplace(resource, std::move(policy));
      }
    }

    if (j.contains("instanceConfigs")) {
      for (const auto& [name, config] : j["instanceConfigs"].items()) {
        loaded_instance_configs[name] = config.get<std::string>();
      }
    }


    if (j.contains("instancePartitions")) {
      for (const auto& [name, encoded_partition] :
           j["instancePartitions"].items()) {
        std::string serialized;
        instance_api::InstancePartition partition;
        if (!encoded_partition.is_string() ||
            !absl::Base64Unescape(encoded_partition.get<std::string>(),
                                  &serialized) ||
            !partition.ParseFromString(serialized) ||
            partition.name() != name ||
            (version >= 5 &&
             (!partition.has_create_time() || !partition.has_update_time()))) {
          return data_loss(
              absl::StrCat("Malformed instance partition for ", name,
                           " in metadata file ", metadata_path_));
        }
        loaded_instance_partitions.emplace(name, std::move(partition));
      }
    }
    if (j.contains("pendingOperations")) {
      for (const auto& [name, encoded_operation] :
           j["pendingOperations"].items()) {
        std::string serialized;
        ::google::longrunning::Operation operation;
        if (!encoded_operation.is_string() ||
            !absl::Base64Unescape(encoded_operation.get<std::string>(),
                                  &serialized) ||
            !operation.ParseFromString(serialized) ||
            operation.name() != name || !operation.done()) {
          return data_loss(
              absl::StrCat("Malformed pending terminal operation for ", name,
                           " in metadata file ", metadata_path_));
        }
        loaded_pending_operations.emplace(name, std::move(operation));
      }
    }

    if (j.contains("pendingBackupDeletions")) {
      for (const auto& resource : j["pendingBackupDeletions"]) {
        if (!resource.is_string() || resource.get<std::string>().empty()) {
          return data_loss(absl::StrCat(
              "Malformed pending backup deletion in metadata file ",
              metadata_path_));
        }
        loaded_pending_backup_deletions.insert(resource.get<std::string>());
      }
    }
    if (j.contains("pendingDdlOperations")) {
      for (const auto& [database, operation_json] :
           j["pendingDdlOperations"].items()) {
        if (!operation_json.is_object() ||
            !operation_json.contains("operationName") ||
            !operation_json["operationName"].is_string() ||
            operation_json["operationName"].get<std::string>().empty() ||
            !operation_json.contains("statements") ||
            !operation_json["statements"].is_array() ||
            !operation_json.contains("protoDescriptors") ||
            !operation_json["protoDescriptors"].is_string() ||
            (version >= 8 &&
             (!operation_json.contains("hasRollbackCheckpoint") ||
              !operation_json["hasRollbackCheckpoint"].is_boolean()))) {
          return data_loss(absl::StrCat(
              "Malformed pending DDL operation for ", database,
              " in metadata file ", metadata_path_));
        }
        PendingDdlOperation operation;
        operation.operation_name =
            operation_json["operationName"].get<std::string>();
        for (const auto& statement : operation_json["statements"]) {
          if (!statement.is_string()) {
            return data_loss(absl::StrCat(
                "Malformed pending DDL statement for ", database,
                " in metadata file ", metadata_path_));
          }
          operation.statements.push_back(statement.get<std::string>());
        }
        if (!absl::Base64Unescape(
                operation_json["protoDescriptors"].get<std::string>(),
                &operation.proto_descriptor_bytes)) {
          return data_loss(absl::StrCat(
              "Malformed pending DDL descriptors for ", database,
              " in metadata file ", metadata_path_));
        }
        operation.has_rollback_checkpoint =
            operation_json.value("hasRollbackCheckpoint", false);
        loaded_pending_ddl_operations.emplace(database, std::move(operation));
      }
    }

    if (j.contains("instances")) {
      for (const auto& [inst_name, inst_json] : j["instances"].items()) {
        if (!inst_json.is_object()) {
          return data_loss(
              absl::StrCat("Malformed instance ", inst_name,
                           " in metadata file ", metadata_path_));
        }
        if (version >= 4 &&
            (!inst_json.contains("displayName") ||
             !inst_json["displayName"].is_string() ||
             !inst_json.contains("config") ||
             !inst_json["config"].is_string() ||
             !inst_json.contains("processingUnits") ||
             !inst_json["processingUnits"].is_number_integer() ||
             (version >= 6 &&
              (!inst_json.contains("nodeCount") ||
               !inst_json["nodeCount"].is_number_integer())) ||
             !inst_json.contains("createTime") ||
             !inst_json["createTime"].is_string() ||
             !inst_json.contains("updateTime") ||
             !inst_json["updateTime"].is_string() ||
             !inst_json.contains("labels") ||
             !inst_json["labels"].is_object() ||
             !inst_json.contains("databases") ||
             !inst_json["databases"].is_object())) {
          return data_loss(absl::StrCat(
              "Incomplete version 4 instance ", inst_name,
              " in metadata file ", metadata_path_));
        }
        InstanceInfo info;
        info.display_name = inst_json.value("displayName", inst_name);
        info.config = inst_json.value("config", "emulator-config");
        info.node_count = inst_json.value("nodeCount", 0);
        info.processing_units = inst_json.value("processingUnits", 1000);
        info.create_time = inst_json.value("createTime", "");
        info.update_time =
            inst_json.value("updateTime", info.create_time);
        if (info.node_count < 0 || info.processing_units < 0 ||
            (version >= 6 &&
             ((info.node_count > 0) == (info.processing_units > 0)))) {
          return data_loss(absl::StrCat(
              "Invalid persisted capacity for ", inst_name, " in ",
              metadata_path_));
        }

        if (inst_json.contains("labels")) {
          if (!inst_json["labels"].is_object()) {
            return data_loss(
                absl::StrCat("Malformed metadata file ", metadata_path_,
                             ": labels for ", inst_name,
                             " must be an object"));
          }
          for (const auto& [key, value] : inst_json["labels"].items()) {
            if (!value.is_string()) {
              return data_loss(
                  absl::StrCat("Malformed label for ", inst_name,
                               " in metadata file ", metadata_path_));
            }
            info.labels[key] = value.get<std::string>();
          }
        }

        if (inst_json.contains("databases")) {
          if (!inst_json["databases"].is_object()) {
            return data_loss(
                absl::StrCat("Malformed metadata file ", metadata_path_,
                             ": databases for ", inst_name,
                             " must be an object"));
          }
          for (const auto& [db_name, db_json] :
               inst_json["databases"].items()) {
            if (!db_json.is_object()) {
              return data_loss(
                  absl::StrCat("Malformed database ", db_name,
                               " in metadata file ", metadata_path_));
            }
            if (version >= 4 &&
                (!db_json.contains("dialect") ||
                 !db_json["dialect"].is_string() ||
                 !db_json.contains("ddlBatches") ||
                 !db_json["ddlBatches"].is_array() ||
                 !db_json.contains("createTime") ||
                 !db_json["createTime"].is_string() ||
                 !db_json.contains("enableDropProtection") ||
                 !db_json["enableDropProtection"].is_boolean() ||
                 !db_json.contains("idCounters") ||
                 !db_json["idCounters"].is_object())) {
              return data_loss(absl::StrCat(
                  "Incomplete version 4 database ", db_name,
                  " in metadata file ", metadata_path_));
            }
            DatabaseInfo db_info;
            db_info.dialect =
                db_json.value("dialect", "GOOGLE_STANDARD_SQL");
            if (db_info.dialect != "GOOGLE_STANDARD_SQL" &&
                db_info.dialect != "POSTGRESQL") {
              return data_loss(absl::StrCat(
                  "Invalid database dialect for ", db_name, " in ",
                  metadata_path_));
            }
            db_info.enable_drop_protection =
                db_json.value("enableDropProtection", false);
            db_info.create_time = db_json.value("createTime", "");
            if (db_json.contains("ddlBatches")) {
              if (!db_json["ddlBatches"].is_array()) {
                return data_loss(absl::StrCat(
                    "Malformed schema-change batches for ", db_name, " in ",
                    metadata_path_));
              }
              for (const auto& batch_json : db_json["ddlBatches"]) {
                if (!batch_json.is_object() ||
                    !batch_json.contains("statements") ||
                    !batch_json["statements"].is_array() ||
                    !batch_json.contains("protoDescriptors") ||
                    !batch_json["protoDescriptors"].is_string()) {
                  return data_loss(absl::StrCat(
                      "Malformed schema-change batch for ", db_name, " in ",
                      metadata_path_));
                }
                PersistedSchemaChangeBatch batch;
                for (const auto& statement : batch_json["statements"]) {
                  if (!statement.is_string()) {
                    return data_loss(absl::StrCat(
                        "Malformed schema-change statement for ", db_name,
                        " in ", metadata_path_));
                  }
                  batch.statements.push_back(statement.get<std::string>());
                }
                if (!absl::Base64Unescape(
                        batch_json["protoDescriptors"].get<std::string>(),
                        &batch.proto_descriptor_bytes)) {
                  return data_loss(absl::StrCat(
                      "Malformed schema-change descriptors for ", db_name,
                      " in ", metadata_path_));
                }
                if (batch_json.contains("schemaChangeTimestamp")) {
                  if (!batch_json["schemaChangeTimestamp"].is_string()) {
                    return data_loss(absl::StrCat(
                        "Malformed schema-change timestamp for ", db_name,
                        " in ", metadata_path_));
                  }
                  batch.schema_change_timestamp =
                      batch_json["schemaChangeTimestamp"].get<std::string>();
                }
                db_info.ddl_statements.insert(
                    db_info.ddl_statements.end(), batch.statements.begin(),
                    batch.statements.end());
                db_info.proto_descriptor_bytes =
                    batch.proto_descriptor_bytes;
                db_info.schema_change_batches.push_back(std::move(batch));
              }
            } else {
              if (version >= 4) {
                return data_loss(absl::StrCat(
                    "Missing schema-change batches for ", db_name, " in ",
                    metadata_path_));
              }
              if (db_json.contains("protoDescriptors")) {
                if (!db_json["protoDescriptors"].is_string() ||
                    !absl::Base64Unescape(
                        db_json["protoDescriptors"].get<std::string>(),
                        &db_info.proto_descriptor_bytes)) {
                  return data_loss(absl::StrCat(
                      "Malformed proto descriptors for ", db_name, " in ",
                      metadata_path_));
                }
              }
              if (db_json.contains("ddlStatements")) {
                if (!db_json["ddlStatements"].is_array()) {
                  return data_loss(absl::StrCat(
                      "Malformed metadata file ", metadata_path_,
                      ": ddlStatements for ", db_name,
                      " must be an array"));
                }
                for (const auto& statement : db_json["ddlStatements"]) {
                  db_info.ddl_statements.push_back(
                      statement.get<std::string>());
                }
              }
              db_info.schema_change_batches.push_back(
                  {.statements = db_info.ddl_statements,
                   .proto_descriptor_bytes =
                       db_info.proto_descriptor_bytes});
            }

            if (db_json.contains("idCounters")) {
              if (!db_json["idCounters"].is_object()) {
                return data_loss(
                    absl::StrCat("Malformed metadata file ", metadata_path_,
                                 ": idCounters for ", db_name,
                                 " must be an object"));
              }
              const auto& counters = db_json["idCounters"];
              if (version >= 4 &&
                  (!counters.contains("tableId") ||
                   !counters["tableId"].is_number_integer() ||
                   !counters.contains("columnId") ||
                   !counters["columnId"].is_number_integer() ||
                   !counters.contains("changeStreamId") ||
                   !counters["changeStreamId"].is_number_integer())) {
                return data_loss(absl::StrCat(
                    "Incomplete version 4 ID counters for ", db_name, " in ",
                    metadata_path_));
              }
              db_info.id_counters.table_id =
                  counters.value("tableId", int64_t{0});
              db_info.id_counters.column_id =
                  counters.value("columnId", int64_t{0});
              db_info.id_counters.change_stream_id =
                  counters.value("changeStreamId", int64_t{0});
              // sequenceId and namedSchemaId from older files are ignored.
            }
            if (db_info.id_counters.table_id < 0 ||
                db_info.id_counters.column_id < 0 ||
                db_info.id_counters.change_stream_id < 0) {
              return data_loss(absl::StrCat(
                  "Negative ID counter for ", db_name, " in ",
                  metadata_path_));
            }

            info.databases[db_name] = std::move(db_info);
          }
        }
        loaded_instances[inst_name] = std::move(info);
      }
    }
  } catch (const json::exception& error) {
    return data_loss(absl::StrCat("Malformed metadata file ", metadata_path_,
                                  ": ", error.what()));
  }

  instances_ = std::move(loaded_instances);
  iam_policies_ = std::move(loaded_iam_policies);
  instance_configs_ = std::move(loaded_instance_configs);
  instance_partitions_ = std::move(loaded_instance_partitions);
  pending_operations_ = std::move(loaded_pending_operations);
  pending_backup_deletions_ =
      std::move(loaded_pending_backup_deletions);
  pending_ddl_operations_ = std::move(loaded_pending_ddl_operations);
  has_metadata_ = !instances_.empty() || !iam_policies_.empty() ||
                  !instance_configs_.empty() ||
                  !instance_partitions_.empty() ||
                  !pending_operations_.empty() ||
                  !pending_backup_deletions_.empty() ||
                  !pending_ddl_operations_.empty();
  if (tmp_exists) {
    std::filesystem::remove_all(tmp_path, filesystem_error);
    if (filesystem_error) {
      instances_.clear();
      iam_policies_.clear();
      instance_configs_.clear();
      instance_partitions_.clear();
      pending_operations_.clear();
      pending_backup_deletions_.clear();
      pending_ddl_operations_.clear();
      has_metadata_ = false;
      return data_loss(absl::StrCat(
          "Failed to clean stale temporary metadata file ", tmp_path, ": ",
          filesystem_error.message()));
    }
  }
  return absl::OkStatus();
}

absl::Status MetadataStore::Save() {
  absl::MutexLock lock(&mu_);
  if (save_hook_for_testing_) {
    absl::Status hook_status = save_hook_for_testing_();
    if (!hook_status.ok()) {
      return hook_status;
    }
  }

  json j;
  j["version"] = 8;
  j["instances"] = json::object();
  j["iamPolicies"] = json::object();
  j["instanceConfigs"] = instance_configs_;
  j["instancePartitions"] = json::object();
  j["pendingOperations"] = json::object();
  j["pendingBackupDeletions"] = pending_backup_deletions_;
  j["pendingDdlOperations"] = json::object();
  for (const auto& [resource, policy] : iam_policies_) {
    j["iamPolicies"][resource] =
        absl::Base64Escape(policy.SerializeAsString());
  }
  for (const auto& [name, partition] : instance_partitions_) {
    j["instancePartitions"][name] =
        absl::Base64Escape(partition.SerializeAsString());
  }
  for (const auto& [name, operation] : pending_operations_) {
    j["pendingOperations"][name] =
        absl::Base64Escape(operation.SerializeAsString());
  }
  for (const auto& [database, operation] : pending_ddl_operations_) {
    j["pendingDdlOperations"][database] = {
        {"operationName", operation.operation_name},
        {"statements", operation.statements},
        {"protoDescriptors",
         absl::Base64Escape(operation.proto_descriptor_bytes)},
        {"hasRollbackCheckpoint", operation.has_rollback_checkpoint},
    };
  }

  for (const auto& [inst_name, info] : instances_) {
    json inst_json;
    inst_json["displayName"] = info.display_name;
    inst_json["config"] = info.config;
    inst_json["nodeCount"] = info.node_count;
    inst_json["processingUnits"] = info.processing_units;
    inst_json["createTime"] = info.create_time;
    inst_json["updateTime"] = info.update_time;
    inst_json["labels"] = info.labels;
    inst_json["databases"] = json::object();

    for (const auto& [db_name, db_info] : info.databases) {
      json db_json;
      db_json["dialect"] = db_info.dialect;
      db_json["ddlBatches"] = json::array();
      for (const auto& batch : db_info.schema_change_batches) {
        json batch_json = {
            {"statements", batch.statements},
            {"protoDescriptors",
             absl::Base64Escape(batch.proto_descriptor_bytes)}};
        if (!batch.schema_change_timestamp.empty()) {
          batch_json["schemaChangeTimestamp"] = batch.schema_change_timestamp;
        }
        db_json["ddlBatches"].push_back(std::move(batch_json));
      }
      db_json["createTime"] = db_info.create_time;
      db_json["enableDropProtection"] = db_info.enable_drop_protection;
      db_json["idCounters"] = {
          {"tableId", db_info.id_counters.table_id},
          {"columnId", db_info.id_counters.column_id},
          {"changeStreamId", db_info.id_counters.change_stream_id},
      };
      inst_json["databases"][db_name] = std::move(db_json);
    }

    j["instances"][inst_name] = std::move(inst_json);
  }

  const std::filesystem::path metadata_path(metadata_path_);
  std::error_code filesystem_error;
  std::filesystem::create_directories(metadata_path.parent_path(),
                                      filesystem_error);
  if (filesystem_error) {
    return absl::InternalError(absl::StrCat(
        "Failed to create metadata directory ",
        metadata_path.parent_path().string(), ": ",
        filesystem_error.message()));
  }
  return WriteFileAtomicallyNoFollow(metadata_path_, j.dump(2));

}

void MetadataStore::AddInstance(
    const std::string& name, const std::string& display_name,
    const std::string& config, int32_t processing_units,
    const std::map<std::string, std::string>& labels,
    const std::string& create_time, const std::string& update_time) {
  absl::MutexLock lock(&mu_);
  InstanceInfo info;
  info.display_name = display_name;
  info.config = config;
  info.processing_units = processing_units;
  info.labels = labels;
  info.create_time = create_time;
  info.update_time = update_time;
  instances_[name] = std::move(info);
}

void MetadataStore::UpdateInstance(
    const std::string& name, const std::string& config,
    const std::string& display_name, int32_t processing_units,
    const std::map<std::string, std::string>& labels,
    const std::string& update_time) {
  absl::MutexLock lock(&mu_);
  auto it = instances_.find(name);
  if (it == instances_.end()) return;
  it->second.config = config;
  it->second.display_name = display_name;
  it->second.processing_units = processing_units;
  it->second.labels = labels;
  it->second.update_time = update_time;
}

void MetadataStore::UpdateInstanceNodeCount(const std::string& name,
                                            int32_t node_count) {
  absl::MutexLock lock(&mu_);
  auto it = instances_.find(name);
  if (it == instances_.end()) return;
  it->second.node_count = node_count;
  if (node_count > 0) {
    it->second.processing_units = 0;
  }
}

void MetadataStore::RemoveInstance(const std::string& name) {
  absl::MutexLock lock(&mu_);
  instances_.erase(name);
  const std::string nested_prefix = name + "/";
  for (auto it = iam_policies_.begin(); it != iam_policies_.end();) {
    if (it->first == name || it->first.rfind(nested_prefix, 0) == 0) {
      it = iam_policies_.erase(it);
    } else {
      ++it;
    }
  }
  for (auto operation = pending_ddl_operations_.begin();
       operation != pending_ddl_operations_.end();) {
    if (operation->first.rfind(nested_prefix, 0) == 0) {
      operation = pending_ddl_operations_.erase(operation);
    } else {
      ++operation;
    }
  }
  for (auto partition = instance_partitions_.begin();
       partition != instance_partitions_.end();) {
    if (partition->first.rfind(nested_prefix, 0) == 0) {
      partition = instance_partitions_.erase(partition);
    } else {
      ++partition;
    }
  }
}

void MetadataStore::AddDatabase(
    const std::string& instance_name, const std::string& db_name,
    const std::string& dialect,
    const std::vector<std::string>& ddl_statements,
    const std::string& proto_descriptor_bytes,
    const std::string& create_time) {
  absl::MutexLock lock(&mu_);
  auto it = instances_.find(instance_name);
  if (it == instances_.end()) return;
  DatabaseInfo db_info;
  db_info.dialect = dialect;
  db_info.ddl_statements = ddl_statements;
  db_info.proto_descriptor_bytes = proto_descriptor_bytes;
  db_info.schema_change_batches.push_back(
      {.statements = ddl_statements,
       .proto_descriptor_bytes = proto_descriptor_bytes,
       .schema_change_timestamp = create_time});
  db_info.create_time = create_time;
  it->second.databases[db_name] = std::move(db_info);
}

void MetadataStore::RemoveDatabase(const std::string& instance_name,
                                   const std::string& db_name) {
  absl::MutexLock lock(&mu_);
  auto it = instances_.find(instance_name);
  if (it != instances_.end()) {
    it->second.databases.erase(db_name);
  }
  const std::string database_resource =
      instance_name + "/databases/" + db_name;
  const std::string nested_prefix = database_resource + "/";
  for (auto policy = iam_policies_.begin(); policy != iam_policies_.end();) {
    if (policy->first == database_resource ||
        policy->first.rfind(nested_prefix, 0) == 0) {
      policy = iam_policies_.erase(policy);
    } else {
      ++policy;
    }
  }
  pending_ddl_operations_.erase(database_resource);
}

void MetadataStore::UpdateDdl(
    const std::string& instance_name, const std::string& db_name,
    const std::vector<std::string>& statements,
    const std::string& proto_descriptor_bytes,
    const std::string& schema_change_timestamp) {
  absl::MutexLock lock(&mu_);
  auto it = instances_.find(instance_name);
  if (it == instances_.end()) return;
  auto db_it = it->second.databases.find(db_name);
  if (db_it == it->second.databases.end()) return;
  db_it->second.ddl_statements.insert(db_it->second.ddl_statements.end(),
                                      statements.begin(), statements.end());
  db_it->second.proto_descriptor_bytes = proto_descriptor_bytes;
  db_it->second.schema_change_batches.push_back(
      {.statements = statements,
       .proto_descriptor_bytes = proto_descriptor_bytes,
       .schema_change_timestamp = schema_change_timestamp});
}

void MetadataStore::UpdateIdCounters(const std::string& instance_name,
                                     const std::string& db_name,
                                     const IdCounters& counters) {
  absl::MutexLock lock(&mu_);
  auto it = instances_.find(instance_name);
  if (it == instances_.end()) return;
  auto db_it = it->second.databases.find(db_name);
  if (db_it == it->second.databases.end()) return;
  db_it->second.id_counters = counters;
}

void MetadataStore::UpdateDropProtection(const std::string& instance_name,
                                         const std::string& db_name,
                                         bool enabled) {
  absl::MutexLock lock(&mu_);
  auto it = instances_.find(instance_name);
  if (it == instances_.end()) return;
  auto db_it = it->second.databases.find(db_name);
  if (db_it == it->second.databases.end()) return;
  db_it->second.enable_drop_protection = enabled;
}
void MetadataStore::SetPendingDdlOperation(
    const std::string& database, const PendingDdlOperation& operation) {
  absl::MutexLock lock(&mu_);
  pending_ddl_operations_[database] = operation;
}

void MetadataStore::RemovePendingDdlOperation(const std::string& database) {
  absl::MutexLock lock(&mu_);
  pending_ddl_operations_.erase(database);
}

std::map<std::string, MetadataStore::PendingDdlOperation>
MetadataStore::AllPendingDdlOperations() const {
  absl::ReaderMutexLock lock(&mu_);
  return pending_ddl_operations_;
}


void MetadataStore::SetIamPolicy(
    const std::string& resource, const ::google::iam::v1::Policy& policy) {
  absl::MutexLock lock(&mu_);
  iam_policies_[resource] = policy;
}

void MetadataStore::RemoveIamPolicy(const std::string& resource) {
  absl::MutexLock lock(&mu_);
  iam_policies_.erase(resource);
}

std::optional<::google::iam::v1::Policy> MetadataStore::GetIamPolicy(
    const std::string& resource) const {
  absl::ReaderMutexLock lock(&mu_);
  auto it = iam_policies_.find(resource);
  if (it == iam_policies_.end()) {
    return std::nullopt;
  }
  return it->second;
}

std::map<std::string, ::google::iam::v1::Policy>
MetadataStore::AllIamPolicies() const {
  absl::ReaderMutexLock lock(&mu_);
  return iam_policies_;
}

void MetadataStore::SetInstanceConfig(const std::string& name,
                                      const std::string& encoded_config) {
  absl::MutexLock lock(&mu_);
  instance_configs_[name] = encoded_config;
}

void MetadataStore::RemoveInstanceConfig(const std::string& name) {
  absl::MutexLock lock(&mu_);
  instance_configs_.erase(name);
  iam_policies_.erase(name);
}

std::map<std::string, std::string> MetadataStore::instance_configs() const {
  absl::ReaderMutexLock lock(&mu_);
  return instance_configs_;
}

void MetadataStore::SetInstancePartition(
    const instance_api::InstancePartition& partition) {
  absl::MutexLock lock(&mu_);
  instance_partitions_[partition.name()] = partition;
}

void MetadataStore::RemoveInstancePartition(const std::string& name) {
  absl::MutexLock lock(&mu_);
  instance_partitions_.erase(name);
  const std::string nested_prefix = name + "/";
  for (auto policy = iam_policies_.begin(); policy != iam_policies_.end();) {
    if (policy->first == name ||
        policy->first.rfind(nested_prefix, 0) == 0) {
      policy = iam_policies_.erase(policy);
    } else {
      ++policy;
    }
  }
}

std::map<std::string, instance_api::InstancePartition>
MetadataStore::instance_partitions() const {
  absl::ReaderMutexLock lock(&mu_);
  return instance_partitions_;
}

void MetadataStore::SetPendingOperation(
    const ::google::longrunning::Operation& operation) {
  absl::MutexLock lock(&mu_);
  pending_operations_[operation.name()] = operation;
}

void MetadataStore::RemovePendingOperation(const std::string& name) {
  absl::MutexLock lock(&mu_);
  pending_operations_.erase(name);
}

std::map<std::string, ::google::longrunning::Operation>
MetadataStore::AllPendingOperations() const {
  absl::ReaderMutexLock lock(&mu_);
  return pending_operations_;
}

absl::Status MetadataStore::ReconcilePendingOperations(
    BackupCatalog* catalog) {
  if (catalog == nullptr) {
    return absl::InvalidArgumentError("Backup catalog must not be null");
  }
  const auto pending = AllPendingOperations();
  for (const auto& [name, operation] : pending) {
    absl::Status operation_status = catalog->SaveOperation(operation);
    if (!operation_status.ok()) {
      return absl::DataLossError(absl::StrCat(
          "Failed to reconcile pending operation ", name, ": ",
          operation_status.message()));
    }
    RemovePendingOperation(name);
  }
  if (pending.empty()) return absl::OkStatus();
  return Save();
}

void MetadataStore::SetPendingBackupDeletion(
    const std::string& resource) {
  absl::MutexLock lock(&mu_);
  pending_backup_deletions_.insert(resource);
}

void MetadataStore::RemovePendingBackupDeletion(
    const std::string& resource) {
  absl::MutexLock lock(&mu_);
  pending_backup_deletions_.erase(resource);
}

std::set<std::string> MetadataStore::AllPendingBackupDeletions() const {
  absl::ReaderMutexLock lock(&mu_);
  return pending_backup_deletions_;
}

absl::Status MetadataStore::ReconcilePendingBackupDeletions(
    BackupCatalog* catalog) {
  if (catalog == nullptr) {
    return absl::InvalidArgumentError("Backup catalog must not be null");
  }
  const std::set<std::string> pending = AllPendingBackupDeletions();
  for (const std::string& resource : pending) {
    absl::Status status;
    if (resource.find("/backupSchedules/") != std::string::npos) {
      status = catalog->DeleteBackupSchedule(resource);
    } else if (resource.find("/backups/") != std::string::npos) {
      status = catalog->DeleteBackup(resource);
    } else {
      return absl::DataLossError(
          absl::StrCat("Invalid pending backup deletion resource: ",
                       resource));
    }
    if (!status.ok() && status.code() != absl::StatusCode::kNotFound) {
      return absl::DataLossError(absl::StrCat(
          "Failed to reconcile pending backup deletion ", resource, ": ",
          status.message()));
    }
    RemovePendingBackupDeletion(resource);
  }
  if (pending.empty()) return absl::OkStatus();
  return Save();
}

std::map<std::string, MetadataStore::InstanceInfo> MetadataStore::instances()
    const {
  absl::ReaderMutexLock lock(&mu_);
  return instances_;  // Return a copy for thread safety.
}

bool MetadataStore::has_metadata() const {
  absl::ReaderMutexLock lock(&mu_);
  return has_metadata_;
}

}  // namespace frontend
}  // namespace emulator
}  // namespace spanner
}  // namespace google

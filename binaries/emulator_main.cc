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

#include <algorithm>
#include <cstddef>
#include <filesystem>
#include <map>
#include <memory>
#include <string>
#include <vector>

#include "absl/status/status.h"
#include "absl/status/statusor.h"

#include "absl/flags/parse.h"
#include "absl/log/absl_log.h"
#include "absl/strings/escaping.h"
#include "absl/strings/match.h"
#include "absl/strings/str_cat.h"
#include "absl/strings/string_view.h"
#include "absl/time/time.h"
#include "backend/database/database.h"
#include "backend/schema/updater/schema_updater.h"
#include "common/config.h"
#include "frontend/collections/database_manager.h"
#include "frontend/common/uris.h"
#include "frontend/converters/time.h"
#include "frontend/persistence/metadata_store.h"
#include "frontend/entities/operation.h"
#include "frontend/server/server.h"
#include "google/spanner/admin/database/v1/common.pb.h"
#include "google/longrunning/operations.pb.h"
#include "google/protobuf/empty.pb.h"
#include "google/spanner/admin/instance/v1/spanner_instance_admin.pb.h"
#include "googlesql/base/status_macros.h"

using Server = ::google::spanner::emulator::frontend::Server;
using OperationManager =
    ::google::spanner::emulator::frontend::OperationManager;
using DatabaseManager =
    ::google::spanner::emulator::frontend::DatabaseManager;
namespace config = ::google::spanner::emulator::config;
namespace instance_api = ::google::spanner::admin::instance::v1;
namespace database_api = ::google::spanner::admin::database::v1;

absl::StatusOr<absl::Time> ParsePersistedTime(
    absl::string_view value, absl::Time fallback,
    absl::string_view field_name, absl::string_view resource_name) {
  if (value.empty()) return fallback;
  absl::Time parsed;
  std::string error;
  if (!absl::ParseTime(absl::RFC3339_full, value, &parsed, &error)) {
    return absl::DataLossError(absl::StrCat(
        "Invalid persisted ", field_name, " for ", resource_name, ": ",
        error));
  }
  return parsed;
}

// Restores instances and databases from persisted metadata.
static absl::Status RestoreFromMetadata(Server* server) {
  auto* env = server->env();
  auto* ms = env->metadata_store();
  std::vector<std::string> database_uris;
  std::map<std::string,
           ::google::spanner::emulator::frontend::MetadataStore::
               PendingDdlOperation>
      pending_ddl_operations;
  if (ms != nullptr) {
    absl::Status metadata_status = ms->Load();
    if (!metadata_status.ok()) {
      return metadata_status;
    }

    // Legacy database IDs may collide with the backup namespace (for example,
    // a database named "backups"). Move legacy roots before the backup catalog
    // is allowed to remove uncatalogued snapshot directories.
    for (const auto& [instance_name, instance] : ms->instances()) {
      absl::string_view project_id;
      absl::string_view instance_id;
      absl::Status instance_status =
          ::google::spanner::emulator::ParseInstanceUri(
              instance_name, &project_id, &instance_id);
      if (!instance_status.ok() ||
          ::google::spanner::emulator::MakeInstanceUri(project_id,
                                                       instance_id) !=
              instance_name ||
          !::google::spanner::emulator::ValidateInstanceId(instance_id).ok()) {
        return absl::DataLossError(absl::StrCat(
            "Invalid persisted instance name ", instance_name, ": ",
            instance_status.ok() ? "non-canonical resource"
                                 : instance_status.message()));
      }
      for (const auto& [database_id, unused] : instance.databases) {
        absl::Status database_status =
            ::google::spanner::emulator::ValidateDatabaseId(database_id);
        if (!database_status.ok()) {
          return absl::DataLossError(absl::StrCat(
              "Invalid persisted database ID ", database_id, ": ",
              database_status.message()));
        }
        database_uris.push_back(
            ::google::spanner::emulator::MakeDatabaseUri(instance_name,
                                                          database_id));
      }
    }
    pending_ddl_operations = ms->AllPendingDdlOperations();
    for (const auto& [database_uri, intent] : pending_ddl_operations) {
      if (std::find(database_uris.begin(), database_uris.end(), database_uri) ==
          database_uris.end()) {
        return absl::DataLossError(absl::StrCat(
            "Pending DDL operation references missing database ",
            database_uri));
      }
      std::string operation_resource;
      absl::string_view operation_id;
      absl::Status operation_status =
          ::google::spanner::emulator::ParseOperationUri(
              intent.operation_name, &operation_resource, &operation_id);
      if (!operation_status.ok() || operation_resource != database_uri ||
          ::google::spanner::emulator::MakeOperationUri(operation_resource,
                                                        operation_id) !=
              intent.operation_name) {
        return absl::DataLossError(absl::StrCat(
            "Invalid pending DDL operation name ", intent.operation_name,
            " for ", database_uri));
      }
    }
    GOOGLESQL_RETURN_IF_ERROR(
        DatabaseManager::MigrateLegacyStorageDirectories(config::data_dir(),
                                                         database_uris));
    GOOGLESQL_RETURN_IF_ERROR(
        DatabaseManager::ReconcileDeletedDatabaseDirectories(
            config::data_dir(), database_uris));
    for (const std::string& database_uri : database_uris) {
      GOOGLESQL_RETURN_IF_ERROR(
          DatabaseManager::MarkDatabaseMetadataCommitted(config::data_dir(),
                                                         database_uri));
    }
    GOOGLESQL_RETURN_IF_ERROR(
        DatabaseManager::CleanupOrphanedRestoreDirectories(
            config::data_dir(), database_uris));
  }

  auto* backups = env->backup_catalog();
  GOOGLESQL_RETURN_IF_ERROR(backups->Load());
  if (ms != nullptr) {
    GOOGLESQL_RETURN_IF_ERROR(
        ms->ReconcilePendingBackupDeletions(backups));
    GOOGLESQL_RETURN_IF_ERROR(ms->ReconcilePendingOperations(backups));
  }
  GOOGLESQL_RETURN_IF_ERROR(
      DatabaseManager::CompleteRecoveredRestoreDirectories(
          config::data_dir(), database_uris));
  for (const auto& operation : backups->AllOperations()) {
    auto restored_operation =
        env->operation_manager()->RestoreOperation(operation);
    if (!restored_operation.ok()) {
      return absl::DataLossError(absl::StrCat(
          "Failed to restore operation ", operation.name(), ": ",
          restored_operation.status().message()));
    }
  }
  if (ms == nullptr) return absl::OkStatus();

  int restored_configs = 0;
  for (const auto& [name, encoded] : ms->instance_configs()) {
    absl::string_view project_id;
    absl::string_view config_id;
    absl::Status name_status =
        ::google::spanner::emulator::ParseInstanceConfigUri(
            name, &project_id, &config_id);
    if (!name_status.ok()) {
      return absl::DataLossError(absl::StrCat(
          "Invalid persisted instance config name ", name, ": ",
          name_status.message()));
    }
    if (::google::spanner::emulator::MakeInstanceConfigUri(project_id,
                                                            config_id) !=
        name) {
      return absl::DataLossError(absl::StrCat(
          "Non-canonical persisted instance config name ", name));
    }
    std::string serialized;
    instance_api::InstanceConfig instance_config;
    if (!absl::Base64Unescape(encoded, &serialized) ||
        !instance_config.ParseFromString(serialized)) {
      return absl::DataLossError(
          absl::StrCat("Failed to restore malformed instance config ", name));
    }
    if (!instance_config.name().empty() &&
        instance_config.name() != name) {
      return absl::DataLossError(absl::StrCat(
          "Persisted instance config key/name mismatch for ", name));
    }
    instance_config.set_name(name);
    absl::Status config_status =
        env->CreateInstanceConfig(instance_config);
    if (!config_status.ok()) {
      return absl::DataLossError(absl::StrCat(
          "Failed to restore instance config ", name, ": ",
          config_status.message()));
    }
    ++restored_configs;
  }
  if (!ms->has_metadata()) return absl::OkStatus();

  const auto instances = ms->instances();
  int restored_instances = 0;
  int restored_databases = 0;

  for (const auto& [inst_name, inst_info] : instances) {
    absl::string_view instance_project_id;
    absl::string_view instance_id;
    GOOGLESQL_RETURN_IF_ERROR(
        ::google::spanner::emulator::ParseInstanceUri(
            inst_name, &instance_project_id, &instance_id));
    std::string instance_config = inst_info.config;
    if (instance_config == "emulator-config") {
      instance_config =
          ::google::spanner::emulator::MakeInstanceConfigUri(
              instance_project_id, "emulator-config");
    } else {
      absl::string_view config_project_id;
      absl::string_view config_id;
      absl::Status config_name_status =
          ::google::spanner::emulator::ParseInstanceConfigUri(
              instance_config, &config_project_id, &config_id);
      if (!config_name_status.ok() ||
          ::google::spanner::emulator::MakeInstanceConfigUri(
              config_project_id, config_id) != instance_config ||
          config_project_id != instance_project_id) {
        return absl::DataLossError(absl::StrCat(
            "Invalid persisted instance config reference ", instance_config,
            " for ", inst_name));
      }
      if (config_id != "emulator-config" &&
          !env->GetCustomInstanceConfig(instance_config).ok()) {
        return absl::DataLossError(absl::StrCat(
            "Persisted instance ", inst_name,
            " references missing custom instance config ", instance_config));
      }
    }
    // Build instance proto for CreateInstance.
    instance_api::Instance inst_pb;
    inst_pb.set_name(inst_name);
    inst_pb.set_display_name(inst_info.display_name);
    inst_pb.set_config(instance_config);
    if (inst_info.node_count > 0) {
      inst_pb.set_node_count(inst_info.node_count);
    } else {
      inst_pb.set_processing_units(inst_info.processing_units);
    }
    for (const auto& [k, v] : inst_info.labels) {
      (*inst_pb.mutable_labels())[k] = v;
    }

    const absl::Time restore_time = env->clock()->Now();
    GOOGLESQL_ASSIGN_OR_RETURN(
        const absl::Time instance_create_time,
        ParsePersistedTime(inst_info.create_time, restore_time, "create_time",
                           inst_name));
    GOOGLESQL_ASSIGN_OR_RETURN(
        const absl::Time instance_update_time,
        ParsePersistedTime(inst_info.update_time, instance_create_time,
                           "update_time", inst_name));
    auto inst_or = env->instance_manager()->CreateInstance(
        inst_name, inst_pb, instance_create_time, instance_update_time);
    if (!inst_or.ok()) {
      return absl::DataLossError(absl::StrCat(
          "Failed to restore instance ", inst_name, ": ",
          inst_or.status().message()));
    }
    restored_instances++;

    // Restore databases under this instance.
    for (const auto& [db_name, db_info] : inst_info.databases) {
      std::string database_uri =
          ::google::spanner::emulator::MakeDatabaseUri(inst_name, db_name);

      // Determine dialect.
      database_api::DatabaseDialect dialect;
      if (db_info.dialect == "POSTGRESQL") {
        dialect = database_api::DatabaseDialect::POSTGRESQL;
      } else if (db_info.dialect == "GOOGLE_STANDARD_SQL") {
        dialect = database_api::DatabaseDialect::GOOGLE_STANDARD_SQL;
      } else {
        return absl::DataLossError(absl::StrCat(
            "Invalid persisted database dialect ", db_info.dialect, " for ",
            database_uri));
      }

      GOOGLESQL_ASSIGN_OR_RETURN(
          const absl::Time database_create_time,
          ParsePersistedTime(db_info.create_time, env->clock()->Now(),
                             "create_time", database_uri));
      std::vector<std::vector<std::string>> statement_batches;
      std::vector<google::spanner::emulator::backend::SchemaChangeOperation>
          schema_change_operations;
      statement_batches.reserve(db_info.schema_change_batches.size());
      schema_change_operations.reserve(db_info.schema_change_batches.size());
      for (std::size_t batch_index = 0;
           batch_index < db_info.schema_change_batches.size();
           ++batch_index) {
        const auto& batch = db_info.schema_change_batches[batch_index];
        statement_batches.emplace_back();
        for (const auto& statement : batch.statements) {
          if (!absl::StartsWithIgnoreCase(statement, "CREATE DATABASE")) {
            statement_batches.back().push_back(statement);
          }
        }
        // Replay each batch at its original commit timestamp so time-based
        // schema state, like change stream creation times, survives restart.
        const absl::Time timestamp_fallback = batch_index == 0
                                                  ? database_create_time
                                                  : absl::InfinitePast();
        GOOGLESQL_ASSIGN_OR_RETURN(
            const absl::Time batch_timestamp,
            ParsePersistedTime(batch.schema_change_timestamp,
                               timestamp_fallback, "schemaChangeTimestamp",
                               database_uri));
        schema_change_operations.push_back(
            {.statements = statement_batches.back(),
             .proto_descriptor_bytes = batch.proto_descriptor_bytes,
             .database_dialect = dialect,
             .schema_change_timestamp = batch_timestamp});
      }
      google::spanner::emulator::backend::Database::IdCounterValues counters{
          .table_id = db_info.id_counters.table_id,
          .column_id = db_info.id_counters.column_id,
          .change_stream_id = db_info.id_counters.change_stream_id,
      };
      if (counters.table_id < 0 || counters.column_id < 0 ||
          counters.change_stream_id < 0) {
        return absl::DataLossError(absl::StrCat(
            "Persisted ID counters must be non-negative for ", database_uri));
      }
      auto pending_ddl = pending_ddl_operations.find(database_uri);
      if (pending_ddl != pending_ddl_operations.end() &&
          pending_ddl->second.has_rollback_checkpoint) {
        GOOGLESQL_RETURN_IF_ERROR(
            DatabaseManager::RestoreDdlRollbackCheckpoint(
                config::data_dir(), database_uri,
                pending_ddl->second.operation_name));
      } else {
        GOOGLESQL_RETURN_IF_ERROR(
            DatabaseManager::RemoveDdlRollbackCheckpoints(
                config::data_dir(), database_uri));
      }

      auto storage_directory_or =
          google::spanner::emulator::backend::Database::
              PersistentStorageDirectory(config::data_dir(), database_uri);
      if (!storage_directory_or.ok()) {
        return absl::DataLossError(absl::StrCat(
            "Invalid persisted database storage path for ", database_uri,
            ": ", storage_directory_or.status().message()));
      }
      const std::filesystem::path storage_directory =
          *storage_directory_or;
      std::error_code storage_error;
      if (!std::filesystem::is_directory(storage_directory, storage_error) ||
          storage_error) {
        return absl::DataLossError(absl::StrCat(
            "Persisted database storage is missing or unreadable for ",
            database_uri, ": ", storage_directory.string(),
            storage_error ? absl::StrCat(": ", storage_error.message()) : ""));
      }

      GOOGLESQL_ASSIGN_OR_RETURN(
          std::unique_ptr<
              google::spanner::emulator::frontend::DatabaseManager::Creation>
              creation,
          env->database_manager()->ReserveDatabase(database_uri));
      auto db_or = creation->Build(schema_change_operations, counters,
                                   database_create_time);
      if (!db_or.ok()) {
        return absl::DataLossError(absl::StrCat(
            "Failed to restore database ", database_uri, ": ",
            db_or.status().message()));
      }
      if (pending_ddl != pending_ddl_operations.end()) {
        const auto& intent = pending_ddl->second;
        std::string operation_resource;
        absl::string_view operation_id;
        GOOGLESQL_RETURN_IF_ERROR(
            ::google::spanner::emulator::ParseOperationUri(
                intent.operation_name, &operation_resource, &operation_id));
        GOOGLESQL_ASSIGN_OR_RETURN(
            std::shared_ptr<
                ::google::spanner::emulator::frontend::Operation>
                operation,
            env->operation_manager()->CreateOperation(
                database_uri, std::string(operation_id)));

        int successful_statements = 0;
        absl::Time commit_timestamp;
        absl::Status backfill_status;
        absl::Status update_status = (*db_or)->backend()->UpdateSchema(
            {.statements = intent.statements,
             .proto_descriptor_bytes = intent.proto_descriptor_bytes,
             .database_dialect = dialect},
            &successful_statements, &commit_timestamp, &backfill_status);

        database_api::UpdateDatabaseDdlMetadata update_metadata;
        update_metadata.set_database(database_uri);
        for (const std::string& statement : intent.statements) {
          update_metadata.add_statements(statement);
        }
        if (update_status.ok()) {
          for (int i = 0; i < successful_statements; ++i) {
            GOOGLESQL_ASSIGN_OR_RETURN(
                *update_metadata.add_commit_timestamps(),
                ::google::spanner::emulator::TimestampToProto(
                    commit_timestamp));
          }
        }
        operation->SetMetadata(update_metadata);
        if (!update_status.ok()) {
          operation->SetError(update_status);
        } else if (!backfill_status.ok()) {
          operation->SetError(backfill_status);
        } else {
          operation->SetResponse(::google::protobuf::Empty());
        }

        ::google::longrunning::Operation operation_proto;
        operation->ToProto(&operation_proto);
        if (update_status.ok()) {
          ms->UpdateDdl(
              inst_name, db_name,
              std::vector<std::string>(
                  intent.statements.begin(),
                  intent.statements.begin() + successful_statements),
              intent.proto_descriptor_bytes,
              absl::FormatTime(absl::RFC3339_full, commit_timestamp,
                               absl::UTCTimeZone()));
          const auto counters = (*db_or)->backend()->GetIdCounterValues();
          ms->UpdateIdCounters(
              inst_name, db_name,
              {.table_id = counters.table_id,
               .column_id = counters.column_id,
               .change_stream_id = counters.change_stream_id});
        }
        ms->RemovePendingDdlOperation(database_uri);
        ms->SetPendingOperation(operation_proto);
        GOOGLESQL_RETURN_IF_ERROR(ms->Save());
        GOOGLESQL_RETURN_IF_ERROR(
            DatabaseManager::RemoveDdlRollbackCheckpoints(
                config::data_dir(), database_uri));
        GOOGLESQL_RETURN_IF_ERROR(backups->SaveOperation(operation_proto));
        ms->RemovePendingOperation(operation_proto.name());
        GOOGLESQL_RETURN_IF_ERROR(ms->Save());
      }

      GOOGLESQL_RETURN_IF_ERROR(creation->Publish());
      (*db_or)->set_enable_drop_protection(db_info.enable_drop_protection);
      restored_databases++;
      const std::filesystem::path restore_marker =
          storage_directory.parent_path() / ".restore-in-progress";
      std::filesystem::remove(restore_marker, storage_error);
      if (storage_error) {
        return absl::DataLossError(absl::StrCat(
            "Failed to remove completed restore marker for ", database_uri,
            ": ", storage_error.message()));
      }
    }
  }

  int restored_partitions = 0;
  for (const auto& [name, partition] : ms->instance_partitions()) {
    absl::string_view project_id;
    absl::string_view instance_id;
    absl::string_view partition_id;
    GOOGLESQL_RETURN_IF_ERROR(
        ::google::spanner::emulator::ParseInstancePartitionUri(
            name, &project_id, &instance_id, &partition_id));
    const std::string parent =
        ::google::spanner::emulator::MakeInstanceUri(project_id, instance_id);
    if (::google::spanner::emulator::MakeInstancePartitionUri(
            parent, partition_id) != name) {
      return absl::DataLossError(absl::StrCat(
          "Non-canonical persisted instance partition name ", name));
    }
    GOOGLESQL_RETURN_IF_ERROR(
        env->instance_manager()->GetInstance(parent).status());
    absl::string_view config_project_id;
    absl::string_view config_id;
    absl::Status config_status =
        ::google::spanner::emulator::ParseInstanceConfigUri(
            partition.config(), &config_project_id, &config_id);
    if (!config_status.ok() || config_project_id != project_id ||
        ::google::spanner::emulator::MakeInstanceConfigUri(
            config_project_id, config_id) != partition.config() ||
        (config_id != "emulator-config" &&
         !env->GetCustomInstanceConfig(partition.config()).ok())) {
      return absl::DataLossError(absl::StrCat(
          "Invalid persisted instance partition config ",
          partition.config(), " for ", name));
    }
    if (partition.state() != instance_api::InstancePartition::READY) {
      return absl::DataLossError(
          absl::StrCat("Persisted instance partition is not READY: ", name));
    }
    GOOGLESQL_ASSIGN_OR_RETURN(
        const absl::Time create_time,
        ::google::spanner::emulator::TimestampFromProto(
            partition.create_time()));
    GOOGLESQL_ASSIGN_OR_RETURN(
        const absl::Time update_time,
        ::google::spanner::emulator::TimestampFromProto(
            partition.update_time()));
    auto restored_partition =
        env->instance_partition_manager()->CreateInstancePartition(
            name, partition, create_time, update_time);
    if (!restored_partition.ok()) {
      return absl::DataLossError(absl::StrCat(
          "Failed to restore instance partition ", name, ": ",
          restored_partition.status().message()));
    }
    ++restored_partitions;
  }

  int restored_policies = 0;
  for (const auto& [resource, policy] : ms->AllIamPolicies()) {
    absl::Status resource_status = env->ValidateIamResource(resource);
    if (!resource_status.ok()) {
      return absl::DataLossError(absl::StrCat(
          "Persisted IAM policy references an invalid or missing resource ",
          resource, ": ", resource_status.message()));
    }
    env->SetIamPolicy(resource, policy);
    ++restored_policies;
  }

  ABSL_LOG(INFO) << "Restored " << restored_instances << " instance(s), "
                 << restored_databases << " database(s), "
                 << restored_partitions << " instance partition(s), "
                 << restored_policies << " IAM policy/policies, and "
                 << restored_configs << " custom instance config(s) from "
                 << config::data_dir() << "/metadata.json";
  return absl::OkStatus();
}

int main(int argc, char** argv) {
  absl::ParseCommandLine(argc, argv);
  std::unique_ptr<Server> server = Server::CreateUnstarted();

  // Complete all durable-state hydration before the listener can accept RPCs.
  if (!config::data_dir().empty()) {
    absl::Status restore_status = RestoreFromMetadata(server.get());
    if (!restore_status.ok()) {
      ABSL_LOG(ERROR) << "Failed to restore persisted state: "
                      << restore_status;
      return EXIT_FAILURE;
    }
  }

  Server::Options options;
  options.server_address = config::grpc_host_port();
  if (!server->Start(options)) {
    ABSL_LOG(ERROR) << "Failed to start gRPC server.";
    return EXIT_FAILURE;
  }

  ABSL_LOG(INFO) << "Cloud Spanner Emulator running.";
  ABSL_LOG(INFO) << "Server address: "
                 << absl::StrCat(server->host(), ":", server->port());

  // Block forever until the server is terminated.
  server->WaitForShutdown();

  return EXIT_SUCCESS;
}

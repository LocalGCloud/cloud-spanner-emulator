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

#include <limits>
#include <map>
#include <string>

#include "absl/log/absl_log.h"
#include "absl/status/status.h"
#include "absl/status/statusor.h"
#include "absl/strings/str_cat.h"
#include "absl/synchronization/mutex.h"
#include "absl/time/time.h"
#include "common/errors.h"
#include "common/limits.h"
#include "frontend/collections/instance_partition_manager.h"
#include "frontend/collections/operation_manager.h"
#include "frontend/common/labels.h"
#include "frontend/common/uris.h"
#include "frontend/converters/time.h"
#include "frontend/entities/instance.h"
#include "frontend/entities/instance_partition.h"
#include "frontend/persistence/backup_catalog.h"
#include "frontend/entities/operation.h"
#include "frontend/server/handler.h"
#include "google/longrunning/operations.pb.h"
#include "google/protobuf/empty.pb.h"
#include "google/spanner/admin/instance/v1/spanner_instance_admin.pb.h"
#include "googlesql/base/status_macros.h"
#include "re2/re2.h"

namespace instance_api = ::google::spanner::admin::instance::v1;
namespace operations_api = ::google::longrunning;
namespace protobuf_api = ::google::protobuf;

namespace google {
namespace spanner {
namespace emulator {
namespace frontend {

namespace {

const absl::string_view kEmulatorInstanceConfig = "emulator-config";

// Hard-coded instance config for emulator.
instance_api::InstanceConfig GetEmulatorInstanceConfig(
    const absl::string_view project_id) {
  instance_api::InstanceConfig config;
  config.set_name(absl::StrCat("projects/", project_id, "/instanceConfigs/",
                               kEmulatorInstanceConfig));
  config.set_display_name("Emulator Instance Config");
  return config;
}

absl::StatusOr<std::string> CanonicalInstanceConfig(
    ServerEnv* env, absl::string_view project_id,
    absl::string_view config_reference) {
  const std::string built_in =
      MakeInstanceConfigUri(project_id, kEmulatorInstanceConfig);
  if (config_reference == kEmulatorInstanceConfig) return built_in;

  absl::string_view config_project_id;
  absl::string_view config_id;
  GOOGLESQL_RETURN_IF_ERROR(ParseInstanceConfigUri(
      config_reference, &config_project_id, &config_id));
  const std::string canonical =
      MakeInstanceConfigUri(config_project_id, config_id);
  if (canonical != config_reference || config_project_id != project_id) {
    return absl::InvalidArgumentError(
        "Instance config must be a canonical resource in the same project");
  }
  if (config_id != kEmulatorInstanceConfig) {
    GOOGLESQL_RETURN_IF_ERROR(
        env->GetCustomInstanceConfig(canonical).status());
  }
  return canonical;
}

}  // namespace

// Lists the supported instance configurations for a given project.
absl::Status ListInstanceConfigs(
    RequestContext* ctx,
    const instance_api::ListInstanceConfigsRequest* request,
    instance_api::ListInstanceConfigsResponse* response) {
  absl::string_view project_id;
  GOOGLESQL_RETURN_IF_ERROR(ParseProjectUri(request->parent(), &project_id));
  if (MakeProjectUri(project_id) != request->parent()) {
    return absl::InvalidArgumentError("Project name must be canonical");
  }
  *response->add_instance_configs() = GetEmulatorInstanceConfig(project_id);
  for (const auto& config :
       ctx->env()->ListCustomInstanceConfigs(request->parent())) {
    *response->add_instance_configs() = config;
  }
  return absl::OkStatus();
}
REGISTER_GRPC_HANDLER(InstanceAdmin, ListInstanceConfigs);

// Gets information about a particular instance configuration.
absl::Status GetInstanceConfig(
    RequestContext* ctx, const instance_api::GetInstanceConfigRequest* request,
    instance_api::InstanceConfig* response) {
  absl::string_view project_id, instance_config_id;
  GOOGLESQL_RETURN_IF_ERROR(ParseInstanceConfigUri(request->name(), &project_id,
                                                   &instance_config_id));
  if (MakeInstanceConfigUri(project_id, instance_config_id) != request->name()) {
    return absl::InvalidArgumentError(
        "Instance config name must be canonical");
  }
  if (instance_config_id == kEmulatorInstanceConfig) {
    *response = GetEmulatorInstanceConfig(project_id);
    return absl::OkStatus();
  }
  GOOGLESQL_ASSIGN_OR_RETURN(
      *response, ctx->env()->GetCustomInstanceConfig(request->name()));
  return absl::OkStatus();
}
REGISTER_GRPC_HANDLER(InstanceAdmin, GetInstanceConfig);

// Lists all instances in a project.
absl::Status ListInstances(RequestContext* ctx,
                           const instance_api::ListInstancesRequest* request,
                           instance_api::ListInstancesResponse* response) {
  // Validate that the ListInstances request is for a valid project.
  absl::string_view project_id;
  GOOGLESQL_RETURN_IF_ERROR(ParseProjectUri(request->parent(), &project_id));
  if (MakeProjectUri(project_id) != request->parent()) {
    return absl::InvalidArgumentError("Project name must be canonical");
  }

  // Validate that the page_token provided is a valid instance_uri.
  if (!request->page_token().empty()) {
    absl::string_view project_id, instance_id;
    GOOGLESQL_RETURN_IF_ERROR(
        ParseInstanceUri(request->page_token(), &project_id, &instance_id));
    if (MakeInstanceUri(project_id, instance_id) != request->page_token() ||
        MakeProjectUri(project_id) != request->parent()) {
      return absl::InvalidArgumentError(
          "Page token must be a canonical instance in the parent project");
    }
  }

  GOOGLESQL_ASSIGN_OR_RETURN(
      std::vector<std::shared_ptr<Instance>> instances,
      ctx->env()->instance_manager()->ListInstances(request->parent()));

  int32_t page_size = request->page_size();
  static const int32_t kMaxPageSize = 1000;
  if (page_size <= 0 || page_size > kMaxPageSize) {
    page_size = kMaxPageSize;
  }

  // Instances returned from instance manager are sorted by instance_uri and
  // thus we use instance uri of first instance in next page as next_page_token.
  for (const auto& instance : instances) {
    if (response->instances_size() >= page_size) {
      response->set_next_page_token(instance->instance_uri());
      break;
    }
    if (instance->instance_uri() >= request->page_token()) {
      instance->ToProto(response->add_instances());
    }
  }
  return absl::OkStatus();
}
REGISTER_GRPC_HANDLER(InstanceAdmin, ListInstances);

// Gets information about a particular instance.
absl::Status GetInstance(RequestContext* ctx,
                         const instance_api::GetInstanceRequest* request,
                         instance_api::Instance* response) {
  GOOGLESQL_ASSIGN_OR_RETURN(std::shared_ptr<Instance> instance,
                             GetInstance(ctx, request->name()));
  instance->ToProto(response);
  return absl::OkStatus();
}
REGISTER_GRPC_HANDLER(InstanceAdmin, GetInstance);

// Creates an instance.
absl::Status CreateInstance(RequestContext* ctx,
                            const instance_api::CreateInstanceRequest* request,
                            operations_api::Operation* response) {
  absl::MutexLock admin_transaction_lock(
      &ctx->env()->admin_transaction_mutex());
  // Verify that the instance creation request is valid.
  absl::string_view project_id;
  GOOGLESQL_RETURN_IF_ERROR(ParseProjectUri(request->parent(), &project_id));
  if (MakeProjectUri(project_id) != request->parent()) {
    return absl::InvalidArgumentError("Project name must be canonical");
  }
  std::string instance_uri =
      MakeInstanceUri(project_id, request->instance_id());
  if (!request->instance().name().empty() &&
      request->instance().name() != instance_uri) {
    return error::InstanceNameMismatch(request->instance().name());
  }

  // Validate instance name.
  GOOGLESQL_RETURN_IF_ERROR(ValidateInstanceId(request->instance_id()));

  // Validate labels.
  GOOGLESQL_RETURN_IF_ERROR(ValidateLabels(request->instance().labels()));
  instance_api::Instance instance_request = request->instance();
  GOOGLESQL_ASSIGN_OR_RETURN(
      const std::string canonical_config,
      CanonicalInstanceConfig(ctx->env(), project_id,
                              instance_request.config()));
  instance_request.set_config(canonical_config);

  // Allocate the operation before mutating instance state.
  GOOGLESQL_ASSIGN_OR_RETURN(
      std::shared_ptr<Operation> operation,
      ctx->env()->operation_manager()->CreateOperation(
          instance_uri, OperationManager::kAutoGeneratedId));
  operation->ToProto(response);
  auto instance_or =
      ctx->env()->instance_manager()->CreateInstance(instance_uri,
                                                     instance_request);
  if (!instance_or.ok()) {
    ctx->env()->operation_manager()->DeleteOperation(response->name())
        .IgnoreError();
    return instance_or.status();
  }
  std::shared_ptr<Instance> instance = *instance_or;

  // Fill in the metadata for the longrunning operation.
  instance_api::Instance instance_pb;
  instance->ToProto(&instance_pb);
  instance_api::CreateInstanceMetadata metadata_pb;
  *metadata_pb.mutable_instance() = instance_pb;

  // Update the start time of the LRO.
  GOOGLESQL_ASSIGN_OR_RETURN(*metadata_pb.mutable_start_time(),
                             TimestampToProto(ctx->env()->clock()->Now()));
  operation->SetMetadata(metadata_pb);

  // Convert to proto before setting the response so that the returned
  // longrunning operation is in !done state. Caller needs to poll the
  // longrunning operation at least once to make sure the instance is created.
  // This behavior follows the prod behavior more closely.
  // TODO: In integration tests, the client library currently
  // has a random delay with a long range before it polls the operations api.
  // This makes testing unnecessarily slow. Remove this when the issue is fixed.
  //  operation->ToProto(response);

  // Update the endtime after the LRO is conceptually completed.
  GOOGLESQL_ASSIGN_OR_RETURN(*metadata_pb.mutable_end_time(),
                             TimestampToProto(ctx->env()->clock()->Now()));
  operation->SetMetadata(metadata_pb);
  operation->SetResponse(instance_pb);

  // TODO: See discussion above, remove when the issue is fixed.
  operation->ToProto(response);

  // Persist metadata if data_dir is set.
  if (auto* ms = ctx->env()->metadata_store(); ms != nullptr) {
    std::map<std::string, std::string> labels(
        request->instance().labels().begin(),
        request->instance().labels().end());
    GOOGLESQL_ASSIGN_OR_RETURN(
        const absl::Time persisted_create_time,
        TimestampFromProto(instance_pb.create_time()));
    GOOGLESQL_ASSIGN_OR_RETURN(
        const absl::Time persisted_update_time,
        TimestampFromProto(instance_pb.update_time()));
    ms->AddInstance(
        instance_uri, instance_pb.display_name(), instance_pb.config(),
        instance_pb.processing_units(), labels,
        absl::FormatTime(absl::RFC3339_full, persisted_create_time,
                         absl::UTCTimeZone()),
        absl::FormatTime(absl::RFC3339_full, persisted_update_time,
                         absl::UTCTimeZone()));
    ms->UpdateInstanceNodeCount(instance_uri, instance_pb.node_count());
    ms->SetPendingOperation(*response);
    absl::Status save_status = ms->Save();
    if (!save_status.ok()) {
      ms->RemoveInstance(instance_uri);
      ms->RemovePendingOperation(response->name());
      ctx->env()->operation_manager()->DeleteOperation(response->name())
          .IgnoreError();
      ctx->env()->instance_manager()->DeleteInstance(instance_uri);
      return save_status;
    }
    absl::Status operation_status =
        ctx->env()->backup_catalog()->SaveOperation(*response);
    if (!operation_status.ok()) {
      ms->RemoveInstance(instance_uri);
      ms->RemovePendingOperation(response->name());
      absl::Status rollback_status = ms->Save();
      ctx->env()->operation_manager()->DeleteOperation(response->name())
          .IgnoreError();
      ctx->env()->instance_manager()->DeleteInstance(instance_uri);
      if (!rollback_status.ok()) {
        return absl::DataLossError(absl::StrCat(
            operation_status.message(),
            "; failed to roll back instance metadata: ",
            rollback_status.message()));
      }
      return operation_status;
    }
    ms->RemovePendingOperation(response->name());
    absl::Status journal_status = ms->Save();
    if (!journal_status.ok()) {
      ABSL_LOG(WARNING)
          << "Failed to clear promoted create-instance operation journal "
          << response->name() << ": " << journal_status;
    }
  }

  return absl::OkStatus();
}
REGISTER_GRPC_HANDLER(InstanceAdmin, CreateInstance);

// Updates an instance.
absl::Status UpdateInstance(RequestContext* ctx,
                            const instance_api::UpdateInstanceRequest* request,
                            operations_api::Operation* response) {
  absl::MutexLock admin_transaction_lock(
      &ctx->env()->admin_transaction_mutex());
  if (!request->has_instance() || request->instance().name().empty()) {
    return absl::InvalidArgumentError("Instance name must be provided");
  }
  absl::string_view project_id;
  absl::string_view instance_id;
  GOOGLESQL_RETURN_IF_ERROR(ParseInstanceUri(
      request->instance().name(), &project_id, &instance_id));
  if (MakeInstanceUri(project_id, instance_id) != request->instance().name()) {
    return absl::InvalidArgumentError("Instance name must be canonical");
  }
  GOOGLESQL_ASSIGN_OR_RETURN(
      std::shared_ptr<Instance> instance,
      ctx->env()->instance_manager()->GetInstance(request->instance().name()));
  if (request->field_mask().paths().empty()) {
    return absl::InvalidArgumentError("Instance field_mask must be provided");
  }

  bool update_display_name = false;
  bool update_node_count = false;
  bool update_processing_units = false;
  bool update_labels = false;
  for (const std::string& path : request->field_mask().paths()) {
    if (path == "display_name" || path == "displayName") {
      update_display_name = true;
    } else if (path == "node_count" || path == "nodeCount") {
      update_node_count = true;
    } else if (path == "processing_units" || path == "processingUnits") {
      update_processing_units = true;
    } else if (path == "labels") {
      GOOGLESQL_RETURN_IF_ERROR(ValidateLabels(request->instance().labels()));
      update_labels = true;
    } else {
      return absl::InvalidArgumentError(
          absl::StrCat("Unsupported instance update field: ", path));
    }
  }
  if (update_node_count && update_processing_units) {
    return absl::InvalidArgumentError(
        "node_count and processing_units cannot both be updated");
  }
  if (update_node_count &&
      (request->instance().node_count() <= 0 ||
       request->instance().node_count() >
           std::numeric_limits<int32_t>::max() / 1000)) {
    return absl::InvalidArgumentError("node_count is outside the valid range");
  }
  const int32_t processing_units = request->instance().processing_units();
  if (update_processing_units &&
      (processing_units <= 0 ||
       (processing_units < 1000 && processing_units % 100 != 0) ||
       (processing_units > 1000 && processing_units % 1000 != 0))) {
    return absl::InvalidArgumentError(
        "processing_units must be positive and use a supported increment");
  }

  instance_api::Instance previous_instance;
  instance->ToProto(&previous_instance);
  GOOGLESQL_ASSIGN_OR_RETURN(
      absl::Time previous_update_time,
      TimestampFromProto(previous_instance.update_time()));
  GOOGLESQL_ASSIGN_OR_RETURN(
      std::shared_ptr<Operation> operation,
      ctx->env()->operation_manager()->CreateOperation(
          previous_instance.name(), OperationManager::kAutoGeneratedId));
  operation->ToProto(response);
  const auto restore_runtime = [&] {
    instance->UpdateDisplayName(previous_instance.display_name(),
                                previous_update_time);
    if (previous_instance.node_count() > 0) {
      instance->UpdateNodeCount(previous_instance.node_count(),
                                previous_update_time);
    } else {
      instance->UpdateProcessingUnits(previous_instance.processing_units(),
                                      previous_update_time);
    }
    instance->UpdateLabels(
        Labels(previous_instance.labels().begin(),
               previous_instance.labels().end()),
        previous_update_time);
  };

  const absl::Time update_time = ctx->env()->clock()->Now();
  if (update_display_name) {
    instance->UpdateDisplayName(request->instance().display_name(), update_time);
  }
  if (update_node_count) {
    instance->UpdateNodeCount(request->instance().node_count(), update_time);
  }
  if (update_processing_units) {
    instance->UpdateProcessingUnits(processing_units, update_time);
  }
  if (update_labels) {
    instance->UpdateLabels(
        Labels(request->instance().labels().begin(),
               request->instance().labels().end()),
        update_time);
  }

  instance_api::Instance instance_proto;
  instance->ToProto(&instance_proto);
  operation->SetResponse(instance_proto);
  operation->ToProto(response);
  if (auto* metadata = ctx->env()->metadata_store(); metadata != nullptr) {
    metadata->UpdateInstance(
        instance_proto.name(), instance_proto.config(),
        instance_proto.display_name(), instance_proto.processing_units(),
        std::map<std::string, std::string>(instance_proto.labels().begin(),
                                           instance_proto.labels().end()),
        absl::FormatTime(absl::RFC3339_full, update_time,
                         absl::UTCTimeZone()));
    metadata->UpdateInstanceNodeCount(instance_proto.name(),
                                      instance_proto.node_count());
    metadata->SetPendingOperation(*response);
    absl::Status save_status = metadata->Save();
    if (!save_status.ok()) {
      restore_runtime();
      metadata->UpdateInstance(
          previous_instance.name(), previous_instance.config(),
          previous_instance.display_name(),
          previous_instance.processing_units(),
          std::map<std::string, std::string>(
              previous_instance.labels().begin(),
              previous_instance.labels().end()),
          absl::FormatTime(absl::RFC3339_full, previous_update_time,
                           absl::UTCTimeZone()));
      metadata->UpdateInstanceNodeCount(previous_instance.name(),
                                        previous_instance.node_count());
      metadata->RemovePendingOperation(response->name());
      ctx->env()->operation_manager()->DeleteOperation(response->name())
          .IgnoreError();
      return save_status;
    }
    absl::Status operation_status =
        ctx->env()->backup_catalog()->SaveOperation(*response);
    if (!operation_status.ok()) {
      restore_runtime();
      metadata->UpdateInstance(
          previous_instance.name(), previous_instance.config(),
          previous_instance.display_name(),
          previous_instance.processing_units(),
          std::map<std::string, std::string>(
              previous_instance.labels().begin(),
              previous_instance.labels().end()),
          absl::FormatTime(absl::RFC3339_full, previous_update_time,
                           absl::UTCTimeZone()));
      metadata->UpdateInstanceNodeCount(previous_instance.name(),
                                        previous_instance.node_count());
      metadata->RemovePendingOperation(response->name());
      absl::Status rollback_status = metadata->Save();
      ctx->env()->operation_manager()->DeleteOperation(response->name())
          .IgnoreError();
      if (!rollback_status.ok()) {
        return absl::DataLossError(absl::StrCat(
            operation_status.message(),
            "; failed to roll back instance metadata: ",
            rollback_status.message()));
      }
      return operation_status;
    }
    metadata->RemovePendingOperation(response->name());
    absl::Status journal_status = metadata->Save();
    if (!journal_status.ok()) {
      ABSL_LOG(WARNING)
          << "Failed to clear promoted update-instance operation journal "
          << response->name() << ": " << journal_status;
    }
  }
  return absl::OkStatus();
}
REGISTER_GRPC_HANDLER(InstanceAdmin, UpdateInstance);

// Deletes an instance. Returns OK even if the instance is not found.
absl::Status DeleteInstance(RequestContext* ctx,
                            const instance_api::DeleteInstanceRequest* request,
                            protobuf_api::Empty* response) {
  absl::string_view project_id;
  absl::string_view instance_id;
  GOOGLESQL_RETURN_IF_ERROR(
      ParseInstanceUri(request->name(), &project_id, &instance_id));
  if (MakeInstanceUri(project_id, instance_id) != request->name()) {
    return absl::InvalidArgumentError("Instance name must be canonical");
  }
  absl::MutexLock admin_transaction_lock(
      &ctx->env()->admin_transaction_mutex());

  GOOGLESQL_ASSIGN_OR_RETURN(
      std::vector<std::shared_ptr<Database>> databases,
      ctx->env()->database_manager()->ListDatabases(request->name()));
  if (!ctx->env()->backup_catalog()->ListBackups(request->name()).empty()) {
    return absl::FailedPreconditionError("Instance still has backups");
  }
  for (const auto& database : databases) {
    if (!ctx->env()
             ->backup_catalog()
             ->ListBackupSchedules(database->database_uri())
             .empty()) {
      return absl::FailedPreconditionError(
          "An instance database still has backup schedules");
    }
  }

  std::vector<std::string> database_names;
  database_names.reserve(databases.size());
  for (const auto& database : databases) {
    database_names.push_back(database->database_uri());
  }

  // Commit durable deletion before mutating the live managers. Per-root
  // markers make a crash after metadata publication recoverable at startup.
  MetadataStore* metadata = ctx->env()->metadata_store();
  std::vector<std::string> marked_databases;
  auto cancel_markers = [&]() {
    absl::Status result = absl::OkStatus();
    for (const std::string& database_name : marked_databases) {
      absl::Status status = DatabaseManager::CancelDatabaseDeletion(
          config::data_dir(), database_name);
      if (!status.ok() && result.ok()) result = status;
    }
    return result;
  };
  if (metadata != nullptr) {
    for (const std::string& database_name : database_names) {
      absl::Status marker_status = DatabaseManager::MarkDatabaseForDeletion(
          config::data_dir(), database_name);
      if (!marker_status.ok()) {
        absl::Status cancel_status = cancel_markers();
        if (!cancel_status.ok()) {
          return absl::DataLossError(absl::StrCat(
              marker_status.message(), "; failed to cancel deletion markers: ",
              cancel_status.message()));
        }
        return marker_status;
      }
      marked_databases.push_back(database_name);
    }
    metadata->RemoveInstance(request->name());
    absl::Status save_status = metadata->Save();
    if (!save_status.ok()) {
      absl::Status reload_status = metadata->Load();
      absl::Status cancel_status = cancel_markers();
      if (!reload_status.ok() || !cancel_status.ok()) {
        return absl::DataLossError(absl::StrCat(
            save_status.message(),
            reload_status.ok()
                ? ""
                : absl::StrCat("; failed to restore metadata snapshot: ",
                               reload_status.message()),
            cancel_status.ok()
                ? ""
                : absl::StrCat("; failed to cancel deletion markers: ",
                               cancel_status.message())));
      }
      return save_status;
    }
  }

  for (const auto& database : databases) {
    GOOGLESQL_ASSIGN_OR_RETURN(
        std::vector<std::shared_ptr<Session>> sessions,
        ctx->env()->session_manager()->ListSessions(
            database->database_uri(), /*include_multiplex_sessions=*/true));
    for (const auto& session : sessions) {
      GOOGLESQL_RETURN_IF_ERROR(ctx->env()->session_manager()->DeleteSession(
          session->session_uri(), /*delete_multiplex_sessions=*/true));
    }
    GOOGLESQL_RETURN_IF_ERROR(ctx->env()->database_manager()->DeleteDatabase(
        database->database_uri()));
  }

  GOOGLESQL_ASSIGN_OR_RETURN(
      std::vector<std::shared_ptr<InstancePartition>> partitions,
      ctx->env()->instance_partition_manager()->ListInstancePartitions(
          request->name()));
  for (const auto& partition : partitions) {
    ctx->env()->instance_partition_manager()->DeleteInstancePartition(
        partition->partition_uri());
  }
  ctx->env()->instance_manager()->DeleteInstance(request->name());
  ctx->env()->RemoveIamPolicies(request->name());

  databases.clear();
  for (const std::string& database_name : database_names) {
    GOOGLESQL_RETURN_IF_ERROR(
        backend::Database::DeletePersistentStorageDirectory(
            config::data_dir(), database_name));
  }
  return absl::OkStatus();
}
REGISTER_GRPC_HANDLER(InstanceAdmin, DeleteInstance);

}  // namespace frontend
}  // namespace emulator
}  // namespace spanner
}  // namespace google

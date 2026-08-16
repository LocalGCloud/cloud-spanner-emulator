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

#include <cstdint>
#include <limits>
#include <memory>
#include <string>
#include <vector>

#include "absl/algorithm/container.h"
#include "absl/container/flat_hash_map.h"
#include "absl/log/absl_log.h"
#include "absl/status/status.h"
#include "absl/status/statusor.h"
#include "absl/strings/match.h"
#include "absl/strings/str_cat.h"
#include "absl/strings/string_view.h"
#include "absl/synchronization/mutex.h"
#include "absl/time/time.h"
#include "backend/database/database.h"
#include "common/errors.h"
#include "frontend/collections/operation_manager.h"
#include "frontend/common/uris.h"
#include "frontend/converters/time.h"
#include "frontend/entities/instance_partition.h"
#include "frontend/persistence/backup_catalog.h"
#include "frontend/persistence/metadata_store.h"
#include "frontend/server/environment.h"
#include "frontend/server/handler.h"
#include "frontend/server/request_context.h"
#include "google/longrunning/operations.pb.h"
#include "google/protobuf/empty.pb.h"
#include "google/spanner/admin/instance/v1/spanner_instance_admin.pb.h"
#include "googlesql/base/status_macros.h"

namespace instance_api = ::google::spanner::admin::instance::v1;
namespace operations_api = ::google::longrunning;
namespace protobuf_api = ::google::protobuf;

namespace google {
namespace spanner {
namespace emulator {
namespace frontend {

namespace {

absl::flat_hash_map<std::string, std::vector<std::string>>
GetReferencingDatabases(ServerEnv* env, const std::string& instance_uri) {
  absl::flat_hash_map<std::string, std::vector<std::string>> partition_to_dbs;
  auto databases_or = env->database_manager()->ListDatabases(instance_uri);
  if (!databases_or.ok()) {
    return partition_to_dbs;
  }
  for (const auto& db : *databases_or) {
    if (db->backend() == nullptr) {
      continue;
    }
    const auto* schema = db->backend()->GetLatestSchema();
    if (schema == nullptr) {
      continue;
    }
    for (const auto* placement : schema->placements()) {
      if (placement->InstancePartition().has_value()) {
        std::string partition_val = placement->InstancePartition().value();
        std::string full_partition_uri;
        if (absl::StartsWith(partition_val, "projects/")) {
          full_partition_uri = partition_val;
        } else {
          full_partition_uri =
              MakeInstancePartitionUri(instance_uri, partition_val);
        }
        auto& dbs = partition_to_dbs[full_partition_uri];
        if (absl::c_find(dbs, db->database_uri()) == dbs.end()) {
          dbs.push_back(db->database_uri());
        }
      }
    }
  }
  return partition_to_dbs;
}

absl::StatusOr<std::string> CanonicalInstanceConfig(
    ServerEnv* env, absl::string_view project_id,
    absl::string_view config_reference) {
  const std::string built_in =
      MakeInstanceConfigUri(project_id, "emulator-config");
  if (config_reference == "emulator-config") return built_in;
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
  if (config_id != "emulator-config") {
    GOOGLESQL_RETURN_IF_ERROR(
        env->GetCustomInstanceConfig(canonical).status());
  }
  return canonical;
}

void PopulateReferencingDatabases(
    const absl::flat_hash_map<std::string, std::vector<std::string>>& dbs_map,
    instance_api::InstancePartition* proto) {
  auto it = dbs_map.find(proto->name());
  if (it != dbs_map.end()) {
    for (const auto& db_uri : it->second) {
      proto->add_referencing_databases(db_uri);
    }
  }
}

}  // namespace

// Lists all instance partitions in an instance.
absl::Status ListInstancePartitions(
    RequestContext* ctx,
    const instance_api::ListInstancePartitionsRequest* request,
    instance_api::ListInstancePartitionsResponse* response) {
  absl::string_view project_id, instance_id;
  GOOGLESQL_RETURN_IF_ERROR(
      ParseInstanceUri(request->parent(), &project_id, &instance_id));
  if (MakeInstanceUri(project_id, instance_id) != request->parent()) {
    return absl::InvalidArgumentError("Instance name must be canonical");
  }
  GOOGLESQL_RETURN_IF_ERROR(
      ctx->env()->instance_manager()->GetInstance(request->parent()).status());

  if (!request->page_token().empty()) {
    absl::string_view p_id, i_id, part_id;
    GOOGLESQL_RETURN_IF_ERROR(ParseInstancePartitionUri(
        request->page_token(), &p_id, &i_id, &part_id));
    if (MakeInstancePartitionUri(MakeInstanceUri(p_id, i_id), part_id) !=
            request->page_token() ||
        p_id != project_id || i_id != instance_id) {
      return absl::InvalidArgumentError(
          "Page token must be a canonical partition in the parent instance");
    }
  }

  GOOGLESQL_ASSIGN_OR_RETURN(
      std::vector<std::shared_ptr<InstancePartition>> partitions,
      ctx->env()->instance_partition_manager()->ListInstancePartitions(
          request->parent()));

  int32_t page_size = request->page_size();
  static const int32_t kMaxPageSize = 1000;
  if (page_size <= 0 || page_size > kMaxPageSize) {
    page_size = kMaxPageSize;
  }

  auto dbs_map = GetReferencingDatabases(ctx->env(), request->parent());

  for (const auto& partition : partitions) {
    if (response->instance_partitions_size() >= page_size) {
      response->set_next_page_token(partition->partition_uri());
      break;
    }
    if (partition->partition_uri() >= request->page_token()) {
      auto* proto = response->add_instance_partitions();
      partition->ToProto(proto);
      PopulateReferencingDatabases(dbs_map, proto);
    }
  }
  return absl::OkStatus();
}
REGISTER_GRPC_HANDLER(InstanceAdmin, ListInstancePartitions);

// Gets information about an instance partition.
absl::Status GetInstancePartition(
    RequestContext* ctx,
    const instance_api::GetInstancePartitionRequest* request,
    instance_api::InstancePartition* response) {
  absl::string_view project_id, instance_id, partition_id;
  GOOGLESQL_RETURN_IF_ERROR(ParseInstancePartitionUri(
      request->name(), &project_id, &instance_id, &partition_id));
  if (MakeInstancePartitionUri(MakeInstanceUri(project_id, instance_id),
                               partition_id) != request->name()) {
    return absl::InvalidArgumentError(
        "Instance partition name must be canonical");
  }
  std::string instance_uri = MakeInstanceUri(project_id, instance_id);

  GOOGLESQL_ASSIGN_OR_RETURN(
      std::shared_ptr<InstancePartition> partition,
      ctx->env()->instance_partition_manager()->GetInstancePartition(
          request->name()));
  partition->ToProto(response);
  auto dbs_map = GetReferencingDatabases(ctx->env(), instance_uri);
  PopulateReferencingDatabases(dbs_map, response);
  return absl::OkStatus();
}
REGISTER_GRPC_HANDLER(InstanceAdmin, GetInstancePartition);

// Creates an instance partition.
absl::Status CreateInstancePartition(
    RequestContext* ctx,
    const instance_api::CreateInstancePartitionRequest* request,
    operations_api::Operation* response) {
  absl::MutexLock admin_transaction_lock(
      &ctx->env()->admin_transaction_mutex());
  absl::string_view project_id, instance_id;
  GOOGLESQL_RETURN_IF_ERROR(
      ParseInstanceUri(request->parent(), &project_id, &instance_id));
  if (MakeInstanceUri(project_id, instance_id) != request->parent()) {
    return absl::InvalidArgumentError("Instance name must be canonical");
  }
  GOOGLESQL_RETURN_IF_ERROR(
      ctx->env()->instance_manager()->GetInstance(request->parent()).status());
  const std::string partition_uri = MakeInstancePartitionUri(
      request->parent(), request->instance_partition_id());
  if (!request->instance_partition().name().empty() &&
      request->instance_partition().name() != partition_uri) {
    return error::InstancePartitionNameMismatch(
        request->instance_partition().name());
  }
  GOOGLESQL_RETURN_IF_ERROR(
      ValidateInstancePartitionId(request->instance_partition_id()));
  instance_api::InstancePartition partition_request =
      request->instance_partition();
  GOOGLESQL_ASSIGN_OR_RETURN(
      const std::string canonical_config,
      CanonicalInstanceConfig(ctx->env(), project_id,
                              partition_request.config()));
  partition_request.set_config(canonical_config);
  const absl::Time now = ctx->env()->clock()->Now();
  GOOGLESQL_ASSIGN_OR_RETURN(const auto now_proto, TimestampToProto(now));
  GOOGLESQL_ASSIGN_OR_RETURN(
      std::shared_ptr<Operation> operation,
      ctx->env()->operation_manager()->CreateOperation(
          partition_uri, OperationManager::kAutoGeneratedId));
  operation->ToProto(response);
  auto partition_or =
      ctx->env()->instance_partition_manager()->CreateInstancePartition(
          partition_uri, partition_request, now, now);
  if (!partition_or.ok()) {
    ctx->env()->operation_manager()->DeleteOperation(response->name())
        .IgnoreError();
    return partition_or.status();
  }
  std::shared_ptr<InstancePartition> partition = *partition_or;
  instance_api::InstancePartition partition_proto;
  partition->ToProto(&partition_proto);
  instance_api::CreateInstancePartitionMetadata operation_metadata;
  *operation_metadata.mutable_instance_partition() = partition_proto;
  *operation_metadata.mutable_start_time() = now_proto;
  *operation_metadata.mutable_end_time() = now_proto;
  operation->SetMetadata(operation_metadata);
  operation->SetResponse(partition_proto);
  operation->ToProto(response);

  if (auto* metadata = ctx->env()->metadata_store(); metadata != nullptr) {
    metadata->SetInstancePartition(partition_proto);
    metadata->SetPendingOperation(*response);
    absl::Status metadata_status = metadata->Save();
    if (!metadata_status.ok()) {
      metadata->RemoveInstancePartition(partition_uri);
      metadata->RemovePendingOperation(response->name());
      ctx->env()->instance_partition_manager()->DeleteInstancePartition(
          partition_uri);
      ctx->env()->operation_manager()->DeleteOperation(response->name())
          .IgnoreError();
      return metadata_status;
    }
    absl::Status operation_status =
        ctx->env()->backup_catalog()->SaveOperation(*response);
    if (!operation_status.ok()) {
      metadata->RemoveInstancePartition(partition_uri);
      metadata->RemovePendingOperation(response->name());
      absl::Status rollback_status = metadata->Save();
      ctx->env()->instance_partition_manager()->DeleteInstancePartition(
          partition_uri);
      ctx->env()->operation_manager()->DeleteOperation(response->name())
          .IgnoreError();
      if (!rollback_status.ok()) {
        return absl::DataLossError(absl::StrCat(
            operation_status.message(),
            "; failed to roll back instance partition metadata: ",
            rollback_status.message()));
      }
      return operation_status;
    }
    metadata->RemovePendingOperation(response->name());
    absl::Status journal_status = metadata->Save();
    if (!journal_status.ok()) {
      ABSL_LOG(WARNING)
          << "Failed to clear promoted instance-partition operation journal "
          << response->name() << ": " << journal_status;
    }
  }
  return absl::OkStatus();
}
REGISTER_GRPC_HANDLER(InstanceAdmin, CreateInstancePartition);

// Updates an instance partition.
absl::Status UpdateInstancePartition(
    RequestContext* ctx,
    const instance_api::UpdateInstancePartitionRequest* request,
    operations_api::Operation* response) {
  absl::MutexLock admin_transaction_lock(
      &ctx->env()->admin_transaction_mutex());
  if (!request->has_instance_partition() ||
      request->instance_partition().name().empty()) {
    return absl::InvalidArgumentError(
        "Instance partition name must be provided");
  }
  absl::string_view project_id, instance_id, partition_id;
  GOOGLESQL_RETURN_IF_ERROR(ParseInstancePartitionUri(
      request->instance_partition().name(), &project_id, &instance_id,
      &partition_id));
  if (MakeInstancePartitionUri(MakeInstanceUri(project_id, instance_id),
                               partition_id) !=
      request->instance_partition().name()) {
    return absl::InvalidArgumentError(
        "Instance partition name must be canonical");
  }
  GOOGLESQL_ASSIGN_OR_RETURN(
      std::shared_ptr<InstancePartition> partition,
      ctx->env()->instance_partition_manager()->GetInstancePartition(
          request->instance_partition().name()));
  if (request->field_mask().paths().empty()) {
    return absl::InvalidArgumentError(
        "Instance partition field_mask must be provided");
  }

  bool update_display_name = false;
  bool update_node_count = false;
  bool update_processing_units = false;
  for (const std::string& path : request->field_mask().paths()) {
    if (path == "display_name" || path == "displayName") {
      update_display_name = true;
    } else if (path == "node_count" || path == "nodeCount") {
      update_node_count = true;
    } else if (path == "processing_units" || path == "processingUnits") {
      update_processing_units = true;
    } else {
      return absl::InvalidArgumentError(
          absl::StrCat("Unsupported instance partition update field: ", path));
    }
  }
  if (update_node_count && update_processing_units) {
    return absl::InvalidArgumentError(
        "node_count and processing_units cannot both be updated");
  }
  if (update_node_count &&
      (request->instance_partition().node_count() <= 0 ||
       request->instance_partition().node_count() >
           std::numeric_limits<int32_t>::max() / 1000)) {
    return absl::InvalidArgumentError("node_count is outside the valid range");
  }
  const int32_t processing_units =
      request->instance_partition().processing_units();
  if (update_processing_units &&
      (processing_units <= 0 ||
       (processing_units < 1000 && processing_units % 100 != 0) ||
       (processing_units > 1000 && processing_units % 1000 != 0))) {
    return absl::InvalidArgumentError(
        "processing_units must be positive and use a supported increment");
  }

  instance_api::InstancePartition previous;
  partition->ToProto(&previous);
  GOOGLESQL_ASSIGN_OR_RETURN(
      const absl::Time previous_update_time,
      TimestampFromProto(previous.update_time()));
  GOOGLESQL_ASSIGN_OR_RETURN(
      std::shared_ptr<Operation> operation,
      ctx->env()->operation_manager()->CreateOperation(
          previous.name(), OperationManager::kAutoGeneratedId));
  const auto restore_runtime = [&] {
    partition->UpdateDisplayName(previous.display_name(),
                                 previous_update_time);
    if (previous.node_count() > 0) {
      partition->UpdateNodeCount(previous.node_count(), previous_update_time);
    } else {
      partition->UpdateProcessingUnits(previous.processing_units(),
                                       previous_update_time);
    }
  };

  const absl::Time update_time = ctx->env()->clock()->Now();
  if (update_display_name) {
    partition->UpdateDisplayName(
        request->instance_partition().display_name(), update_time);
  }
  if (update_node_count) {
    partition->UpdateNodeCount(request->instance_partition().node_count(),
                               update_time);
  }
  if (update_processing_units) {
    partition->UpdateProcessingUnits(processing_units, update_time);
  }
  instance_api::InstancePartition partition_proto;
  partition->ToProto(&partition_proto);
  operation->SetResponse(partition_proto);
  operation->ToProto(response);

  if (auto* metadata = ctx->env()->metadata_store(); metadata != nullptr) {
    metadata->SetInstancePartition(partition_proto);
    metadata->SetPendingOperation(*response);
    absl::Status metadata_status = metadata->Save();
    if (!metadata_status.ok()) {
      restore_runtime();
      metadata->SetInstancePartition(previous);
      metadata->RemovePendingOperation(response->name());
      ctx->env()->operation_manager()->DeleteOperation(response->name())
          .IgnoreError();
      return metadata_status;
    }
    absl::Status operation_status =
        ctx->env()->backup_catalog()->SaveOperation(*response);
    if (!operation_status.ok()) {
      restore_runtime();
      metadata->SetInstancePartition(previous);
      metadata->RemovePendingOperation(response->name());
      absl::Status rollback_status = metadata->Save();
      ctx->env()->operation_manager()->DeleteOperation(response->name())
          .IgnoreError();
      if (!rollback_status.ok()) {
        return absl::DataLossError(absl::StrCat(
            operation_status.message(),
            "; failed to roll back instance partition metadata: ",
            rollback_status.message()));
      }
      return operation_status;
    }
    metadata->RemovePendingOperation(response->name());
    absl::Status journal_status = metadata->Save();
    if (!journal_status.ok()) {
      ABSL_LOG(WARNING)
          << "Failed to clear promoted instance-partition operation journal "
          << response->name() << ": " << journal_status;
    }
  }
  return absl::OkStatus();
}
REGISTER_GRPC_HANDLER(InstanceAdmin, UpdateInstancePartition);

// Deletes an instance partition.
absl::Status DeleteInstancePartition(
    RequestContext* ctx,
    const instance_api::DeleteInstancePartitionRequest* request,
    protobuf_api::Empty* response) {
  absl::MutexLock admin_transaction_lock(
      &ctx->env()->admin_transaction_mutex());
  absl::string_view project_id, instance_id, partition_id;
  GOOGLESQL_RETURN_IF_ERROR(ParseInstancePartitionUri(
      request->name(), &project_id, &instance_id, &partition_id));
  if (MakeInstancePartitionUri(MakeInstanceUri(project_id, instance_id),
                               partition_id) != request->name()) {
    return absl::InvalidArgumentError(
        "Instance partition name must be canonical");
  }
  std::string instance_uri = MakeInstanceUri(project_id, instance_id);

  GOOGLESQL_ASSIGN_OR_RETURN(
      std::shared_ptr<InstancePartition> partition,
      ctx->env()->instance_partition_manager()->GetInstancePartition(
          request->name()));

  auto dbs_map = GetReferencingDatabases(ctx->env(), instance_uri);
  auto it = dbs_map.find(request->name());
  if (it != dbs_map.end() && !it->second.empty()) {
    return error::InstancePartitionReferencedByDatabase(request->name());
  }

  if (auto* metadata = ctx->env()->metadata_store(); metadata != nullptr) {
    instance_api::InstancePartition previous;
    partition->ToProto(&previous);
    const auto previous_policy = metadata->GetIamPolicy(request->name());
    metadata->RemoveInstancePartition(request->name());
    absl::Status status = metadata->Save();
    if (!status.ok()) {
      metadata->SetInstancePartition(previous);
      if (previous_policy.has_value()) {
        metadata->SetIamPolicy(request->name(), *previous_policy);
      }
      return status;
    }
  }
  ctx->env()->instance_partition_manager()->DeleteInstancePartition(
      request->name());
  ctx->env()->RemoveIamPolicies(request->name());
  return absl::OkStatus();
}
REGISTER_GRPC_HANDLER(InstanceAdmin, DeleteInstancePartition);

// Lists operations on instance partitions.
absl::Status ListInstancePartitionOperations(
    RequestContext* ctx,
    const instance_api::ListInstancePartitionOperationsRequest* request,
    instance_api::ListInstancePartitionOperationsResponse* response) {
  absl::string_view project_id, instance_id;
  GOOGLESQL_RETURN_IF_ERROR(
      ParseInstanceUri(request->parent(), &project_id, &instance_id));
  if (MakeInstanceUri(project_id, instance_id) != request->parent()) {
    return absl::InvalidArgumentError("Instance name must be canonical");
  }
  GOOGLESQL_RETURN_IF_ERROR(
      ctx->env()->instance_manager()->GetInstance(request->parent()).status());

  std::string prefix = absl::StrCat(request->parent(), "/instancePartitions/");
  GOOGLESQL_ASSIGN_OR_RETURN(
      std::vector<std::shared_ptr<Operation>> operations,
      ctx->env()->operation_manager()->ListOperations(prefix));
  for (const auto& op : operations) {
    op->ToProto(response->add_operations());
  }
  return absl::OkStatus();
}
REGISTER_GRPC_HANDLER(InstanceAdmin, ListInstancePartitionOperations);

}  // namespace frontend
}  // namespace emulator
}  // namespace spanner
}  // namespace google

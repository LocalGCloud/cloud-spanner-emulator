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

#include <cstdint>
#include <memory>
#include <string>
#include <vector>

#include "absl/status/status.h"
#include "absl/strings/str_cat.h"
#include "absl/synchronization/mutex.h"
#include "frontend/collections/operation_manager.h"
#include "frontend/common/uris.h"
#include "frontend/server/handler.h"
#include "google/longrunning/operations.pb.h"
#include "google/spanner/admin/database/v1/spanner_database_admin.pb.h"
#include "googlesql/base/status_macros.h"

namespace database_api = ::google::spanner::admin::database::v1;
namespace operations_api = ::google::longrunning;

namespace google {
namespace spanner {
namespace emulator {
namespace frontend {
namespace {

constexpr int32_t kMaximumPageSize = 1000;

absl::Status ValidateInstanceParent(RequestContext* ctx,
                                    const std::string& parent) {
  absl::string_view project_id;
  absl::string_view instance_id;
  GOOGLESQL_RETURN_IF_ERROR(
      ParseInstanceUri(parent, &project_id, &instance_id));
  return ctx->env()->instance_manager()->GetInstance(parent).status();
}

int32_t EffectivePageSize(int32_t requested) {
  return requested <= 0 || requested > kMaximumPageSize ? kMaximumPageSize
                                                        : requested;
}

}  // namespace

// Split points are an optimization hint. Local LevelDB storage does not need
// physical splits, so a valid request succeeds without mutating row data.
absl::Status AddSplitPoints(RequestContext* ctx,
                            const database_api::AddSplitPointsRequest* request,
                            database_api::AddSplitPointsResponse* response) {
  if (request->database().empty()) {
    return absl::InvalidArgumentError("Database must be provided");
  }
  absl::string_view project_id;
  absl::string_view instance_id;
  absl::string_view database_id;
  GOOGLESQL_RETURN_IF_ERROR(ParseDatabaseUri(request->database(), &project_id,
                                             &instance_id, &database_id));
  return ctx->env()
      ->database_manager()
      ->GetDatabase(request->database())
      .status();
}
REGISTER_GRPC_HANDLER(DatabaseAdmin, AddSplitPoints);

// This RPC is used by production's graph backfill workers. The emulator runs
// schema work synchronously, but accepts terminal updates for an existing LRO.
absl::Status InternalUpdateGraphOperation(
    RequestContext* ctx,
    const database_api::InternalUpdateGraphOperationRequest* request,
    database_api::InternalUpdateGraphOperationResponse* response) {
  absl::MutexLock admin_transaction_lock(
      &ctx->env()->admin_transaction_mutex());
  if (request->database().empty() || request->operation_id().empty()) {
    return absl::InvalidArgumentError(
        "Database and operation_id must be provided");
  }
  absl::string_view project_id;
  absl::string_view instance_id;
  absl::string_view database_id;
  GOOGLESQL_RETURN_IF_ERROR(ParseDatabaseUri(request->database(), &project_id,
                                             &instance_id, &database_id));
  GOOGLESQL_ASSIGN_OR_RETURN(
      std::shared_ptr<Operation> operation,
      ctx->env()->operation_manager()->GetOperation(
          MakeOperationUri(request->database(), request->operation_id())));
  if (request->has_status() && request->status().code() != 0) {
    operation->SetError(
        absl::Status(static_cast<absl::StatusCode>(request->status().code()),
                     request->status().message()));
  }
  return absl::OkStatus();
}
REGISTER_GRPC_HANDLER(DatabaseAdmin, InternalUpdateGraphOperation);

absl::Status ListBackupOperations(
    RequestContext* ctx,
    const database_api::ListBackupOperationsRequest* request,
    database_api::ListBackupOperationsResponse* response) {
  GOOGLESQL_RETURN_IF_ERROR(ValidateInstanceParent(ctx, request->parent()));
  GOOGLESQL_ASSIGN_OR_RETURN(
      std::vector<std::shared_ptr<Operation>> operations,
      ctx->env()->operation_manager()->ListOperations(
          absl::StrCat(request->parent(), "/")));
  const int32_t page_size = EffectivePageSize(request->page_size());
  for (const auto& operation : operations) {
    operations_api::Operation proto;
    operation->ToProto(&proto);
    absl::string_view operation_id;
    OperationResourceType resource_type;
    GOOGLESQL_RETURN_IF_ERROR(ParseOperationUri(
        proto.name(), /*resource_uri=*/nullptr, &operation_id,
        &resource_type));
    if (resource_type != OperationResourceType::kBackup) continue;
    if (!request->page_token().empty() &&
        proto.name() < request->page_token()) {
      continue;
    }
    if (response->operations_size() >= page_size) {
      response->set_next_page_token(proto.name());
      break;
    }
    *response->add_operations() = std::move(proto);
  }
  return absl::OkStatus();
}
REGISTER_GRPC_HANDLER(DatabaseAdmin, ListBackupOperations);

absl::Status ListDatabaseOperations(
    RequestContext* ctx,
    const database_api::ListDatabaseOperationsRequest* request,
    database_api::ListDatabaseOperationsResponse* response) {
  GOOGLESQL_RETURN_IF_ERROR(ValidateInstanceParent(ctx, request->parent()));
  GOOGLESQL_ASSIGN_OR_RETURN(
      std::vector<std::shared_ptr<Operation>> operations,
      ctx->env()->operation_manager()->ListOperations(
          absl::StrCat(request->parent(), "/")));
  const int32_t page_size = EffectivePageSize(request->page_size());
  for (const auto& operation : operations) {
    operations_api::Operation proto;
    operation->ToProto(&proto);
    absl::string_view operation_id;
    OperationResourceType resource_type;
    GOOGLESQL_RETURN_IF_ERROR(ParseOperationUri(
        proto.name(), /*resource_uri=*/nullptr, &operation_id,
        &resource_type));
    if (resource_type != OperationResourceType::kDatabase) continue;
    if (!request->page_token().empty() &&
        proto.name() < request->page_token()) {
      continue;
    }
    if (response->operations_size() >= page_size) {
      response->set_next_page_token(proto.name());
      break;
    }
    *response->add_operations() = std::move(proto);
  }
  return absl::OkStatus();
}
REGISTER_GRPC_HANDLER(DatabaseAdmin, ListDatabaseOperations);

// Fine-grained database roles are not synthesized. Returning an empty page is
// the same observable result as a production database with no roles.
absl::Status ListDatabaseRoles(
    RequestContext* ctx, const database_api::ListDatabaseRolesRequest* request,
    database_api::ListDatabaseRolesResponse* response) {
  if (request->parent().empty()) {
    return absl::InvalidArgumentError("Database parent must be provided");
  }
  absl::string_view project_id;
  absl::string_view instance_id;
  absl::string_view database_id;
  GOOGLESQL_RETURN_IF_ERROR(ParseDatabaseUri(request->parent(), &project_id,
                                             &instance_id, &database_id));
  GOOGLESQL_RETURN_IF_ERROR(
      ctx->env()->database_manager()->GetDatabase(request->parent()).status());
  return absl::OkStatus();
}
REGISTER_GRPC_HANDLER(DatabaseAdmin, ListDatabaseRoles);

}  // namespace frontend
}  // namespace emulator
}  // namespace spanner
}  // namespace google

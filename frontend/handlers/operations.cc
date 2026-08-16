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

#include <memory>
#include <optional>
#include <string>

#include "absl/status/status.h"
#include "absl/strings/str_cat.h"
#include "absl/strings/string_view.h"
#include "absl/synchronization/mutex.h"
#include "frontend/common/uris.h"
#include "frontend/persistence/backup_catalog.h"
#include "frontend/persistence/metadata_store.h"
#include "frontend/server/handler.h"
#include "google/longrunning/operations.pb.h"
#include "google/protobuf/empty.pb.h"
#include "googlesql/base/status_macros.h"

namespace operations_api = ::google::longrunning;
namespace protobuf_api = ::google::protobuf;

namespace google {
namespace spanner {
namespace emulator {
namespace frontend {

// Lists operations that match the specified filter in the request.
absl::Status ListOperations(
    RequestContext* ctx, const operations_api::ListOperationsRequest* request,
    operations_api::ListOperationsResponse* response) {
  // Verify the operations URI is valid.
  absl::string_view operation_id;
  GOOGLESQL_RETURN_IF_ERROR(
      ParseOperationUri(absl::StrCat(request->name(), "/"),
                        /*resource_uri=*/nullptr, &operation_id));
  GOOGLESQL_ASSIGN_OR_RETURN(
      std::vector<std::shared_ptr<Operation>> operations,
      ctx->env()->operation_manager()->ListOperations(request->name()));
  for (const auto& op : operations) {
    op->ToProto(response->add_operations());
  }
  return absl::OkStatus();
}
REGISTER_GRPC_HANDLER(Operations, ListOperations);

// Gets the latest state of a long-running operation.
absl::Status GetOperation(RequestContext* ctx,
                          const operations_api::GetOperationRequest* request,
                          operations_api::Operation* response) {
  absl::string_view operation_id;
  GOOGLESQL_RETURN_IF_ERROR(ParseOperationUri(
      request->name(), /*resource_uri=*/nullptr, &operation_id));
  GOOGLESQL_ASSIGN_OR_RETURN(
      std::shared_ptr<Operation> operation,
      ctx->env()->operation_manager()->GetOperation(request->name()));
  operation->ToProto(response);
  return absl::OkStatus();
}
REGISTER_GRPC_HANDLER(Operations, GetOperation);

// Deletes a long-running operation.
absl::Status DeleteOperation(
    RequestContext* ctx, const operations_api::DeleteOperationRequest* request,
    protobuf_api::Empty* response) {
  absl::MutexLock admin_transaction_lock(
      &ctx->env()->admin_transaction_mutex());
  absl::string_view operation_id;
  GOOGLESQL_RETURN_IF_ERROR(ParseOperationUri(
      request->name(), /*resource_uri=*/nullptr, &operation_id));
  GOOGLESQL_RETURN_IF_ERROR(
      ctx->env()->operation_manager()->GetOperation(request->name()).status());

  MetadataStore* metadata = ctx->env()->metadata_store();
  std::optional<operations_api::Operation> pending_operation;
  if (metadata != nullptr) {
    // A prior journal-clear Save may have failed after the operation was
    // promoted. Reload so deletion observes the durable record, not only the
    // optimistically cleared in-memory map.
    GOOGLESQL_RETURN_IF_ERROR(metadata->Load());
    const auto pending_operations = metadata->AllPendingOperations();
    auto pending = pending_operations.find(request->name());
    if (pending != pending_operations.end()) {
      pending_operation = pending->second;
      metadata->RemovePendingOperation(request->name());
      GOOGLESQL_RETURN_IF_ERROR(metadata->Save());
    }
  }

  absl::Status catalog_status =
      ctx->env()->backup_catalog()->DeleteOperation(request->name());
  if (!catalog_status.ok()) {
    if (metadata != nullptr && pending_operation.has_value()) {
      metadata->SetPendingOperation(*pending_operation);
      absl::Status rollback_status = metadata->Save();
      if (!rollback_status.ok()) {
        return absl::DataLossError(absl::StrCat(
            catalog_status.message(),
            "; failed to restore pending operation journal: ",
            rollback_status.message()));
      }
    }
    return catalog_status;
  }
  return ctx->env()->operation_manager()->DeleteOperation(request->name());
}
REGISTER_GRPC_HANDLER(Operations, DeleteOperation);

// Cancels a long-running operation.
absl::Status CancelOperation(
    RequestContext* ctx, const operations_api::CancelOperationRequest* request,
    protobuf_api::Empty* response) {
  absl::string_view operation_id;
  GOOGLESQL_RETURN_IF_ERROR(ParseOperationUri(
      request->name(), /*resource_uri=*/nullptr, &operation_id));
  // Emulator operations complete synchronously. Cancellation is accepted for
  // an existing operation and leaves an already-terminal result unchanged.
  GOOGLESQL_RETURN_IF_ERROR(
      ctx->env()->operation_manager()->GetOperation(request->name()).status());
  return absl::OkStatus();
}
REGISTER_GRPC_HANDLER(Operations, CancelOperation);

// Waits for the specified long-running operation until it is done.
absl::Status WaitOperation(RequestContext* ctx,
                           const operations_api::WaitOperationRequest* request,
                           operations_api::Operation* response) {
  absl::string_view operation_id;
  GOOGLESQL_RETURN_IF_ERROR(ParseOperationUri(
      request->name(), /*resource_uri=*/nullptr, &operation_id));
  GOOGLESQL_ASSIGN_OR_RETURN(
      std::shared_ptr<Operation> operation,
      ctx->env()->operation_manager()->GetOperation(request->name()));
  operation->ToProto(response);
  return absl::OkStatus();
}
REGISTER_GRPC_HANDLER(Operations, WaitOperation);

}  // namespace frontend
}  // namespace emulator
}  // namespace spanner
}  // namespace google

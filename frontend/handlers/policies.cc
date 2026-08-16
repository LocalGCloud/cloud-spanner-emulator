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

#include <string>

#include "absl/status/status.h"
#include "absl/strings/str_cat.h"
#include "absl/synchronization/mutex.h"
#include "absl/time/time.h"
#include "frontend/persistence/metadata_store.h"
#include "frontend/server/handler.h"
#include "google/iam/v1/iam_policy.pb.h"
#include "google/iam/v1/policy.pb.h"
#include "googlesql/base/status_macros.h"

namespace iam_api = ::google::iam::v1;

namespace google {
namespace spanner {
namespace emulator {
namespace frontend {
// Sets the access control policy on a resource.
absl::Status SetIamPolicy(RequestContext* ctx,
                          const iam_api::SetIamPolicyRequest* request,
                          iam_api::Policy* response) {
  absl::MutexLock transaction_lock(&ctx->env()->admin_transaction_mutex());
  GOOGLESQL_RETURN_IF_ERROR(
      ctx->env()->ValidateIamResource(request->resource()));

  *response = request->policy();
  if (response->etag().empty()) {
    response->set_etag(absl::StrCat(
        "localcloud-", absl::ToUnixNanos(ctx->env()->clock()->Now())));
  }
  const auto previous = ctx->env()->GetIamPolicy(request->resource());
  ctx->env()->SetIamPolicy(request->resource(), *response);

  if (auto* metadata = ctx->env()->metadata_store(); metadata != nullptr) {
    const auto previous_persisted =
        metadata->GetIamPolicy(request->resource());
    metadata->SetIamPolicy(request->resource(), *response);
    absl::Status status = metadata->Save();
    if (!status.ok()) {
      if (previous.has_value()) {
        ctx->env()->SetIamPolicy(request->resource(), *previous);
      } else {
        ctx->env()->RemoveIamPolicy(request->resource());
      }
      if (previous_persisted.has_value()) {
        metadata->SetIamPolicy(request->resource(), *previous_persisted);
      } else {
        metadata->RemoveIamPolicy(request->resource());
      }
      return status;
    }
  }
  return absl::OkStatus();
}
REGISTER_GRPC_HANDLER(InstanceAdmin, SetIamPolicy);
REGISTER_GRPC_HANDLER(DatabaseAdmin, SetIamPolicy);

// Gets the access control policy for a resource.
absl::Status GetIamPolicy(RequestContext* ctx,
                          const iam_api::GetIamPolicyRequest* request,
                          iam_api::Policy* response) {
  absl::MutexLock transaction_lock(&ctx->env()->admin_transaction_mutex());
  GOOGLESQL_RETURN_IF_ERROR(
      ctx->env()->ValidateIamResource(request->resource()));

  if (auto policy = ctx->env()->GetIamPolicy(request->resource());
      policy.has_value()) {
    *response = *policy;
    return absl::OkStatus();
  }



  response->Clear();
  return absl::OkStatus();
}
REGISTER_GRPC_HANDLER(InstanceAdmin, GetIamPolicy);
REGISTER_GRPC_HANDLER(DatabaseAdmin, GetIamPolicy);

// Returns permissions that the caller has on the specified resource.
absl::Status TestIamPermissions(
    RequestContext* ctx, const iam_api::TestIamPermissionsRequest* request,
    iam_api::TestIamPermissionsResponse* response) {
  absl::MutexLock transaction_lock(&ctx->env()->admin_transaction_mutex());
  GOOGLESQL_RETURN_IF_ERROR(
      ctx->env()->ValidateIamResource(request->resource()));
  response->clear_permissions();
  for (const std::string& permission : request->permissions()) {
    response->add_permissions(permission);
  }
  return absl::OkStatus();
}
REGISTER_GRPC_HANDLER(InstanceAdmin, TestIamPermissions);
REGISTER_GRPC_HANDLER(DatabaseAdmin, TestIamPermissions);

}  // namespace frontend
}  // namespace emulator
}  // namespace spanner
}  // namespace google

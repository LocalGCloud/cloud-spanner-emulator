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

#include "google/spanner/v1/spanner.pb.h"
#include "frontend/collections/database_manager.h"
#include "frontend/common/uris.h"
#include "frontend/server/environment.h"
#include "frontend/server/handler.h"
#include "googlesql/base/status_macros.h"

namespace google {
namespace spanner {
namespace emulator {
namespace frontend {

absl::Status FetchCacheUpdate(
    RequestContext* ctx,
    const spanner_api::FetchCacheUpdateRequest* request,
    ServerStream<spanner_api::CacheUpdate>* /*stream*/) {
  absl::string_view project_id;
  absl::string_view instance_id;
  absl::string_view database_id;
  GOOGLESQL_RETURN_IF_ERROR(ParseDatabaseUri(
      request->database(), &project_id, &instance_id, &database_id));
  GOOGLESQL_RETURN_IF_ERROR(ValidateInstanceId(instance_id));
  GOOGLESQL_RETURN_IF_ERROR(ValidateDatabaseId(database_id));
  const std::string canonical_database =
      MakeDatabaseUri(MakeInstanceUri(project_id, instance_id), database_id);
  if (canonical_database != request->database()) {
    return absl::InvalidArgumentError("Invalid database resource name");
  }
  return ctx->env()
      ->database_manager()
      ->GetDatabase(request->database())
      .status();
}
REGISTER_GRPC_HANDLER(Spanner, FetchCacheUpdate);

}  // namespace frontend
}  // namespace emulator
}  // namespace spanner
}  // namespace google

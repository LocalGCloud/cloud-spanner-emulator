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

#ifndef THIRD_PARTY_CLOUD_SPANNER_EMULATOR_FRONTEND_HANDLERS_REQUEST_STATS_H_
#define THIRD_PARTY_CLOUD_SPANNER_EMULATOR_FRONTEND_HANDLERS_REQUEST_STATS_H_

#include "google/spanner/v1/spanner.pb.h"
#include "absl/strings/string_view.h"
#include "frontend/entities/transaction.h"
#include "frontend/server/request_context.h"

namespace google {
namespace spanner {
namespace emulator {
namespace frontend {

// Returns the attributes of a request on session `session_uri` that the
// SPANNER_SYS statistics record. `partition_token` is the request's partition
// token, if any.
RequestStatsInfo MakeRequestStatsInfo(RequestContext* ctx,
                                      absl::string_view session_uri,
                                      const v1::RequestOptions& options,
                                      absl::string_view partition_token);

}  // namespace frontend
}  // namespace emulator
}  // namespace spanner
}  // namespace google

#endif  // THIRD_PARTY_CLOUD_SPANNER_EMULATOR_FRONTEND_HANDLERS_REQUEST_STATS_H_

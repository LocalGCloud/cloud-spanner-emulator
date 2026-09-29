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

#include "frontend/handlers/request_stats.h"

#include <string>

#include "google/spanner/v1/spanner.pb.h"
#include "absl/strings/string_view.h"
#include "absl/strings/strip.h"
#include "frontend/entities/transaction.h"
#include "frontend/server/request_context.h"
#include "grpcpp/server_context.h"

namespace google {
namespace spanner {
namespace emulator {
namespace frontend {

namespace {

// Returns the address in a gRPC peer URI such as ipv4:127.0.0.1:1234 or
// ipv6:[::1]:1234.
std::string PeerAddress(absl::string_view peer) {
  absl::string_view address = peer;
  if (absl::ConsumePrefix(&address, "ipv4:")) {
    return std::string(address.substr(0, address.rfind(':')));
  }
  if (absl::ConsumePrefix(&address, "ipv6:[")) {
    return std::string(address.substr(0, address.find(']')));
  }
  return std::string(peer);
}

std::string Metadata(const grpc::ServerContext& context,
                     absl::string_view key) {
  auto it = context.client_metadata().find(
      grpc::string_ref(key.data(), key.size()));
  return it == context.client_metadata().end()
             ? ""
             : std::string(it->second.data(), it->second.size());
}

std::string Priority(v1::RequestOptions::Priority priority) {
  switch (priority) {
    case v1::RequestOptions::PRIORITY_LOW:
      return "LOW";
    case v1::RequestOptions::PRIORITY_MEDIUM:
      return "MEDIUM";
    default:
      // Requests have high priority unless they specify otherwise.
      return "HIGH";
  }
}

}  // namespace

RequestStatsInfo MakeRequestStatsInfo(RequestContext* ctx,
                                      absl::string_view session_uri,
                                      const v1::RequestOptions& options,
                                      absl::string_view partition_token) {
  RequestStatsInfo info;
  info.request_tag = options.request_tag();
  info.session_id = session_uri.substr(session_uri.rfind('/') + 1);
  info.partitioned = !partition_token.empty();
  info.priority = Priority(options.priority());
  if (ctx->grpc() != nullptr) {
    info.client_ip_address = PeerAddress(ctx->grpc()->peer());
    info.api_client_header = Metadata(*ctx->grpc(), "x-goog-api-client");
    info.user_agent_header = Metadata(*ctx->grpc(), "user-agent");
  }
  return info;
}

}  // namespace frontend
}  // namespace emulator
}  // namespace spanner
}  // namespace google

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
#include <memory>
#include <string>
#include <utility>
#include <vector>

#include "google/protobuf/empty.pb.h"
#include "google/spanner/v1/spanner.pb.h"
#include "backend/schema/catalog/access_policy.h"
#include "common/errors.h"
#include "common/limits.h"
#include "frontend/collections/database_manager.h"
#include "frontend/common/labels.h"
#include "frontend/common/list_filter.h"
#include "frontend/common/uris.h"
#include "frontend/entities/database.h"
#include "frontend/entities/session.h"
#include "frontend/server/environment.h"
#include "frontend/server/handler.h"
#include "googlesql/base/status_macros.h"
#include "absl/status/status.h"
#include "absl/status/statusor.h"
#include "absl/strings/match.h"
#include "absl/strings/str_cat.h"
#include "absl/strings/strip.h"

namespace protobuf_api = ::google::protobuf;

namespace google {
namespace spanner {
namespace emulator {
namespace frontend {

namespace {

// Sessions may use any role of the database, including the system roles.
absl::Status ValidateCreatorRole(const std::string& creator_role,
                                 const Database& database) {
  if (creator_role.empty()) {
    return absl::OkStatus();
  }
  return backend::AccessPolicy::Create(
             database.backend()->GetLatestSchema(), creator_role)
      .status();
}

// Evaluates a ListSessions filter, which supports labels.<key>.
absl::StatusOr<bool> SessionMatchesFilter(const ListFilter& filter,
                                          const spanner_api::Session& session) {
  return filter.Matches([&session](absl::string_view field,
                                   absl::string_view op,
                                   absl::string_view value)
                            -> absl::StatusOr<bool> {
    if (absl::ConsumePrefix(&field, "labels.")) {
      return MatchLabel(session.labels(), field, op, value);
    }
    return absl::InvalidArgumentError(
        absl::StrCat("Unsupported session filter field: ", field));
  });
}

}  // namespace

// Creates a new session.
absl::Status CreateSession(RequestContext* ctx,
                           const spanner_api::CreateSessionRequest* request,
                           spanner_api::Session* response) {
  // Validate the request.
  absl::string_view project_id, instance_id, database_id;
  GOOGLESQL_RETURN_IF_ERROR(ParseDatabaseUri(request->database(), &project_id,
                                   &instance_id, &database_id));
  GOOGLESQL_RETURN_IF_ERROR(ValidateLabels(request->session().labels()));

  // Check that the instance is valid.
  GOOGLESQL_ASSIGN_OR_RETURN(std::shared_ptr<Instance> instance,
                   GetInstance(ctx, MakeInstanceUri(project_id, instance_id)));

  // Fetch the database.
  GOOGLESQL_ASSIGN_OR_RETURN(
      std::shared_ptr<Database> database,
      ctx->env()->database_manager()->GetDatabase(request->database()));

  GOOGLESQL_RETURN_IF_ERROR(
      ValidateCreatorRole(request->session().creator_role(), *database));

  // Create a session.
  Labels labels(request->session().labels().begin(),
                request->session().labels().end());
  GOOGLESQL_ASSIGN_OR_RETURN(std::shared_ptr<Session> session,
                   ctx->env()->session_manager()->CreateSession(
                       labels, request->session().multiplexed(), database,
                       ctx->env()->mux_txn_manager(),
                       request->session().creator_role()));

  // Return details about the newly created session.
  return session->ToProto(response, /*include_labels=*/true);
}
REGISTER_GRPC_HANDLER(Spanner, CreateSession);

// Creates a batch of new sessions.
absl::Status BatchCreateSessions(
    RequestContext* ctx, const spanner_api::BatchCreateSessionsRequest* request,
    spanner_api::BatchCreateSessionsResponse* response) {
  // Validate the request.
  absl::string_view project_id, instance_id, database_id;
  GOOGLESQL_RETURN_IF_ERROR(ParseDatabaseUri(request->database(), &project_id,
                                   &instance_id, &database_id));
  GOOGLESQL_RETURN_IF_ERROR(ValidateLabels(request->session_template().labels()));
  if (request->session_count() < 0) {
    return error::TooFewSessions(request->session_count());
  }

  if (request->session_template().multiplexed()) {
    return error::InvalidOperationBatchCreateSessions();
  }

  // Check that the instance is valid.
  GOOGLESQL_ASSIGN_OR_RETURN(std::shared_ptr<Instance> instance,
                   GetInstance(ctx, MakeInstanceUri(project_id, instance_id)));

  // Fetch the database to ensure that it exists.
  GOOGLESQL_ASSIGN_OR_RETURN(
      std::shared_ptr<Database> database,
      ctx->env()->database_manager()->GetDatabase(request->database()));

  GOOGLESQL_RETURN_IF_ERROR(ValidateCreatorRole(
      request->session_template().creator_role(), *database));

  // Silently truncate requested session count to max allowed session count.
  const int32_t actual_session_count =
      std::min(limits::kMaxBatchCreateSessionsCount, request->session_count());

  // Create the requested sessions.
  std::vector<std::shared_ptr<Session>> sessions(actual_session_count);
  Labels labels(request->session_template().labels().begin(),
                request->session_template().labels().end());
  for (int i = 0; i < sessions.size(); ++i) {
    // Mux does not support batch create sessions. So its ok to set the
    // mux_txn_manager to null.
    GOOGLESQL_ASSIGN_OR_RETURN(
        sessions[i],
        ctx->env()->session_manager()->CreateSession(
            labels, request->session_template().multiplexed(), database,
            /*mux_txn_manager=*/nullptr,
            request->session_template().creator_role()));
  }

  // Return details about the newly created session.
  for (const auto& session : sessions) {
    GOOGLESQL_RETURN_IF_ERROR(session->ToProto(response->add_session()));
  }
  return absl::OkStatus();
}
REGISTER_GRPC_HANDLER(Spanner, BatchCreateSessions);

// Gets information about a particular session.
absl::Status GetSession(RequestContext* ctx,
                        const spanner_api::GetSessionRequest* request,
                        spanner_api::Session* response) {
  absl::string_view project_id, instance_id, database_id, session_id;
  GOOGLESQL_RETURN_IF_ERROR(ParseSessionUri(request->name(), &project_id, &instance_id,
                                  &database_id, &session_id));
  GOOGLESQL_ASSIGN_OR_RETURN(std::shared_ptr<Session> session,
                   ctx->env()->session_manager()->GetSession(request->name()));
  return session->ToProto(response, /*include_labels=*/false);
}
REGISTER_GRPC_HANDLER(Spanner, GetSession);

// Lists all sessions in a given database that match the specified filter.
absl::Status ListSessions(RequestContext* ctx,
                          const spanner_api::ListSessionsRequest* request,
                          spanner_api::ListSessionsResponse* response) {
  // Validate the request.
  absl::string_view project_id, instance_id, database_id;
  GOOGLESQL_RETURN_IF_ERROR(ParseDatabaseUri(request->database(), &project_id,
                                   &instance_id, &database_id));

  // Check that the instance is valid.
  GOOGLESQL_ASSIGN_OR_RETURN(std::shared_ptr<Instance> instance,
                   GetInstance(ctx, MakeInstanceUri(project_id, instance_id)));

  // Fetch the database to ensure that it exists.
  GOOGLESQL_ASSIGN_OR_RETURN(
      std::shared_ptr<Database> database,
      ctx->env()->database_manager()->GetDatabase(request->database()));

  GOOGLESQL_ASSIGN_OR_RETURN(const ListFilter filter,
                             ListFilter::Parse(request->filter()));
  GOOGLESQL_RETURN_IF_ERROR(
      SessionMatchesFilter(filter, spanner_api::Session()).status());
  std::string page_start;
  if (!request->page_token().empty()) {
    GOOGLESQL_ASSIGN_OR_RETURN(
        page_start,
        ParseListPageToken(request->page_token(), request->filter()));
    if (!absl::StartsWith(page_start,
                          absl::StrCat(database->database_uri(), "/sessions/"))) {
      return absl::InvalidArgumentError(
          "Page token must be a session in the database");
    }
  }

  // List all sessions for the given database.
  GOOGLESQL_ASSIGN_OR_RETURN(
      std::vector<std::shared_ptr<Session>> sessions,
      ctx->env()->session_manager()->ListSessions(database->database_uri()));

  int32_t page_size = request->page_size();
  static const int32_t kMaxPageSize = 1000;
  if (page_size <= 0 || page_size > kMaxPageSize) {
    page_size = kMaxPageSize;
  }

  // Sessions returned from session manager are sorted by session_uri and
  // thus the next_page_token resumes at the first matching session after the
  // requested page size.
  for (const auto& session : sessions) {
    if (session->session_uri() < page_start) continue;
    spanner_api::Session proto;
    GOOGLESQL_RETURN_IF_ERROR(session->ToProto(&proto, /*include_labels=*/true));
    GOOGLESQL_ASSIGN_OR_RETURN(const bool matches,
                               SessionMatchesFilter(filter, proto));
    if (!matches) continue;
    if (response->sessions_size() >= page_size) {
      response->set_next_page_token(
          MakeListPageToken(request->filter(), session->session_uri()));
      break;
    }
    *response->add_sessions() = std::move(proto);
  }
  return absl::OkStatus();
}
REGISTER_GRPC_HANDLER(Spanner, ListSessions);

// Ends a session, releasing server resources associated with it.
absl::Status DeleteSession(RequestContext* ctx,
                           const spanner_api::DeleteSessionRequest* request,
                           protobuf_api::Empty* response) {
  absl::string_view project_id, instance_id, database_id, session_id;
  GOOGLESQL_RETURN_IF_ERROR(ParseSessionUri(request->name(), &project_id, &instance_id,
                                  &database_id, &session_id));
  return ctx->env()->session_manager()->DeleteSession(request->name());
}
REGISTER_GRPC_HANDLER(Spanner, DeleteSession);

}  // namespace frontend
}  // namespace emulator
}  // namespace spanner
}  // namespace google

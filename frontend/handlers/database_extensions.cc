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

#include <algorithm>
#include <cstdint>
#include <memory>
#include <string>
#include <utility>
#include <vector>

#include "googlesql/public/value.h"
#include "absl/status/status.h"
#include "absl/status/statusor.h"
#include "absl/strings/match.h"
#include "absl/strings/str_cat.h"
#include "absl/strings/str_join.h"
#include "absl/strings/string_view.h"
#include "absl/synchronization/mutex.h"
#include "absl/time/clock.h"
#include "absl/time/time.h"
#include "absl/types/span.h"
#include "backend/database/database.h"
#include "backend/schema/catalog/grants.h"
#include "backend/schema/catalog/index.h"
#include "backend/schema/catalog/schema.h"
#include "backend/schema/catalog/table.h"
#include "backend/stats/system_stats_collector.h"
#include "common/errors.h"
#include "frontend/collections/operation_manager.h"
#include "frontend/common/list_filter.h"
#include "frontend/common/uris.h"
#include "frontend/converters/time.h"
#include "frontend/converters/values.h"
#include "frontend/entities/database.h"
#include "frontend/server/handler.h"
#include "google/longrunning/operations.pb.h"
#include "google/protobuf/empty.pb.h"
#include "google/protobuf/repeated_ptr_field.h"
#include "google/protobuf/struct.pb.h"
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

// Pages through the operations on the `resource_type` resources of instance
// `parent` that match `filter_expression`, filtering before pagination.
absl::Status ListInstanceOperations(
    RequestContext* ctx, const std::string& parent,
    OperationResourceType resource_type, const std::string& filter_expression,
    int32_t requested_page_size, const std::string& page_token,
    google::protobuf::RepeatedPtrField<operations_api::Operation>* page,
    std::string* next_page_token) {
  GOOGLESQL_RETURN_IF_ERROR(ValidateInstanceParent(ctx, parent));
  GOOGLESQL_ASSIGN_OR_RETURN(const ListFilter filter,
                             ListFilter::Parse(filter_expression));
  GOOGLESQL_RETURN_IF_ERROR(
      FilterMatchesOperation(filter, operations_api::Operation()).status());
  const std::string prefix = absl::StrCat(parent, "/");
  std::string page_start;
  if (!page_token.empty()) {
    GOOGLESQL_ASSIGN_OR_RETURN(page_start,
                               ParseListPageToken(page_token, filter_expression));
    if (!absl::StartsWith(page_start, prefix)) {
      return absl::InvalidArgumentError(
          "Page token must be an operation in the parent instance");
    }
  }
  GOOGLESQL_ASSIGN_OR_RETURN(
      std::vector<std::shared_ptr<Operation>> operations,
      ctx->env()->operation_manager()->ListOperations(prefix));
  const int32_t page_size = EffectivePageSize(requested_page_size);
  for (const auto& operation : operations) {
    operations_api::Operation proto;
    operation->ToProto(&proto);
    absl::string_view operation_id;
    OperationResourceType operation_resource_type;
    GOOGLESQL_RETURN_IF_ERROR(ParseOperationUri(
        proto.name(), /*resource_uri=*/nullptr, &operation_id,
        &operation_resource_type));
    if (operation_resource_type != resource_type ||
        proto.name() < page_start) {
      continue;
    }
    GOOGLESQL_ASSIGN_OR_RETURN(const bool matches,
                               FilterMatchesOperation(filter, proto));
    if (!matches) continue;
    if (page->size() >= page_size) {
      *next_page_token = MakeListPageToken(filter_expression, proto.name());
      break;
    }
    *page->Add() = std::move(proto);
  }
  return absl::OkStatus();
}

constexpr char kDefaultSplitPointInitiator[] = "CloudAddSplitPointsAPI";
constexpr int kMaxSplitPointInitiatorLength = 50;
constexpr absl::Duration kDefaultSplitPointExpiration = absl::Hours(10 * 24);
constexpr absl::Duration kMaxSplitPointExpiration = absl::Hours(30 * 24);

// Formats the key parts of a split key like SPANNER_SYS.USER_SPLIT_POINTS:
// `key_parts` are the leading parts of `key_columns`, and the parts that are
// missing are shown as <begin>.
absl::StatusOr<std::string> FormatSplitKeyParts(
    const google::protobuf::ListValue& key_parts, int start,
    absl::Span<const backend::KeyColumn* const> key_columns) {
  std::vector<std::string> parts;
  for (int i = 0; i < key_columns.size(); ++i) {
    if (start + i >= key_parts.values_size()) {
      parts.push_back("<begin>");
      continue;
    }
    GOOGLESQL_ASSIGN_OR_RETURN(
        googlesql::Value value,
        ValueFromProto(key_parts.values(start + i),
                       key_columns[i]->column()->GetType()));
    parts.push_back(backend::FormatKeyValue(value));
  }
  return absl::StrJoin(parts, ",");
}

// Returns the split points of `split_points` for SPANNER_SYS.
absl::StatusOr<std::vector<backend::UserSplitPoint>> ResolveSplitPoints(
    const backend::Schema& schema,
    const database_api::SplitPoints& split_points,
    const std::string& initiator, absl::Time now) {
  const backend::Table* table = schema.FindTable(split_points.table());
  if (table == nullptr) {
    return error::TableNotFound(split_points.table());
  }
  const backend::Index* index = nullptr;
  if (!split_points.index().empty()) {
    index = schema.FindIndex(split_points.index());
    if (index == nullptr) {
      return error::IndexNotFound(split_points.index());
    }
    if (index->indexed_table() != table) {
      return absl::InvalidArgumentError(absl::StrCat(
          "Index ", index->Name(), " is not an index on table ",
          table->Name()));
    }
  }
  if (split_points.keys().empty()) {
    return absl::InvalidArgumentError("Split points must have keys");
  }
  absl::Time expire_time = now + kDefaultSplitPointExpiration;
  if (split_points.has_expire_time()) {
    GOOGLESQL_ASSIGN_OR_RETURN(expire_time,
                               TimestampFromProto(split_points.expire_time()));
    if (expire_time > now + kMaxSplitPointExpiration) {
      return absl::InvalidArgumentError(
          "The expiration time of split points must be at most 30 days in "
          "the future");
    }
  }

  // An index split key has the index key parts and then the primary key
  // parts of the indexed table.
  const absl::Span<const backend::KeyColumn* const> key_columns =
      index == nullptr ? table->primary_key()
                       : index->index_data_table()->primary_key();
  const int index_key_size =
      index == nullptr ? 0 : index->key_columns().size();
  std::vector<backend::UserSplitPoint> result;
  for (const database_api::SplitPoints::Key& key : split_points.keys()) {
    if (key.key_parts().values_size() > key_columns.size()) {
      return error::WrongNumberOfKeyParts(
          index == nullptr ? table->Name() : index->Name(), key_columns.size(),
          key.key_parts().values_size(), key.key_parts().ShortDebugString());
    }
    std::string split_key;
    if (index == nullptr) {
      GOOGLESQL_ASSIGN_OR_RETURN(
          std::string parts, FormatSplitKeyParts(key.key_parts(), 0,
                                                 key_columns));
      split_key = absl::StrCat(table->Name(), "(", parts, ")");
    } else {
      GOOGLESQL_ASSIGN_OR_RETURN(
          std::string index_parts,
          FormatSplitKeyParts(key.key_parts(), 0,
                              key_columns.subspan(0, index_key_size)));
      GOOGLESQL_ASSIGN_OR_RETURN(
          std::string table_parts,
          FormatSplitKeyParts(key.key_parts(), index_key_size,
                              key_columns.subspan(index_key_size)));
      split_key = absl::StrCat("Index: ", index->Name(), " on ", table->Name(),
                               ", Index Key: (", index_parts,
                               "), Primary Table Key: (", table_parts, ")");
    }
    result.push_back({.table_name = table->Name(),
                      .index_name = index == nullptr ? "" : index->Name(),
                      .initiator = initiator,
                      .split_key = std::move(split_key),
                      .expire_time = expire_time});
  }
  return result;
}

}  // namespace

// Split points are an optimization hint. Local storage does not need physical
// splits, so a valid request records the split points for
// SPANNER_SYS.USER_SPLIT_POINTS without changing how data is stored.
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
  GOOGLESQL_ASSIGN_OR_RETURN(
      std::shared_ptr<Database> database,
      ctx->env()->database_manager()->GetDatabase(request->database()));

  // Longer initiators are trimmed.
  std::string initiator =
      request->initiator().empty()
          ? kDefaultSplitPointInitiator
          : std::string(absl::ClippedSubstr(request->initiator(), 0,
                                            kMaxSplitPointInitiatorLength));
  const backend::Schema* schema = database->backend()->GetLatestSchema();
  const absl::Time now = absl::Now();
  std::vector<backend::UserSplitPoint> split_points;
  for (const database_api::SplitPoints& request_split_points :
       request->split_points()) {
    GOOGLESQL_ASSIGN_OR_RETURN(
        std::vector<backend::UserSplitPoint> resolved,
        ResolveSplitPoints(*schema, request_split_points, initiator, now));
    split_points.insert(split_points.end(), resolved.begin(), resolved.end());
  }
  return database->backend()->AddSplitPoints(split_points);
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
  if (request->progress() < 0 || request->progress() > 100) {
    return absl::InvalidArgumentError(
        absl::StrCat("progress must be between 0 and 100: ",
                     request->progress()));
  }
  if (request->has_status() && request->status().code() != 0) {
    operation->SetError(
        absl::Status(static_cast<absl::StatusCode>(request->status().code()),
                     request->status().message()));
    return absl::OkStatus();
  }
  // An OK status or full progress completes a pending operation; graph
  // operations are schema updates, whose response is Empty.
  operations_api::Operation proto;
  operation->ToProto(&proto);
  if (!proto.done() && (request->has_status() || request->progress() == 100)) {
    operation->SetResponse(google::protobuf::Empty());
  }
  return absl::OkStatus();
}
REGISTER_GRPC_HANDLER(DatabaseAdmin, InternalUpdateGraphOperation);

absl::Status ListBackupOperations(
    RequestContext* ctx,
    const database_api::ListBackupOperationsRequest* request,
    database_api::ListBackupOperationsResponse* response) {
  return ListInstanceOperations(
      ctx, request->parent(), OperationResourceType::kBackup,
      request->filter(), request->page_size(), request->page_token(),
      response->mutable_operations(), response->mutable_next_page_token());
}
REGISTER_GRPC_HANDLER(DatabaseAdmin, ListBackupOperations);

absl::Status ListDatabaseOperations(
    RequestContext* ctx,
    const database_api::ListDatabaseOperationsRequest* request,
    database_api::ListDatabaseOperationsResponse* response) {
  return ListInstanceOperations(
      ctx, request->parent(), OperationResourceType::kDatabase,
      request->filter(), request->page_size(), request->page_token(),
      response->mutable_operations(), response->mutable_next_page_token());
}
REGISTER_GRPC_HANDLER(DatabaseAdmin, ListDatabaseOperations);

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
  GOOGLESQL_ASSIGN_OR_RETURN(
      std::shared_ptr<Database> database,
      ctx->env()->database_manager()->GetDatabase(request->parent()));
  const std::string prefix = absl::StrCat(request->parent(), "/databaseRoles/");
  if (!request->page_token().empty() &&
      (!absl::StartsWith(request->page_token(), prefix) ||
       request->page_token().size() == prefix.size())) {
    return absl::InvalidArgumentError("Invalid database role page token");
  }
  // Every database has the system roles.
  std::vector<std::string> names = {
      absl::StrCat(prefix, backend::kPublicRole),
      absl::StrCat(prefix, backend::kSpannerInfoReaderRole),
      absl::StrCat(prefix, backend::kSpannerSysReaderRole)};
  for (const backend::Role* role :
       database->backend()->GetLatestSchema()->roles()) {
    names.push_back(absl::StrCat(prefix, role->Name()));
  }
  std::sort(names.begin(), names.end());
  const int32_t page_size = EffectivePageSize(request->page_size());
  for (const std::string& name : names) {
    if (name < request->page_token()) {
      continue;
    }
    if (response->database_roles_size() >= page_size) {
      response->set_next_page_token(name);
      break;
    }
    response->add_database_roles()->set_name(name);
  }
  return absl::OkStatus();
}
REGISTER_GRPC_HANDLER(DatabaseAdmin, ListDatabaseRoles);

}  // namespace frontend
}  // namespace emulator
}  // namespace spanner
}  // namespace google

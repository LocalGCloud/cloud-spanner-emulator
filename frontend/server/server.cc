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

#include "frontend/server/server.h"

#include <memory>
#include <string>
#include <utility>

#include "googlesql/base/logging.h"
#include "google/iam/v1/iam_policy.pb.h"
#include "google/iam/v1/policy.pb.h"
#include "google/longrunning/operations.grpc.pb.h"
#include "google/protobuf/empty.pb.h"
#include "google/rpc/error_details.pb.h"
#include "google/spanner/admin/database/v1/spanner_database_admin.grpc.pb.h"
#include "google/spanner/admin/instance/v1/spanner_instance_admin.grpc.pb.h"
#include "google/spanner/v1/commit_response.pb.h"
#include "google/spanner/v1/result_set.pb.h"
#include "google/spanner/v1/spanner.grpc.pb.h"
#include "google/spanner/v1/spanner.pb.h"
#include "google/spanner/v1/transaction.pb.h"
#include "absl/memory/memory.h"
#include "common/constants.h"
#include "common/errors.h"
#include "common/limits.h"
#include "frontend/common/status.h"
#include "frontend/server/handler.h"
#include "frontend/server/request_context.h"
#include "grpcpp/server_builder.h"
#include "grpcpp/support/status.h"

namespace google {
namespace spanner {
namespace emulator {
namespace frontend {

// Convenience namespace aliases.
namespace database_api = ::google::spanner::admin::database::v1;
namespace iam_api = ::google::iam::v1;
namespace instance_api = ::google::spanner::admin::instance::v1;
namespace operations_api = ::google::longrunning;
namespace protobuf_api = ::google::protobuf;
namespace spanner_api = ::google::spanner::v1;

namespace {

void MaybeAddTrailingMetadata(const absl::Status& status, RequestContext* ctx) {
  if (!status.ok()) {
    // Check for ResourceInfo within the returned status and append it as extra
    // trailing metadata. The Java client library expects the ResourceInfo to be
    // added as additional trailing metadata.
    auto payload = status.GetPayload(kResourceInfoType);
    if (payload.has_value()) {
      std::string serialized_info(payload.value());
      ctx->grpc()->AddTrailingMetadata(kResourceInfoBinaryHeader,
                                       serialized_info);
    }
  }
}

}  // namespace

// Invokes the given unary gRPC method on the given service by looking up the
// handler registry. Returns INTERNAL error if the handler could not be found.
template <typename RequestT, typename ResponseT>
absl::Status Invoke(const std::string& service_name,
                    const std::string& method_name,
                    grpc::ServerContext* grpc_ctx, ServerEnv* env,
                    const RequestT* request, ResponseT* response) {
  GRPCHandlerBase* handler = GetHandler(service_name, method_name);
  if (!handler) {
    return error::Internal(absl::StrCat("Could not find handler for ",
                                        service_name, ".", method_name));
  }
  RequestContext ctx(env, grpc_ctx);
  absl::Status status =
      dynamic_cast<UnaryGRPCHandler<RequestT, ResponseT>*>(handler)->Run(
          &ctx, request, response);
  MaybeAddTrailingMetadata(status, &ctx);
  return status;
}

// Invokes the given server streaming gRPC method on the given service by
// looking up the handler registry. Returns INTERNAL error if the handler could
// not be found.
template <typename RequestT, typename ResponseT>
absl::Status Invoke(const std::string& service_name,
                    const std::string& method_name,
                    grpc::ServerContext* grpc_ctx, ServerEnv* env,
                    const RequestT* request,
                    grpc::ServerWriter<ResponseT>* writer) {
  GRPCHandlerBase* handler = GetHandler(service_name, method_name);
  if (!handler) {
    return error::Internal(absl::StrCat("Could not find handler for ",
                                        service_name, ".", method_name));
  }
  RequestContext ctx(env, grpc_ctx);
  absl::Status status =
      dynamic_cast<ServerStreamingGRPCHandler<RequestT, ResponseT>*>(handler)
          ->Run(&ctx, request, writer);
  MaybeAddTrailingMetadata(status, &ctx);
  return status;
}

#define DEFINE_GRPC_METHOD(ServiceName, MethodName, RequestType, ResponseType) \
  grpc::Status MethodName(grpc::ServerContext* grpc_ctx,                       \
                          const RequestType* request, ResponseType* response)  \
      override {                                                               \
    return ToGRPCStatus(                                                       \
        Invoke(#ServiceName, #MethodName, grpc_ctx, env_, request, response)); \
  }

// Implementation of the Spanner gRPC service.
class SpannerService : public spanner_api::Spanner::Service {
 public:
  explicit SpannerService(ServerEnv* env) : env_(env) {}

  // Sessions.
  DEFINE_GRPC_METHOD(Spanner, CreateSession, spanner_api::CreateSessionRequest,
                     spanner_api::Session)
  DEFINE_GRPC_METHOD(Spanner, GetSession, spanner_api::GetSessionRequest,
                     spanner_api::Session)
  DEFINE_GRPC_METHOD(Spanner, ListSessions, spanner_api::ListSessionsRequest,
                     spanner_api::ListSessionsResponse)
  DEFINE_GRPC_METHOD(Spanner, DeleteSession, spanner_api::DeleteSessionRequest,
                     protobuf_api::Empty)
  DEFINE_GRPC_METHOD(Spanner, BatchCreateSessions,
                     spanner_api::BatchCreateSessionsRequest,
                     spanner_api::BatchCreateSessionsResponse)

  // Reads.
  DEFINE_GRPC_METHOD(Spanner, Read, spanner_api::ReadRequest,
                     spanner_api::ResultSet);
  DEFINE_GRPC_METHOD(Spanner, StreamingRead, spanner_api::ReadRequest,
                     grpc::ServerWriter<v1::PartialResultSet>);

  // Queries.
  DEFINE_GRPC_METHOD(Spanner, ExecuteSql, spanner_api::ExecuteSqlRequest,
                     spanner_api::ResultSet);
  DEFINE_GRPC_METHOD(Spanner, ExecuteStreamingSql,
                     spanner_api::ExecuteSqlRequest,
                     grpc::ServerWriter<v1::PartialResultSet>);
  DEFINE_GRPC_METHOD(Spanner, ExecuteBatchDml,
                     spanner_api::ExecuteBatchDmlRequest,
                     spanner_api::ExecuteBatchDmlResponse);

  // Batch
  DEFINE_GRPC_METHOD(Spanner, BatchWrite, spanner_api::BatchWriteRequest,
                     grpc::ServerWriter<v1::BatchWriteResponse>);

  // Client cache.
  DEFINE_GRPC_METHOD(Spanner, FetchCacheUpdate,
                     spanner_api::FetchCacheUpdateRequest,
                     grpc::ServerWriter<spanner_api::CacheUpdate>);

  // Partitions.
  DEFINE_GRPC_METHOD(Spanner, PartitionRead, spanner_api::PartitionReadRequest,
                     spanner_api::PartitionResponse);
  DEFINE_GRPC_METHOD(Spanner, PartitionQuery,
                     spanner_api::PartitionQueryRequest,
                     spanner_api::PartitionResponse);

  // Transactions.
  DEFINE_GRPC_METHOD(Spanner, BeginTransaction,
                     spanner_api::BeginTransactionRequest,
                     spanner_api::Transaction);
  DEFINE_GRPC_METHOD(Spanner, Commit, spanner_api::CommitRequest,
                     spanner_api::CommitResponse);
  DEFINE_GRPC_METHOD(Spanner, Rollback, spanner_api::RollbackRequest,
                     protobuf_api::Empty);

 private:
  ServerEnv* const env_;
};

// Implementation of the DatabaseAdmin gRPC service.
class DatabaseAdminService : public database_api::DatabaseAdmin::Service {
 public:
  explicit DatabaseAdminService(ServerEnv* env) : env_(env) {}

  // Databases.
  DEFINE_GRPC_METHOD(DatabaseAdmin, ListDatabases,
                     database_api::ListDatabasesRequest,
                     database_api::ListDatabasesResponse);
  DEFINE_GRPC_METHOD(DatabaseAdmin, CreateDatabase,
                     database_api::CreateDatabaseRequest,
                     operations_api::Operation);
  DEFINE_GRPC_METHOD(DatabaseAdmin, GetDatabase,
                     database_api::GetDatabaseRequest, database_api::Database);
  DEFINE_GRPC_METHOD(DatabaseAdmin, UpdateDatabase,
                     database_api::UpdateDatabaseRequest,
                     operations_api::Operation);
  DEFINE_GRPC_METHOD(DatabaseAdmin, DropDatabase,
                     database_api::DropDatabaseRequest, protobuf_api::Empty);

  // Backups.
  DEFINE_GRPC_METHOD(DatabaseAdmin, CreateBackup,
                     database_api::CreateBackupRequest,
                     operations_api::Operation);
  DEFINE_GRPC_METHOD(DatabaseAdmin, CopyBackup,
                     database_api::CopyBackupRequest,
                     operations_api::Operation);
  DEFINE_GRPC_METHOD(DatabaseAdmin, GetBackup,
                     database_api::GetBackupRequest, database_api::Backup);
  DEFINE_GRPC_METHOD(DatabaseAdmin, ListBackups,
                     database_api::ListBackupsRequest,
                     database_api::ListBackupsResponse);
  DEFINE_GRPC_METHOD(DatabaseAdmin, UpdateBackup,
                     database_api::UpdateBackupRequest, database_api::Backup);
  DEFINE_GRPC_METHOD(DatabaseAdmin, DeleteBackup,
                     database_api::DeleteBackupRequest, protobuf_api::Empty);
  DEFINE_GRPC_METHOD(DatabaseAdmin, RestoreDatabase,
                     database_api::RestoreDatabaseRequest,
                     operations_api::Operation);
  DEFINE_GRPC_METHOD(DatabaseAdmin, ListBackupOperations,
                     database_api::ListBackupOperationsRequest,
                     database_api::ListBackupOperationsResponse);

  // Backup schedules.
  DEFINE_GRPC_METHOD(DatabaseAdmin, CreateBackupSchedule,
                     database_api::CreateBackupScheduleRequest,
                     database_api::BackupSchedule);
  DEFINE_GRPC_METHOD(DatabaseAdmin, GetBackupSchedule,
                     database_api::GetBackupScheduleRequest,
                     database_api::BackupSchedule);
  DEFINE_GRPC_METHOD(DatabaseAdmin, ListBackupSchedules,
                     database_api::ListBackupSchedulesRequest,
                     database_api::ListBackupSchedulesResponse);
  DEFINE_GRPC_METHOD(DatabaseAdmin, UpdateBackupSchedule,
                     database_api::UpdateBackupScheduleRequest,
                     database_api::BackupSchedule);
  DEFINE_GRPC_METHOD(DatabaseAdmin, DeleteBackupSchedule,
                     database_api::DeleteBackupScheduleRequest,
                     protobuf_api::Empty);

  // Schema.
  DEFINE_GRPC_METHOD(DatabaseAdmin, UpdateDatabaseDdl,
                     database_api::UpdateDatabaseDdlRequest,
                     operations_api::Operation);
  DEFINE_GRPC_METHOD(DatabaseAdmin, GetDatabaseDdl,
                     database_api::GetDatabaseDdlRequest,
                     database_api::GetDatabaseDdlResponse);

  // Extended database administration.
  DEFINE_GRPC_METHOD(DatabaseAdmin, AddSplitPoints,
                     database_api::AddSplitPointsRequest,
                     database_api::AddSplitPointsResponse);
  DEFINE_GRPC_METHOD(DatabaseAdmin, InternalUpdateGraphOperation,
                     database_api::InternalUpdateGraphOperationRequest,
                     database_api::InternalUpdateGraphOperationResponse);
  DEFINE_GRPC_METHOD(DatabaseAdmin, ListDatabaseOperations,
                     database_api::ListDatabaseOperationsRequest,
                     database_api::ListDatabaseOperationsResponse);
  DEFINE_GRPC_METHOD(DatabaseAdmin, ListDatabaseRoles,
                     database_api::ListDatabaseRolesRequest,
                     database_api::ListDatabaseRolesResponse);

  // Policies.
  DEFINE_GRPC_METHOD(InstanceAdmin, SetIamPolicy, iam_api::SetIamPolicyRequest,
                     iam_api::Policy);
  DEFINE_GRPC_METHOD(InstanceAdmin, GetIamPolicy, iam_api::GetIamPolicyRequest,
                     iam_api::Policy);
  DEFINE_GRPC_METHOD(InstanceAdmin, TestIamPermissions,
                     iam_api::TestIamPermissionsRequest,
                     iam_api::TestIamPermissionsResponse);

 private:
  ServerEnv* const env_;
};

// Implementation of the InstanceAdmin gRPC service.
class InstanceAdminService : public instance_api::InstanceAdmin::Service {
 public:
  explicit InstanceAdminService(ServerEnv* env) : env_(env) {}

  // Instance configs.
  DEFINE_GRPC_METHOD(InstanceAdmin, ListInstanceConfigs,
                     instance_api::ListInstanceConfigsRequest,
                     instance_api::ListInstanceConfigsResponse);
  DEFINE_GRPC_METHOD(InstanceAdmin, GetInstanceConfig,
                     instance_api::GetInstanceConfigRequest,
                     instance_api::InstanceConfig);
  DEFINE_GRPC_METHOD(InstanceAdmin, CreateInstanceConfig,
                     instance_api::CreateInstanceConfigRequest,
                     operations_api::Operation);
  DEFINE_GRPC_METHOD(InstanceAdmin, UpdateInstanceConfig,
                     instance_api::UpdateInstanceConfigRequest,
                     operations_api::Operation);
  DEFINE_GRPC_METHOD(InstanceAdmin, DeleteInstanceConfig,
                     instance_api::DeleteInstanceConfigRequest,
                     protobuf_api::Empty);
  DEFINE_GRPC_METHOD(InstanceAdmin, ListInstanceConfigOperations,
                     instance_api::ListInstanceConfigOperationsRequest,
                     instance_api::ListInstanceConfigOperationsResponse);

  // Instances.
  DEFINE_GRPC_METHOD(InstanceAdmin, ListInstances,
                     instance_api::ListInstancesRequest,
                     instance_api::ListInstancesResponse);
  DEFINE_GRPC_METHOD(InstanceAdmin, GetInstance,
                     instance_api::GetInstanceRequest, instance_api::Instance);
  DEFINE_GRPC_METHOD(InstanceAdmin, CreateInstance,
                     instance_api::CreateInstanceRequest,
                     operations_api::Operation);
  DEFINE_GRPC_METHOD(InstanceAdmin, UpdateInstance,
                     instance_api::UpdateInstanceRequest,
                     operations_api::Operation);
  DEFINE_GRPC_METHOD(InstanceAdmin, DeleteInstance,
                     instance_api::DeleteInstanceRequest, protobuf_api::Empty);
  DEFINE_GRPC_METHOD(InstanceAdmin, MoveInstance,
                     instance_api::MoveInstanceRequest,
                     operations_api::Operation);

  // Instance partitions.
  DEFINE_GRPC_METHOD(InstanceAdmin, ListInstancePartitions,
                     instance_api::ListInstancePartitionsRequest,
                     instance_api::ListInstancePartitionsResponse);
  DEFINE_GRPC_METHOD(InstanceAdmin, GetInstancePartition,
                     instance_api::GetInstancePartitionRequest,
                     instance_api::InstancePartition);
  DEFINE_GRPC_METHOD(InstanceAdmin, CreateInstancePartition,
                     instance_api::CreateInstancePartitionRequest,
                     operations_api::Operation);
  DEFINE_GRPC_METHOD(InstanceAdmin, UpdateInstancePartition,
                     instance_api::UpdateInstancePartitionRequest,
                     operations_api::Operation);
  DEFINE_GRPC_METHOD(InstanceAdmin, DeleteInstancePartition,
                     instance_api::DeleteInstancePartitionRequest,
                     protobuf_api::Empty);
  DEFINE_GRPC_METHOD(InstanceAdmin, ListInstancePartitionOperations,
                     instance_api::ListInstancePartitionOperationsRequest,
                     instance_api::ListInstancePartitionOperationsResponse);

  // Policies.
  DEFINE_GRPC_METHOD(InstanceAdmin, SetIamPolicy, iam_api::SetIamPolicyRequest,
                     iam_api::Policy);
  DEFINE_GRPC_METHOD(InstanceAdmin, GetIamPolicy, iam_api::GetIamPolicyRequest,
                     iam_api::Policy);
  DEFINE_GRPC_METHOD(InstanceAdmin, TestIamPermissions,
                     iam_api::TestIamPermissionsRequest,
                     iam_api::TestIamPermissionsResponse);

 private:
  ServerEnv* const env_;
};

// Implementation of the Operations gRPC service.
class OperationsService : public operations_api::Operations::Service {
 public:
  explicit OperationsService(ServerEnv* env) : env_(env) {}

  DEFINE_GRPC_METHOD(Operations, ListOperations,
                     operations_api::ListOperationsRequest,
                     operations_api::ListOperationsResponse);
  DEFINE_GRPC_METHOD(Operations, GetOperation,
                     operations_api::GetOperationRequest,
                     operations_api::Operation);
  DEFINE_GRPC_METHOD(Operations, DeleteOperation,
                     operations_api::DeleteOperationRequest,
                     protobuf_api::Empty);
  DEFINE_GRPC_METHOD(Operations, CancelOperation,
                     operations_api::CancelOperationRequest,
                     protobuf_api::Empty);
  DEFINE_GRPC_METHOD(Operations, WaitOperation,
                     operations_api::WaitOperationRequest,
                     operations_api::Operation);

 private:
  ServerEnv* const env_;
};

Server::Server(std::unique_ptr<ServerEnv> env)
    : env_(std::move(env)),
      database_admin_service_(new DatabaseAdminService(env_.get())),
      instance_admin_service_(new InstanceAdminService(env_.get())),
      operations_service_(new OperationsService(env_.get())),
      spanner_service_(new SpannerService(env_.get())) {}

// Server lifecycle methods.
std::unique_ptr<Server> Server::CreateUnstarted() {
  return absl::WrapUnique(new Server(std::make_unique<ServerEnv>()));
}

std::unique_ptr<Server> Server::Create(const Server::Options& options) {
  std::unique_ptr<Server> server = CreateUnstarted();
  if (!server->Start(options)) {
    return nullptr;
  }
  return server;
}

bool Server::Start(const Server::Options& options) {
  if (grpc_server_ != nullptr) {
    ABSL_LOG(ERROR) << "Server has already been started.";
    return false;
  }
  ::grpc::ServerBuilder builder;

  // Configure server address.
  host_ = options.server_address.substr(
      0, options.server_address.find_last_of(':'));
  builder.AddListeningPort(options.server_address,
                           ::grpc::InsecureServerCredentials(), &port_);

  // Configure server message limits.
  builder.AddChannelArgument(GRPC_ARG_MAX_SEND_MESSAGE_LENGTH,
                             limits::kMaxGRPCOutgoingMessageSize);
  builder.AddChannelArgument(GRPC_ARG_MAX_RECEIVE_MESSAGE_LENGTH,
                             limits::kMaxGRPCIncomingMessageSize);

  // Configure services exported on this server.
  builder.RegisterService(spanner_service_.get())
      .RegisterService(database_admin_service_.get())
      .RegisterService(instance_admin_service_.get())
      .RegisterService(operations_service_.get());

  grpc_server_ = builder.BuildAndStart();
  if (grpc_server_ == nullptr || port_ < 0) {
    ABSL_LOG(ERROR) << "Failed to bind to address: " << options.server_address;
    grpc_server_.reset();
    port_ = -1;
    return false;
  }
  return true;
}

void Server::WaitForShutdown() { grpc_server_->Wait(); }

void Server::Shutdown() { grpc_server_->Shutdown(); }

}  // namespace frontend
}  // namespace emulator
}  // namespace spanner
}  // namespace google

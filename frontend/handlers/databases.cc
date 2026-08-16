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

#include <filesystem>
#include <functional>
#include <memory>
#include <string>
#include <vector>

#include "absl/log/absl_log.h"
#include "absl/status/status.h"
#include "absl/status/statusor.h"
#include "absl/strings/string_view.h"
#include "absl/synchronization/mutex.h"
#include "absl/time/time.h"
#include "backend/database/database.h"
#include "backend/schema/ddl/operations.pb.h"
#include "backend/schema/parser/ddl_parser.h"
#include "backend/schema/printer/print_ddl.h"
#include "backend/schema/updater/schema_updater.h"
#include "common/config.h"
#include "common/errors.h"
#include "common/feature_flags.h"
#include "frontend/collections/database_manager.h"
#include "frontend/common/uris.h"
#include "frontend/converters/time.h"
#include "frontend/persistence/backup_catalog.h"
#include "frontend/entities/database.h"
#include "frontend/server/handler.h"
#include "google/longrunning/operations.pb.h"
#include "google/protobuf/empty.pb.h"
#include "google/protobuf/timestamp.pb.h"
#include "google/spanner/admin/database/v1/common.pb.h"
#include "google/spanner/admin/database/v1/spanner_database_admin.pb.h"
#include "googlesql/base/status_macros.h"

namespace google {
namespace spanner {
namespace emulator {
namespace frontend {

namespace {

namespace database_api = ::google::spanner::admin::database::v1;
namespace operations_api = ::google::longrunning;
namespace protobuf_api = ::google::protobuf;

}  // namespace

// Lists all databases in an instance.
absl::Status ListDatabases(RequestContext* ctx,
                           const database_api::ListDatabasesRequest* request,
                           database_api::ListDatabasesResponse* response) {
  // Validate that the ListDatabases request is for a valid instance.
  GOOGLESQL_ASSIGN_OR_RETURN(std::shared_ptr<Instance> instance,
                             GetInstance(ctx, request->parent()));

  // Validate that the page_token provided is a valid database_uri.
  if (!request->page_token().empty()) {
    absl::string_view project_id, instance_id, database_id;
    GOOGLESQL_RETURN_IF_ERROR(ParseDatabaseUri(
        request->page_token(), &project_id, &instance_id, &database_id));
  }

  GOOGLESQL_ASSIGN_OR_RETURN(
      std::vector<std::shared_ptr<Database>> databases,
      ctx->env()->database_manager()->ListDatabases(request->parent()));

  int32_t page_size = request->page_size();
  static const int32_t kMaxPageSize = 1000;
  if (page_size <= 0 || page_size > kMaxPageSize) {
    page_size = kMaxPageSize;
  }

  // Databases returned from database manager are sorted by database_uri and
  // thus we use database uri of first database in next page as next_page_token.
  for (const auto& database : databases) {
    if (response->databases_size() >= page_size) {
      response->set_next_page_token(database->database_uri());
      break;
    }
    if (database->database_uri() >= request->page_token()) {
      GOOGLESQL_RETURN_IF_ERROR(database->ToProto(response->add_databases()));
    }
  }
  return absl::OkStatus();
}
REGISTER_GRPC_HANDLER(DatabaseAdmin, ListDatabases);

// Creates a new database within an instance.
absl::Status CreateDatabase(RequestContext* ctx,
                            const database_api::CreateDatabaseRequest* request,
                            operations_api::Operation* response) {
  absl::MutexLock admin_transaction_lock(
      &ctx->env()->admin_transaction_mutex());
  // Validate the request.
  GOOGLESQL_ASSIGN_OR_RETURN(std::shared_ptr<Instance> instance,
                             GetInstance(ctx, request->parent()));
  if (request->create_statement().empty()) {
    return error::CreateDatabaseMissingCreateStatement();
  }

  // Determine the database dialect.
  database_api::DatabaseDialect dialect;
  // DATABASE_DIALECT_UNSPECIFIED defaults to creating a database with the
  // GOOGLE_STANDARD_SQL dialect.
  if (request->database_dialect() ==
      database_api::DatabaseDialect::DATABASE_DIALECT_UNSPECIFIED) {
    dialect = database_api::DatabaseDialect::GOOGLE_STANDARD_SQL;
  } else if (!EmulatorFeatureFlags::instance()
                  .flags()
                  .enable_postgresql_interface &&
             request->database_dialect() ==
                 database_api::DatabaseDialect::POSTGRESQL) {
    return error::CannotCreatePostgreSQLDialectDatabase();
  } else {
    dialect = request->database_dialect();
  }

  // Extract database name from create statement.
  GOOGLESQL_ASSIGN_OR_RETURN(
      std::unique_ptr<backend::ddl::DDLStatement> stmt,
      backend::ParseDDLByDialect(request->create_statement(), dialect));
  std::string database_name = stmt->create_database().db_name();

  // Validate database name.
  GOOGLESQL_RETURN_IF_ERROR(ValidateDatabaseId(database_name));

  std::string database_uri = MakeDatabaseUri(request->parent(), database_name);
  std::vector<std::string> create_statements;
  for (const std::string& statement : request->extra_statements()) {
    create_statements.push_back(statement);
  }
  // Allocate the terminal operation before mutating database state.
  GOOGLESQL_ASSIGN_OR_RETURN(
      std::shared_ptr<Operation> operation,
      ctx->env()->operation_manager()->CreateOperation(
          database_uri, OperationManager::kAutoGeneratedId));
  operation->ToProto(response);
  auto database_or = ctx->env()->database_manager()->CreateDatabase(
      database_uri,
      backend::SchemaChangeOperation{
          .statements = create_statements,
          .proto_descriptor_bytes = request->proto_descriptors(),
          .database_dialect = dialect,
      });
  if (!database_or.ok()) {
    ctx->env()->operation_manager()->DeleteOperation(response->name())
        .IgnoreError();
    return database_or.status();
  }
  std::shared_ptr<Database> database = *database_or;

  database_api::CreateDatabaseMetadata metadata;
  metadata.set_database(database_uri);
  operation->SetMetadata(metadata);
  database_api::Database response_database;
  GOOGLESQL_RETURN_IF_ERROR(database->ToProto(&response_database));
  operation->SetResponse(response_database);
  operation->ToProto(response);

  auto rollback_uncommitted_create =
      [&](const absl::Status& failure) -> absl::Status {
    ctx->env()->operation_manager()->DeleteOperation(response->name())
        .IgnoreError();
    ctx->env()->database_manager()->DeleteDatabase(database_uri).IgnoreError();
    database.reset();
    absl::Status cleanup_status =
        backend::Database::DeletePersistentStorageDirectory(config::data_dir(),
                                                            database_uri);
    if (!cleanup_status.ok()) {
      return absl::DataLossError(absl::StrCat(
          failure.message(), "; database rollback failed: ",
          cleanup_status.message()));
    }
    return failure;
  };

  // Persist metadata if data_dir is set.
  if (auto* ms = ctx->env()->metadata_store(); ms != nullptr) {
    std::string dialect_str =
        dialect == database_api::DatabaseDialect::POSTGRESQL
            ? "POSTGRESQL"
            : "GOOGLE_STANDARD_SQL";
    // Preserve the exact committed request and descriptor bundle. Canonical
    // latest-schema DDL cannot replay create-then-drop proto sequences.
    GOOGLESQL_ASSIGN_OR_RETURN(
        const absl::Time database_create_time,
        TimestampFromProto(response_database.create_time()));
    ms->AddDatabase(
        request->parent(), database_name, dialect_str, create_statements,
        request->proto_descriptors(),
        absl::FormatTime(absl::RFC3339_full, database_create_time,
                         absl::UTCTimeZone()));
    // Persist ID counters so restored schemas get the same table/column IDs.
    auto counters = database->backend()->GetIdCounterValues();
    ms->UpdateIdCounters(request->parent(), database_name,
                         frontend::MetadataStore::IdCounters{
                             .table_id = counters.table_id,
                             .column_id = counters.column_id,
                             .change_stream_id = counters.change_stream_id,
                         });
    ms->SetPendingOperation(*response);
    absl::Status save_status = ms->Save();
    const bool metadata_committed = save_status.ok();
    if (save_status.ok()) {
      save_status = DatabaseManager::MarkDatabaseMetadataCommitted(
          config::data_dir(), database_uri);
    }
    if (!save_status.ok()) {
      ms->RemoveDatabase(request->parent(), database_name);
      ms->RemovePendingOperation(response->name());
      if (metadata_committed) {
        absl::Status rollback_status = ms->Save();
        if (!rollback_status.ok()) {
          absl::Status reload_status = ms->Load();
          return absl::DataLossError(absl::StrCat(
              save_status.message(), "; metadata rollback failed: ",
              rollback_status.message(),
              reload_status.ok()
                  ? ""
                  : absl::StrCat("; metadata reload failed: ",
                                 reload_status.message())));
        }
      }
      return rollback_uncommitted_create(save_status);
    }
    absl::Status operation_status =
        ctx->env()->backup_catalog()->SaveOperation(*response);
    if (!operation_status.ok()) {
      // The durable metadata journal will promote this terminal operation on
      // restart. The database and its committed marker remain authoritative.
      return operation_status;
    }
    ms->RemovePendingOperation(response->name());
    absl::Status journal_status = ms->Save();
    if (!journal_status.ok()) {
      ABSL_LOG(WARNING)
          << "Failed to clear promoted create-database operation journal "
          << response->name() << ": " << journal_status;
    }
  }
  return absl::OkStatus();
}
REGISTER_GRPC_HANDLER(DatabaseAdmin, CreateDatabase);

// Gets the current state of a database.
absl::Status GetDatabase(RequestContext* ctx,
                         const database_api::GetDatabaseRequest* request,
                         database_api::Database* response) {
  GOOGLESQL_ASSIGN_OR_RETURN(std::shared_ptr<Database> database,
                             GetDatabase(ctx, request->name()));
  return database->ToProto(response);
}
REGISTER_GRPC_HANDLER(DatabaseAdmin, GetDatabase);

// Updates mutable database resource fields.
absl::Status UpdateDatabase(RequestContext* ctx,
                            const database_api::UpdateDatabaseRequest* request,
                            operations_api::Operation* response) {
  absl::MutexLock admin_transaction_lock(
      &ctx->env()->admin_transaction_mutex());
  if (!request->has_database() || request->database().name().empty()) {
    return absl::InvalidArgumentError("Database name must be provided");
  }
  absl::string_view project_id, instance_id, database_id;
  GOOGLESQL_RETURN_IF_ERROR(ParseDatabaseUri(
      request->database().name(), &project_id, &instance_id, &database_id));
  if (MakeDatabaseUri(MakeInstanceUri(project_id, instance_id), database_id) !=
      request->database().name()) {
    return absl::InvalidArgumentError("Database name must be canonical");
  }
  GOOGLESQL_ASSIGN_OR_RETURN(
      std::shared_ptr<Database> database,
      ctx->env()->database_manager()->GetDatabase(request->database().name()));
  if (request->update_mask().paths().empty()) {
    return absl::InvalidArgumentError("Database update_mask must be provided");
  }
  for (const std::string& path : request->update_mask().paths()) {
    if (path != "enable_drop_protection") {
      return absl::InvalidArgumentError(
          absl::StrCat("Unsupported database update field: ", path));
    }
  }
  GOOGLESQL_ASSIGN_OR_RETURN(
      std::shared_ptr<Operation> operation,
      ctx->env()->operation_manager()->CreateOperation(
          request->database().name(), OperationManager::kAutoGeneratedId));
  operation->ToProto(response);
  const bool previous_drop_protection = database->enable_drop_protection();
  database->set_enable_drop_protection(
      request->database().enable_drop_protection());
  database_api::Database database_proto;
  absl::Status proto_status = database->ToProto(&database_proto);
  if (!proto_status.ok()) {
    database->set_enable_drop_protection(previous_drop_protection);
    ctx->env()->operation_manager()->DeleteOperation(response->name())
        .IgnoreError();
    return proto_status;
  }
  operation->SetResponse(database_proto);
  operation->ToProto(response);

  if (auto* metadata = ctx->env()->metadata_store(); metadata != nullptr) {
    const std::string instance_uri = MakeInstanceUri(project_id, instance_id);
    metadata->UpdateDropProtection(instance_uri, std::string(database_id),
                                   database->enable_drop_protection());
    metadata->SetPendingOperation(*response);
    absl::Status save_status = metadata->Save();
    if (!save_status.ok()) {
      database->set_enable_drop_protection(previous_drop_protection);
      metadata->UpdateDropProtection(instance_uri, std::string(database_id),
                                     previous_drop_protection);
      metadata->RemovePendingOperation(response->name());
      ctx->env()->operation_manager()->DeleteOperation(response->name())
          .IgnoreError();
      return save_status;
    }
    absl::Status operation_status =
        ctx->env()->backup_catalog()->SaveOperation(*response);
    if (!operation_status.ok()) {
      database->set_enable_drop_protection(previous_drop_protection);
      metadata->UpdateDropProtection(instance_uri, std::string(database_id),
                                     previous_drop_protection);
      metadata->RemovePendingOperation(response->name());
      absl::Status rollback_status = metadata->Save();
      ctx->env()->operation_manager()->DeleteOperation(response->name())
          .IgnoreError();
      if (!rollback_status.ok()) {
        return absl::DataLossError(absl::StrCat(
            operation_status.message(),
            "; failed to roll back database metadata: ",
            rollback_status.message()));
      }
      return operation_status;
    }
    metadata->RemovePendingOperation(response->name());
    absl::Status journal_status = metadata->Save();
    if (!journal_status.ok()) {
      ABSL_LOG(WARNING)
          << "Failed to clear promoted update-database operation journal "
          << response->name() << ": " << journal_status;
    }
  }
  return absl::OkStatus();
}
REGISTER_GRPC_HANDLER(DatabaseAdmin, UpdateDatabase);

// Updates the schema of a database.
absl::Status UpdateDatabaseDdl(
    RequestContext* ctx, const database_api::UpdateDatabaseDdlRequest* request,
    operations_api::Operation* response) {
  absl::MutexLock admin_transaction_lock(
      &ctx->env()->admin_transaction_mutex());
  // Validate request URI.
  absl::string_view project_id, instance_id, database_id;
  GOOGLESQL_RETURN_IF_ERROR(ParseDatabaseUri(request->database(), &project_id,
                                             &instance_id, &database_id));
  if (MakeDatabaseUri(MakeInstanceUri(project_id, instance_id), database_id) !=
      request->database()) {
    return absl::InvalidArgumentError("Database name must be canonical");
  }

  // Check for request replay.
  if (!request->operation_id().empty()) {
    if (!IsValidOperationId(request->operation_id())) {
      return error::InvalidOperationId(request->operation_id());
    }
    const std::string operation_uri =
        MakeOperationUri(request->database(), request->operation_id());
    auto maybe_operation =
        ctx->env()->operation_manager()->GetOperation(operation_uri);
    if (maybe_operation.ok()) {
      return error::OperationAlreadyExists(operation_uri);
    }
  }

  // Lookup the database by URI.
  GOOGLESQL_ASSIGN_OR_RETURN(std::shared_ptr<Database> database,
                             GetDatabase(ctx, request->database()));
  MetadataStore* ms = ctx->env()->metadata_store();
  absl::MutexLock schema_change_lock(&database->schema_change_mutex());

  std::vector<std::string> statements;
  for (const std::string& statement : request->statements()) {
    statements.push_back(statement);
  }

  // Allocate the operation before applying any schema mutation.
  GOOGLESQL_ASSIGN_OR_RETURN(std::shared_ptr<Operation> operation,
                             ctx->env()->operation_manager()->CreateOperation(
                                 request->database(), request->operation_id()));
  operation->ToProto(response);
  std::string rollback_checkpoint_directory;
  bool ddl_intent_published = false;
  std::function<absl::Status()> publish_ddl_intent = [] {
    return absl::OkStatus();
  };
  if (ms != nullptr) {
    GOOGLESQL_ASSIGN_OR_RETURN(
        rollback_checkpoint_directory,
        DatabaseManager::DdlRollbackCheckpointDirectory(
            config::data_dir(), request->database(), response->name()));
    publish_ddl_intent = [&] {
      ms->SetPendingDdlOperation(
          request->database(),
          {.operation_name = response->name(),
           .statements = statements,
           .proto_descriptor_bytes = request->proto_descriptors(),
           .has_rollback_checkpoint = true});
      absl::Status intent_status = ms->Save();
      if (intent_status.ok()) {
        ddl_intent_published = true;
      }
      if (!intent_status.ok() &&
          intent_status.code() == absl::StatusCode::kDataLoss) {
        ddl_intent_published = true;
        // AtomicFile has already renamed the new metadata at this point. The
        // intent is visible, even though synchronizing its parent failed.
        ABSL_LOG(WARNING) << "Continuing from ambiguously durable DDL intent "
                          << response->name() << ": " << intent_status;
        return absl::OkStatus();
      }
      return intent_status;
    };
  }

  backend::Database* backend_database = database->backend();
  int num_succesful_statements;
  absl::Time commit_timestamp;
  absl::Status backfill_status;
  const backend::SchemaChangeOperation schema_change_operation{
      .statements = statements,
      .proto_descriptor_bytes = request->proto_descriptors(),
      .database_dialect = backend_database->dialect()};
  auto populate_operation = [&]() -> absl::Status {
    database_api::UpdateDatabaseDdlMetadata update_md;
    update_md.set_database(request->database());
    for (const std::string& statement : statements) {
      update_md.add_statements(statement);
    }
    for (int i = 0; i < num_succesful_statements; ++i) {
      GOOGLESQL_ASSIGN_OR_RETURN(*update_md.add_commit_timestamps(),
                                 TimestampToProto(commit_timestamp));
    }
    operation->SetMetadata(update_md);
    if (backfill_status.ok()) {
      operation->SetResponse(protobuf_api::Empty());
    } else {
      operation->SetError(backfill_status);
    }
    operation->ToProto(response);
    return absl::OkStatus();
  };

  bool durable_promotion_completed = ms == nullptr;
  std::function<absl::Status()> schema_change_applied;
  if (ms != nullptr) {
    schema_change_applied = [&]() -> absl::Status {
      absl::Status operation_status = populate_operation();
      if (!operation_status.ok()) {
        backend_database->MarkRestoreRequired();
        operation->SetError(operation_status);
        operation->ToProto(response);
        ABSL_LOG(WARNING) << "Quarantined committed DDL after operation "
                             "serialization failed for "
                          << response->name() << ": " << operation_status;
        return absl::OkStatus();
      }

      std::string instance_uri = MakeInstanceUri(project_id, instance_id);
      std::vector<std::string> successful_statements(
          statements.begin(), statements.begin() + num_succesful_statements);
      ms->UpdateDdl(instance_uri, std::string(database_id),
                    successful_statements, request->proto_descriptors(),
                    absl::FormatTime(absl::RFC3339_full, commit_timestamp,
                                     absl::UTCTimeZone()));
      auto counters = backend_database->GetIdCounterValues();
      ms->UpdateIdCounters(instance_uri, std::string(database_id),
                           frontend::MetadataStore::IdCounters{
                               .table_id = counters.table_id,
                               .column_id = counters.column_id,
                               .change_stream_id = counters.change_stream_id,
                           });
      ms->RemovePendingDdlOperation(request->database());
      ms->SetPendingOperation(*response);
      absl::Status save_status = ms->Save();
      if (!save_status.ok()) {
        // Keep the schema lock until the database is quarantined. No request
        // may observe or mutate a state that exists only in live LevelDB.
        backend_database->MarkRestoreRequired();
        ABSL_LOG(WARNING) << "Quarantined committed DDL pending restart "
                            "recovery for "
                          << response->name() << ": " << save_status;
        return absl::OkStatus();
      }
      durable_promotion_completed = true;
      return absl::OkStatus();
    };
  }
  absl::Status update_status =
      ms == nullptr
          ? backend_database->UpdateSchema(
                schema_change_operation, &num_succesful_statements,
                &commit_timestamp, &backfill_status)
          : backend_database->UpdateSchemaWithRollbackCheckpoint(
                schema_change_operation, rollback_checkpoint_directory,
                publish_ddl_intent, schema_change_applied,
                &num_succesful_statements, &commit_timestamp,
                &backfill_status);
  if (!update_status.ok()) {
    if (ms != nullptr) {
      ms->RemovePendingDdlOperation(request->database());
      if (ddl_intent_published) {
        absl::Status cleanup_status = ms->Save();
        if (!cleanup_status.ok() &&
            cleanup_status.code() != absl::StatusCode::kDataLoss) {
          ctx->env()->operation_manager()->DeleteOperation(response->name())
              .IgnoreError();
          return absl::DataLossError(absl::StrCat(
              update_status.message(),
              "; failed to clear durable DDL intent: ",
              cleanup_status.message()));
        }
        if (!cleanup_status.ok()) {
          ABSL_LOG(WARNING) << "DDL intent removal was published but not fully "
                              "synchronized for "
                            << response->name() << ": " << cleanup_status;
        }
      }
    }
    ctx->env()->operation_manager()->DeleteOperation(response->name())
        .IgnoreError();
    if (ms != nullptr) {
      GOOGLESQL_RETURN_IF_ERROR(
          DatabaseManager::RemoveDdlRollbackCheckpoints(
              config::data_dir(), request->database()));
    }
    return update_status;
  }

  if (ms == nullptr) {
    GOOGLESQL_RETURN_IF_ERROR(populate_operation());
    return absl::OkStatus();
  }

  if (!durable_promotion_completed) {
    return absl::OkStatus();
  }

  absl::Status checkpoint_cleanup =
      DatabaseManager::RemoveDdlRollbackCheckpoints(
          config::data_dir(), request->database());
  if (!checkpoint_cleanup.ok()) {
    ABSL_LOG(WARNING) << "Committed DDL but failed to remove rollback "
                         "checkpoint for "
                      << response->name() << ": " << checkpoint_cleanup;
  }
  absl::Status operation_status =
      ctx->env()->backup_catalog()->SaveOperation(*response);
  if (!operation_status.ok()) {
    // The metadata journal is already durable and startup will retry the
    // idempotent operation promotion. The schema mutation itself has committed.
    ABSL_LOG(WARNING) << "Deferred DDL operation promotion "
                      << response->name() << ": " << operation_status;
    return absl::OkStatus();
  }
  ms->RemovePendingOperation(response->name());
  absl::Status journal_status = ms->Save();
  if (!journal_status.ok()) {
    ABSL_LOG(WARNING) << "Failed to clear promoted DDL operation journal "
                      << response->name() << ": " << journal_status;
  }

  return absl::OkStatus();
}
REGISTER_GRPC_HANDLER(DatabaseAdmin, UpdateDatabaseDdl);

// Drops (aka deletes) a database.
absl::Status DropDatabase(RequestContext* ctx,
                          const database_api::DropDatabaseRequest* request,
                          protobuf_api::Empty* response) {
  // Validate the request.
  absl::string_view project_id, instance_id, database_id;
  GOOGLESQL_RETURN_IF_ERROR(ParseDatabaseUri(request->database(), &project_id,
                                             &instance_id, &database_id));
  if (MakeDatabaseUri(MakeInstanceUri(project_id, instance_id), database_id) !=
      request->database()) {
    return absl::InvalidArgumentError("Database name must be canonical");
  }
  absl::MutexLock admin_transaction_lock(
      &ctx->env()->admin_transaction_mutex());
  GOOGLESQL_ASSIGN_OR_RETURN(
      std::shared_ptr<Instance> instance,
      GetInstance(ctx, MakeInstanceUri(project_id, instance_id)));
  if (!ctx->env()
           ->backup_catalog()
           ->ListBackupSchedules(request->database())
           .empty()) {
    return absl::FailedPreconditionError(
        "Database still has backup schedules");
  }

  // Clean up resources associated with the database.
  auto maybe_database = ctx->env()
                            ->database_manager()
                            ->GetDatabaseIncludingRecoveryRequired(
                                request->database());
  if (maybe_database.ok() && (*maybe_database)->enable_drop_protection()) {
    return absl::FailedPreconditionError(absl::StrCat(
        "Database has drop protection enabled: ", request->database()));
  }
  if (maybe_database.ok()) {
    GOOGLESQL_ASSIGN_OR_RETURN(
        std::vector<std::shared_ptr<Session>> sessions,
        ctx->env()->session_manager()->ListSessions(
            request->database(), /*include_multiplex_sessions=*/true));
    for (const auto& session : sessions) {
      GOOGLESQL_RETURN_IF_ERROR(ctx->env()->session_manager()->DeleteSession(
          session->session_uri(), /*delete_multiplex_sessions=*/true));
    }
  }
  bool deletion_marked = false;
  if (maybe_database.ok() && ctx->env()->metadata_store() != nullptr) {
    GOOGLESQL_RETURN_IF_ERROR(DatabaseManager::MarkDatabaseForDeletion(
        config::data_dir(), request->database()));
    deletion_marked = true;
  }

  // Persist deletion before publishing it to the in-memory managers. If the
  // atomic save fails, reload the last durable snapshot so a later unrelated
  // save cannot accidentally commit the failed deletion.
  MetadataStore* metadata = ctx->env()->metadata_store();
  if (metadata != nullptr) {
    metadata->RemoveDatabase(MakeInstanceUri(project_id, instance_id),
                             std::string(database_id));
    absl::Status save_status = metadata->Save();
    if (!save_status.ok()) {
      absl::Status reload_status = metadata->Load();
      absl::Status marker_status = absl::OkStatus();
      if (deletion_marked) {
        marker_status = DatabaseManager::CancelDatabaseDeletion(
            config::data_dir(), request->database());
      }
      if (!reload_status.ok() || !marker_status.ok()) {
        return absl::DataLossError(absl::StrCat(
            save_status.message(),
            reload_status.ok()
                ? ""
                : absl::StrCat("; failed to restore metadata snapshot: ",
                               reload_status.message()),
            marker_status.ok()
                ? ""
                : absl::StrCat("; failed to cancel database deletion: ",
                               marker_status.message())));
      }
      return save_status;
    }
  }

  GOOGLESQL_RETURN_IF_ERROR(
      ctx->env()->database_manager()->DeleteDatabase(request->database()));
  ctx->env()->RemoveIamPolicies(request->database());
  if (maybe_database.ok()) {
    (*maybe_database).reset();
  }
  GOOGLESQL_RETURN_IF_ERROR(
      backend::Database::DeletePersistentStorageDirectory(
          config::data_dir(), request->database()));

  return absl::OkStatus();
}
REGISTER_GRPC_HANDLER(DatabaseAdmin, DropDatabase);

// Returns the schema of a database as a list of formatted DDL statements.
absl::Status GetDatabaseDdl(RequestContext* ctx,
                            const database_api::GetDatabaseDdlRequest* request,
                            database_api::GetDatabaseDdlResponse* response) {
  GOOGLESQL_ASSIGN_OR_RETURN(std::shared_ptr<Database> database,
                             GetDatabase(ctx, request->database()));

  auto latest_schema = database->backend()->GetLatestSchema();
  GOOGLESQL_ASSIGN_OR_RETURN(std::vector<std::string> printed_statements,
                             backend::PrintDDLStatements(latest_schema));
  for (const auto& statement : printed_statements) {
    response->add_statements(statement);
  }
  GOOGLESQL_ASSIGN_OR_RETURN(
      std::string proto_descriptor_bytes,
      latest_schema->proto_bundle()->GetProtoDescriptorBytes());
  response->set_proto_descriptors(proto_descriptor_bytes);
  return absl::OkStatus();
}
REGISTER_GRPC_HANDLER(DatabaseAdmin, GetDatabaseDdl);

}  // namespace frontend
}  // namespace emulator
}  // namespace spanner
}  // namespace google

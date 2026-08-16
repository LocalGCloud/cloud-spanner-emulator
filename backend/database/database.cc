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

#include "backend/database/database.h"

#include <filesystem>
#include <memory>
#include <thread>  // NOLINT
#include <system_error>
#include <utility>

#include "absl/functional/bind_front.h"
#include "absl/memory/memory.h"
#include "absl/status/status.h"
#include "absl/status/statusor.h"
#include "absl/strings/str_cat.h"
#include "absl/strings/string_view.h"
#include "absl/time/clock.h"
#include "absl/time/time.h"
#include "absl/types/variant.h"
#include "backend/actions/manager.h"
#include "backend/common/ids.h"
#include "backend/database/change_stream/change_stream_partition_churner.h"
#include "backend/database/pg_oid_assigner/pg_oid_assigner.h"
#include "backend/locking/manager.h"
#include "backend/query/query_engine.h"
#include "backend/schema/catalog/proto_bundle.h"
#include "backend/schema/catalog/schema.h"
#include "backend/schema/catalog/versioned_catalog.h"
#include "backend/schema/graph/schema_graph.h"
#include "backend/schema/updater/schema_updater.h"
#include "backend/schema/updater/scoped_schema_change_lock.h"
#include "backend/storage/in_memory_storage.h"
#include "backend/storage/persistent_storage.h"
#include "backend/transaction/options.h"
#include "backend/transaction/read_only_transaction.h"
#include "backend/transaction/read_write_transaction.h"
#include "common/clock.h"
#include "common/config.h"
#include "common/errors.h"
#include "google/spanner/admin/database/v1/common.pb.h"
#include "googlesql/base/status_macros.h"
#include "googlesql/public/types/type_factory.h"

namespace google {
namespace spanner {
namespace emulator {
namespace backend {

// TransactionIDGenerator is initialized to 1 because 0 is used as a sentinel
// value for an invalid transaction.
Database::Database()
    : transaction_id_generator_(absl::ToUnixMicros(absl::Now())) {}

absl::StatusOr<std::unique_ptr<Database>> Database::Create(
    Clock* clock, std::string_view database_id,
    const SchemaChangeOperation& schema_change_operation) {
  return Create(clock, database_id, schema_change_operation, IdCounterValues{});
}

absl::StatusOr<std::unique_ptr<Database>> Database::Create(
    Clock* clock, std::string_view database_id,
    const SchemaChangeOperation& schema_change_operation,
    const IdCounterValues& id_counters) {
  return Create(clock, database_id, schema_change_operation, id_counters,
                database_id);
}

absl::StatusOr<std::string> Database::PersistentStorageDirectory(
    std::string_view data_dir, std::string_view storage_namespace) {
  const std::filesystem::path namespace_path{std::string(storage_namespace)};
  if (namespace_path.empty() || namespace_path.is_absolute() ||
      namespace_path.lexically_normal() != namespace_path) {
    return absl::InvalidArgumentError(
        absl::StrCat("Invalid persistent storage namespace: ",
                     storage_namespace));
  }
  std::filesystem::path current{std::string(data_dir)};
  for (const auto& component : namespace_path) {
    if (component == "." || component == "..") {
      return absl::InvalidArgumentError(
          absl::StrCat("Invalid persistent storage namespace: ",
                       storage_namespace));
    }
    current /= component;
    std::error_code error;
    const std::filesystem::file_status status =
        std::filesystem::symlink_status(current, error);
    if (error == std::errc::no_such_file_or_directory) {
      error.clear();
    } else if (error) {
      return absl::InternalError(absl::StrCat(
          "Failed to inspect persistent database path ", current.string(),
          ": ", error.message()));
    }
    if (std::filesystem::exists(status) &&
        std::filesystem::is_symlink(status)) {
      return absl::FailedPreconditionError(absl::StrCat(
          "Persistent database path contains a symbolic link: ",
          current.string()));
    }
  }
  return (std::filesystem::path(std::string(data_dir)) / namespace_path /
          "storage")
      .string();
}

absl::Status Database::DeletePersistentStorageDirectory(
    std::string_view data_dir, std::string_view storage_namespace) {
  if (data_dir.empty()) return absl::OkStatus();
  GOOGLESQL_ASSIGN_OR_RETURN(
      const std::string storage_directory,
      PersistentStorageDirectory(data_dir, storage_namespace));
  const std::filesystem::path database_root =
      std::filesystem::path(storage_directory).parent_path();
  std::error_code error;
  std::filesystem::remove_all(database_root, error);
  if (error) {
    return absl::InternalError(
        absl::StrCat("Failed to remove persistent database directory ",
                     database_root.string(), ": ", error.message()));
  }
  return absl::OkStatus();
}

absl::StatusOr<std::unique_ptr<Database>> Database::Create(
    Clock* clock, std::string_view database_id,
    const SchemaChangeOperation& schema_change_operation,
    const IdCounterValues& id_counters,
    std::string_view storage_namespace) {
  auto database = absl::WrapUnique(new Database());
  database->clock_ = clock;
  database->database_id_ = database_id;

  std::string data_dir = config::data_dir();
  if (!data_dir.empty()) {
    // Use persistent storage with a caller-selected per-database namespace.
    GOOGLESQL_ASSIGN_OR_RETURN(
        const std::string storage_directory,
        PersistentStorageDirectory(data_dir, storage_namespace));
    auto persistent_storage = PersistentStorage::Create(storage_directory);
    if (!persistent_storage.ok()) {
      return persistent_storage.status();
    }
    database->storage_ = std::move(*persistent_storage);
  } else {
    database->storage_ = std::make_unique<InMemoryStorage>();
  }
  database->lock_manager_ = std::make_unique<LockManager>(clock);
  database->type_factory_ = std::make_unique<googlesql::TypeFactory>();
  database->action_manager_ = std::make_unique<ActionManager>();
  database->dialect_ = schema_change_operation.database_dialect;
  database->pg_oid_assigner_ = std::make_unique<PgOidAssigner>(
      schema_change_operation.database_dialect ==
      database_api::DatabaseDialect::POSTGRESQL);

  if (schema_change_operation.statements.empty()) {
    if (database->dialect_ == database_api::DatabaseDialect::POSTGRESQL) {
      // Create an empty schema with the dialect set.
      database->versioned_catalog_ =
          std::make_unique<VersionedCatalog>(std::make_unique<const Schema>(
              SchemaGraph::CreateEmpty(), ProtoBundle::CreateEmpty(),
              database->dialect_, database_id));
    } else {
      database->versioned_catalog_ = std::make_unique<VersionedCatalog>();
    }
  } else {
    auto context = database->GetSchemaChangeContext();
    if (schema_change_operation.schema_change_timestamp !=
        absl::InfinitePast()) {
      context.schema_change_timestamp =
          schema_change_operation.schema_change_timestamp;
    }
    SchemaUpdater updater;
    GOOGLESQL_ASSIGN_OR_RETURN(
        std::unique_ptr<const Schema> schema,
        updater.CreateSchemaFromDDL(schema_change_operation, context));
    database->versioned_catalog_ =
        std::make_unique<VersionedCatalog>(std::move(schema));
  }

  // Replaying the persisted DDL must start from zero so schema object IDs
  // match the keys already stored in LevelDB. Afterwards advance generators
  // to the persisted high-water marks, which may include dropped objects that
  // no longer appear in the current DDL.
  if (id_counters.table_id > database->table_id_generator_.current_value()) {
    database->table_id_generator_.Seed(id_counters.table_id);
  }
  if (id_counters.column_id > database->column_id_generator_.current_value()) {
    database->column_id_generator_.Seed(id_counters.column_id);
  }
  if (id_counters.change_stream_id >
      database->change_stream_id_generator_.current_value()) {
    database->change_stream_id_generator_.Seed(id_counters.change_stream_id);
  }

  database->query_engine_ = std::make_unique<QueryEngine>(
      database->type_factory_.get(),
      database->versioned_catalog_->GetLatestSchema());

  database->action_manager_->AddActionsForSchema(
      database->versioned_catalog_->GetLatestSchema(),
      database->query_engine_->function_catalog(),
      database->query_engine_->type_factory());

  database->change_stream_partition_churner_ =
      std::make_unique<ChangeStreamPartitionChurner>(
          absl::bind_front(&Database::CreateReadWriteTransaction,
                           database.get()),
          database->clock_);

  database->change_stream_partition_churner_->Update(
      database->versioned_catalog_->GetLatestSchema());

  // Some functions need to access the schema (e.g. sequence functions), so
  // set the latest schema to the function catalog here.
  database->query_engine_->SetLatestSchemaForFunctionCatalog(
      database->versioned_catalog_->GetLatestSchema());

  database->storage_->SetVersionRetentionPeriod(
      database->versioned_catalog_->version_retention_period());

  return database;
}

absl::StatusOr<std::unique_ptr<Database>> Database::Create(
    Clock* clock, std::string_view database_id,
    const std::vector<SchemaChangeOperation>& schema_change_operations,
    const IdCounterValues& id_counters,
    std::string_view storage_namespace) {
  const SchemaChangeOperation empty_operation;
  const SchemaChangeOperation& initial_operation =
      schema_change_operations.empty() ? empty_operation
                                       : schema_change_operations.front();
  GOOGLESQL_ASSIGN_OR_RETURN(
      std::unique_ptr<Database> database,
      Create(clock, database_id, initial_operation, IdCounterValues{},
             storage_namespace));
  for (std::size_t index = 1; index < schema_change_operations.size();
       ++index) {
    const SchemaChangeOperation& operation = schema_change_operations[index];
    if (operation.statements.empty()) continue;
    int successful_statements = 0;
    absl::Time commit_timestamp;
    absl::Status backfill_status;
    GOOGLESQL_RETURN_IF_ERROR(database->UpdateSchema(
        operation, &successful_statements, &commit_timestamp,
        &backfill_status));
    if (successful_statements != operation.statements.size()) {
      return absl::DataLossError(
          "Persisted schema-change batch was only partially replayed");
    }
    GOOGLESQL_RETURN_IF_ERROR(backfill_status);
  }
  if (id_counters.table_id > database->table_id_generator_.current_value()) {
    database->table_id_generator_.Seed(id_counters.table_id);
  }
  if (id_counters.column_id >
      database->column_id_generator_.current_value()) {
    database->column_id_generator_.Seed(id_counters.column_id);
  }
  if (id_counters.change_stream_id >
      database->change_stream_id_generator_.current_value()) {
    database->change_stream_id_generator_.Seed(id_counters.change_stream_id);
  }
  return database;
}
absl::StatusOr<std::unique_ptr<ReadOnlyTransaction>>
Database::CreateReadOnlyTransaction(const ReadOnlyOptions& options) {
  if (restore_required()) {
    return absl::FailedPreconditionError(
        "Database recovery is required before serving transactions");
  }
  return std::make_unique<ReadOnlyTransaction>(
      options, transaction_id_generator_.NextId(), clock_, storage_.get(),
      lock_manager_.get(), versioned_catalog_.get(), &restore_required_);
}

absl::StatusOr<std::unique_ptr<ReadWriteTransaction>>
Database::CreateReadWriteTransaction(const ReadWriteOptions& options,
                                     const RetryState& retry_state) {
  if (restore_required()) {
    return absl::FailedPreconditionError(
        "Database recovery is required before serving transactions");
  }
  return std::make_unique<ReadWriteTransaction>(
      options, retry_state, transaction_id_generator_.NextId(), clock_,
      storage_.get(), lock_manager_.get(), versioned_catalog_.get(),
      action_manager_.get(), &restore_required_);
}

void Database::MarkRestoreRequired() {
  restore_required_.store(true, std::memory_order_release);
}

bool Database::restore_required() const {
  return restore_required_.load(std::memory_order_acquire);
}

SchemaChangeContext Database::GetSchemaChangeContext() {
  return SchemaChangeContext{
      .type_factory = type_factory_.get(),
      .table_id_generator = &table_id_generator_,
      .column_id_generator = &column_id_generator_,
      .storage = storage_.get(),
      .pg_oid_assigner = pg_oid_assigner_.get(),
      .database_id = database_id_,
  };
}

absl::Status Database::UpdateSchema(
    const SchemaChangeOperation& schema_change_operation,
    int* num_succesful_statements, absl::Time* commit_timestamp,
    absl::Status* backfill_status) {
  return UpdateSchemaInternal(schema_change_operation, nullptr, nullptr,
                              nullptr, num_succesful_statements,
                              commit_timestamp, backfill_status);
}

absl::Status Database::UpdateSchemaWithRollbackCheckpoint(
    const SchemaChangeOperation& schema_change_operation,
    const std::string& rollback_directory,
    const std::function<absl::Status()>& rollback_checkpoint_ready,
    const std::function<absl::Status()>& schema_change_applied,
    int* num_succesful_statements, absl::Time* commit_timestamp,
    absl::Status* backfill_status) {
  return UpdateSchemaInternal(
      schema_change_operation, &rollback_directory,
      &rollback_checkpoint_ready, &schema_change_applied,
      num_succesful_statements, commit_timestamp, backfill_status);
}

absl::Status Database::UpdateSchemaInternal(
    const SchemaChangeOperation& schema_change_operation,
    const std::string* rollback_directory,
    const std::function<absl::Status()>* rollback_checkpoint_ready,
    const std::function<absl::Status()>* schema_change_applied,
    int* num_succesful_statements, absl::Time* commit_timestamp,
    absl::Status* backfill_status) {
  if (restore_required()) {
    return absl::FailedPreconditionError(
        "Database recovery is required before applying schema changes");
  }
  if (schema_change_operation.statements.empty()) {
    return error::UpdateDatabaseMissingStatements();
  }

  // Make an exclusive lock request for the database. If there are any
  // concurrent transactions it will be denied and the operation aborted.
  ScopedSchemaChangeLock lock{transaction_id_generator_.NextId(),
                              lock_manager_.get()};
  GOOGLESQL_RETURN_IF_ERROR(lock.Wait());
  if (rollback_directory != nullptr) {
    auto checkpoint = CreateBackupCheckpoint(*rollback_directory);
    if (!checkpoint.ok()) return checkpoint.status();
    GOOGLESQL_RETURN_IF_ERROR((*rollback_checkpoint_ready)());
  }

  // Reserve a commit timestamp for the schema changes. Even if the
  // schema change fails, it will result in a no-op commit that will
  // be invisible to other read-only/read-write transactions.
  GOOGLESQL_ASSIGN_OR_RETURN(auto update_timestamp,
                             lock.ReserveCommitTimestamp());
  // Recovery replay carries the originally committed timestamps so
  // time-based schema state (for example change stream creation times)
  // comes back exactly as it was before the restart.
  if (schema_change_operation.schema_change_timestamp !=
      absl::InfinitePast()) {
    update_timestamp = schema_change_operation.schema_change_timestamp;
  }

  auto context = GetSchemaChangeContext();
  context.schema_change_timestamp = update_timestamp;
  const Schema* existing_schema = versioned_catalog_->GetLatestSchema();
  SchemaUpdater updater;
  GOOGLESQL_ASSIGN_OR_RETURN(
      auto result, updater.UpdateSchemaFromDDL(
                       existing_schema, schema_change_operation, context));
  *commit_timestamp = update_timestamp;
  *num_succesful_statements = result.num_successful_input_statements;
  *backfill_status = result.backfill_status;

  // We update the schema even if the backfill status was not OK, the returned
  // schema will be the schema for the last valid statement before the statement
  // for which the backfill/verification failed.
  if (result.updated_schema != nullptr) {
    GOOGLESQL_RETURN_IF_ERROR(versioned_catalog_->AddSchema(
        update_timestamp, std::move(result.updated_schema)));
    action_manager_->AddActionsForSchema(versioned_catalog_->GetLatestSchema(),
                                         query_engine_->function_catalog(),
                                         query_engine_->type_factory());
  }
  change_stream_partition_churner_->Update(
      versioned_catalog_->GetLatestSchema());

  // Some functions need to access the schema (e.g. sequence functions), so
  // set the latest schema to the function catalog here.
  query_engine_->SetLatestSchemaForFunctionCatalog(
      versioned_catalog_->GetLatestSchema());

  storage_->SetVersionRetentionPeriod(
      versioned_catalog_->version_retention_period());

  // Enforce the retention period.
  storage_->CleanUpDeletedTables(update_timestamp);
  storage_->CleanUpDeletedColumns(update_timestamp);
  versioned_catalog_->RemoveExpiredSchemas(update_timestamp);
  if (schema_change_applied != nullptr) {
    GOOGLESQL_RETURN_IF_ERROR((*schema_change_applied)());
  }

  return absl::OkStatus();
}

const Schema* Database::GetLatestSchema() const {
  return versioned_catalog_->GetLatestSchema();
}

absl::StatusOr<absl::Time> Database::CreateBackupCheckpoint(
    const std::string& output_dir) const {
  const auto* persistent_storage =
      dynamic_cast<const PersistentStorage*>(storage_.get());
  if (persistent_storage == nullptr) {
    return absl::FailedPreconditionError(
        "Native backups require --data_dir persistent storage");
  }
  return lock_manager_->RunWithCommitSerialization(
      [&] { return persistent_storage->CreateCheckpoint(output_dir); });
}

Database::IdCounterValues Database::GetIdCounterValues() const {
  return IdCounterValues{
      .table_id = table_id_generator_.current_value(),
      .column_id = column_id_generator_.current_value(),
      .change_stream_id = change_stream_id_generator_.current_value(),
  };
}

}  // namespace backend
}  // namespace emulator
}  // namespace spanner
}  // namespace google

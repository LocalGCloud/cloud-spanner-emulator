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

#include "frontend/entities/transaction.h"

#include <cstddef>
#include <cstdint>
#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <utility>
#include <variant>
#include <vector>

#include "google/spanner/v1/spanner.pb.h"
#include "googlesql/public/value.h"
#include "absl/container/flat_hash_set.h"
#include "absl/status/status.h"
#include "absl/status/statusor.h"
#include "absl/synchronization/mutex.h"
#include "absl/time/clock.h"
#include "absl/time/time.h"
#include "absl/types/variant.h"
#include "backend/access/read.h"
#include "backend/access/write.h"
#include "backend/common/ids.h"
#include "backend/common/variant.h"
#include "backend/database/database.h"
#include "backend/query/query_context.h"
#include "backend/query/query_engine.h"
#include "backend/schema/catalog/access_policy.h"
#include "backend/schema/catalog/change_stream.h"
#include "backend/schema/catalog/column.h"
#include "backend/schema/catalog/schema.h"
#include "backend/schema/catalog/table.h"
#include "backend/schema/ddl/operations.pb.h"
#include "backend/transaction/options.h"
#include "backend/transaction/read_only_transaction.h"
#include "backend/transaction/read_write_transaction.h"
#include "common/config.h"
#include "common/constants.h"
#include "common/errors.h"
#include "frontend/converters/time.h"
#include "frontend/converters/types.h"
#include "frontend/converters/values.h"
#include "frontend/entities/database.h"
#include "googlesql/base/status_macros.h"
#include "googlesql/base/ret_check.h"
#include "absl/status/status.h"
#include "googlesql/base/time_proto_util.h"

namespace google {
namespace spanner {
namespace emulator {
namespace frontend {

namespace spanner_api = ::google::spanner::v1;

namespace {

Transaction::Type TypeFromTransactionOptions(
    const spanner_api::TransactionOptions& options) {
  switch (options.mode_case()) {
    case v1::TransactionOptions::kReadWrite: {
      return Transaction::Type::kReadWrite;
    }
    case v1::TransactionOptions::kReadOnly: {
      return Transaction::Type::kReadOnly;
    }
    case v1::TransactionOptions::kPartitionedDml: {
      return Transaction::Type::kPartitionedDml;
    }
    case v1::TransactionOptions::MODE_NOT_SET: {
      return Transaction::Type::kReadOnly;
    }
  }
}

bool HasPayload(const absl::Status& status, const std::string& url) {
  return status.GetPayload(url).has_value();
}

// A cursor that counts the rows and bytes that a read returns and records the
// read in the SPANNER_SYS statistics when it is destroyed.
class ReadStatsCursor : public backend::RowCursor {
 public:
  ReadStatsCursor(std::unique_ptr<backend::RowCursor> cursor,
                  backend::SystemStatsCollector* stats_collector,
                  backend::ReadExecution read, absl::Duration start_cpu_time)
      : cursor_(std::move(cursor)),
        stats_collector_(stats_collector),
        read_(std::move(read)),
        start_cpu_time_(start_cpu_time) {}

  ~ReadStatsCursor() override {
    read_.cpu_time = backend::ThreadCpuTime() - start_cpu_time_;
    stats_collector_->RecordRead(absl::Now(), read_);
  }

  bool Next() override {
    if (!cursor_->Next()) {
      return false;
    }
    ++read_.rows;
    for (int i = 0; i < cursor_->NumColumns(); ++i) {
      read_.bytes += backend::LogicalByteSize(cursor_->ColumnValue(i));
    }
    return true;
  }
  absl::Status Status() const override { return cursor_->Status(); }
  int NumColumns() const override { return cursor_->NumColumns(); }
  const std::string ColumnName(int i) const override {
    return cursor_->ColumnName(i);
  }
  const googlesql::Value ColumnValue(int i) const override {
    return cursor_->ColumnValue(i);
  }
  const googlesql::Type* ColumnType(int i) const override {
    return cursor_->ColumnType(i);
  }

 private:
  std::unique_ptr<backend::RowCursor> cursor_;
  backend::SystemStatsCollector* stats_collector_;
  backend::ReadExecution read_;
  absl::Duration start_cpu_time_;
};

backend::TransactionAttempt::ConcurrencyMode ConcurrencyMode(
    const spanner_api::TransactionOptions& options) {
  const bool optimistic =
      options.read_write().read_lock_mode() ==
      spanner_api::TransactionOptions::ReadWrite::OPTIMISTIC;
  const bool pessimistic =
      options.read_write().read_lock_mode() ==
      spanner_api::TransactionOptions::ReadWrite::PESSIMISTIC;
  if (options.isolation_level() ==
      spanner_api::TransactionOptions::REPEATABLE_READ) {
    return pessimistic ? backend::TransactionAttempt::ConcurrencyMode::
                             kRepeatableReadPessimistic
                       : backend::TransactionAttempt::ConcurrencyMode::
                             kRepeatableReadOptimistic;
  }
  return optimistic
             ? backend::TransactionAttempt::ConcurrencyMode::
                   kSerializableOptimistic
             : backend::TransactionAttempt::ConcurrencyMode::
                   kSerializablePessimistic;
}

// Checks that a role may read the requested columns of a table.
absl::Status CheckReadAccess(const backend::AccessPolicy& access,
                             const backend::Schema* schema,
                             const backend::ReadArg& read_arg) {
  // Change stream queries read internal tables, which roles do not govern.
  if (!read_arg.change_stream_for_partition_table.empty() ||
      !read_arg.change_stream_for_data_table.empty()) {
    return absl::OkStatus();
  }
  // Reads of unknown tables and columns report their own errors.
  const backend::Table* table = schema->FindTable(read_arg.table);
  if (table == nullptr) {
    return absl::OkStatus();
  }
  GOOGLESQL_RETURN_IF_ERROR(access.CheckSchemaUsage(table->Name()));
  for (const std::string& column_name : read_arg.columns) {
    const backend::Column* column = table->FindColumn(column_name);
    if (column != nullptr &&
        !access.Has(backend::ddl::Privilege::SELECT, table, column)) {
      return access.PrivilegeError("table", table->Name());
    }
  }
  return absl::OkStatus();
}

// Checks that a role may apply the operations of a mutation. Inserts need
// INSERT and updates need UPDATE on the written columns. Deletes need DELETE on
// the table.
absl::Status CheckWriteAccess(const backend::AccessPolicy& access,
                              const backend::Schema* schema,
                              const backend::Mutation& mutation) {
  for (const backend::MutationOp& op : mutation.ops()) {
    const backend::Table* table = schema->FindTable(op.table);
    if (table == nullptr) {
      continue;
    }
    GOOGLESQL_RETURN_IF_ERROR(access.CheckSchemaUsage(table->Name()));
    auto has_on_columns = [&](backend::ddl::Privilege::Type type) {
      for (const std::string& column_name : op.columns) {
        const backend::Column* column = table->FindColumn(column_name);
        if (column != nullptr && !access.Has(type, table, column)) {
          return false;
        }
      }
      return true;
    };
    bool allowed = false;
    switch (op.type) {
      case backend::MutationOpType::kInsert:
        allowed = has_on_columns(backend::ddl::Privilege::INSERT);
        break;
      case backend::MutationOpType::kUpdate:
        allowed = has_on_columns(backend::ddl::Privilege::UPDATE);
        break;
      case backend::MutationOpType::kInsertOrUpdate:
        allowed = has_on_columns(backend::ddl::Privilege::INSERT) &&
                  has_on_columns(backend::ddl::Privilege::UPDATE);
        break;
      case backend::MutationOpType::kReplace:
        allowed = has_on_columns(backend::ddl::Privilege::INSERT) &&
                  access.Has(backend::ddl::Privilege::DELETE, table);
        break;
      case backend::MutationOpType::kDelete:
        allowed = access.Has(backend::ddl::Privilege::DELETE, table);
        break;
    }
    if (!allowed) {
      return access.PrivilegeError("table", table->Name());
    }
    // Inserting a row computes the default values of the columns that the
    // mutation does not write.
    if (op.type == backend::MutationOpType::kInsert ||
        op.type == backend::MutationOpType::kInsertOrUpdate ||
        op.type == backend::MutationOpType::kReplace) {
      absl::flat_hash_set<const backend::Column*> written_columns;
      for (const std::string& column_name : op.columns) {
        written_columns.insert(table->FindColumn(column_name));
      }
      std::vector<const backend::Column*> defaulted_columns;
      for (const backend::Column* column : table->columns()) {
        if (!written_columns.contains(column)) {
          defaulted_columns.push_back(column);
        }
      }
      GOOGLESQL_RETURN_IF_ERROR(
          access.CheckDefaultValueSequences(defaulted_columns));
    }
  }
  return absl::OkStatus();
}

}  // namespace

using ReadWriteTransactionPtr = std::unique_ptr<backend::ReadWriteTransaction>;
using ReadOnlyTransactionPtr = std::unique_ptr<backend::ReadOnlyTransaction>;

Transaction::Transaction(
    std::shared_ptr<Database> database_owner,
    std::variant<std::unique_ptr<backend::ReadWriteTransaction>,
                 std::unique_ptr<backend::ReadOnlyTransaction>>
        backend_transaction,
    const backend::QueryEngine* query_engine,
    const spanner_api::TransactionOptions& options, const Usage& usage,
    const std::string& creator_role)
    : database_owner_(std::move(database_owner)),
      transaction_(std::move(backend_transaction)),
      query_engine_(query_engine),
      usage_type_(usage),
      type_(TypeFromTransactionOptions(options)),
      options_(options),
      creator_role_(creator_role),
      create_time_(absl::Now()) {
  if (type_ == kReadWrite && stats_collector() != nullptr) {
    retry_ = read_write()->retry_state().abort_retry_count > 0;
    // Lock statistics sample only the lock requests of user transactions.
    stats_collector()->RegisterTransaction(id(), "");
  }
}

Transaction::~Transaction() {
  if (type_ == kReadWrite && stats_collector() != nullptr) {
    stats_collector()->UnregisterTransaction(id());
  }
}

backend::SystemStatsCollector* Transaction::stats_collector() const {
  return database_owner_ == nullptr
             ? nullptr
             : database_owner_->backend()->stats_collector();
}

void Transaction::SetTransactionTag(absl::string_view tag) {
  if ((type_ != kReadWrite && type_ != kPartitionedDml) || tag.empty()) {
    return;
  }
  absl::MutexLock lock(mu_);
  if (transaction_tag_.empty()) {
    transaction_tag_ = std::string(tag);
    if (stats_collector() != nullptr) {
      stats_collector()->RegisterTransaction(id(), transaction_tag_);
    }
    if (read_write() != nullptr) {
      read_write()->SetTransactionTag(transaction_tag_);
    }
  }
}

void Transaction::Close() {
  absl::MutexLock lock(mu_);
  closed_ = true;
  if (type_ == kReadWrite || type_ == kPartitionedDml) {
    read_write()->Rollback().IgnoreError();
  }
}

absl::StatusOr<spanner_api::Transaction> Transaction::ToProto() {
  spanner_api::Transaction txn;
  if (usage_type_ != kSingleUse) {
    *txn.mutable_id() = std::to_string(id());
  }
  if (options_.has_read_only() &&
      options_.read_only().return_read_timestamp()) {
    GOOGLESQL_ASSIGN_OR_RETURN(absl::Time read_timestamp, GetReadTimestamp());
    GOOGLESQL_ASSIGN_OR_RETURN(*txn.mutable_read_timestamp(),
                     TimestampToProto(read_timestamp));
  }
  return txn;
}

bool Transaction::IsClosed() const {
  absl::MutexLock lock(mu_);
  return closed_;
}

bool Transaction::HasState(
    const backend::ReadWriteTransaction::State& state) const {
  switch (type_) {
    case kReadOnly: {
      return false;
    }
    case kReadWrite:
    case kPartitionedDml: {
      return read_write()->state() == state;
    }
  }
}

bool Transaction::IsRolledback() const {
  mu_.AssertHeld();
  return HasState(backend::ReadWriteTransaction::State::kRolledback);
}

bool Transaction::IsInvalid() const {
  mu_.AssertHeld();
  return HasState(backend::ReadWriteTransaction::State::kInvalid);
}

bool Transaction::IsAborted() const {
  absl::MutexLock lock(mu_);
  return type_ == kReadWrite && status_.code() == absl::StatusCode::kAborted;
}

bool Transaction::IsCommitted() const {
  mu_.AssertHeld();
  return HasState(backend::ReadWriteTransaction::State::kCommitted);
}

const backend::Schema* Transaction::schema() const {
  switch (type_) {
    case kReadOnly: {
      return read_only()->schema();
    }
    case kReadWrite:
    case kPartitionedDml: {
      return read_write()->schema();
    }
  }
}

backend::TransactionID Transaction::id() const {
  switch (type_) {
    case kReadOnly: {
      return read_only()->id();
    }
    case kReadWrite:
    case kPartitionedDml: {
      return read_write()->id();
    }
  }
}

absl::StatusOr<std::optional<backend::AccessPolicy>>
Transaction::GetAccessPolicy() const {
  if (creator_role_.empty()) {
    return std::nullopt;
  }
  // The role may have been dropped since the session was created.
  return backend::AccessPolicy::Create(schema(), creator_role_);
}

absl::Status Transaction::CheckChangeStreamReadAccess(
    const std::string& change_stream_name) const {
  GOOGLESQL_ASSIGN_OR_RETURN(std::optional<backend::AccessPolicy> access,
                   GetAccessPolicy());
  const backend::ChangeStream* change_stream =
      schema()->FindChangeStream(change_stream_name);
  if (!access.has_value() || change_stream == nullptr) {
    return absl::OkStatus();
  }
  if (!access->Has(backend::ddl::Privilege::SELECT, change_stream)) {
    return access->PrivilegeError("change stream", change_stream->Name());
  }
  if (!access->Has(backend::ddl::Privilege::EXECUTE, change_stream)) {
    return access->PrivilegeError("table function", change_stream->tvf_name());
  }
  return absl::OkStatus();
}

absl::Status Transaction::Read(const backend::ReadArg& read_arg,
                               std::unique_ptr<backend::RowCursor>* cursor) {
  mu_.AssertHeld();
  GOOGLESQL_ASSIGN_OR_RETURN(std::optional<backend::AccessPolicy> access,
                   GetAccessPolicy());
  if (access.has_value()) {
    GOOGLESQL_RETURN_IF_ERROR(CheckReadAccess(*access, schema(), read_arg));
  }
  switch (type_) {
    case kReadOnly: {
      return read_only()->Read(read_arg, cursor);
    }
    case kReadWrite: {
      return read_write()->Read(read_arg, cursor);
    }
    case kPartitionedDml: {
      return error::InvalidOperationUsingPartitionedDmlTransaction();
    }
  }
}

absl::Status Transaction::Read(const backend::ReadArg& read_arg,
                               std::unique_ptr<backend::RowCursor>* cursor,
                               const RequestStatsInfo& stats_info) {
  mu_.AssertHeld();
  const absl::Duration start_cpu_time = backend::ThreadCpuTime();
  GOOGLESQL_RETURN_IF_ERROR(Read(read_arg, cursor));
  backend::SystemStatsCollector* collector = stats_collector();
  if (collector == nullptr) {
    return absl::OkStatus();
  }
  if (type_ == kReadWrite) {
    attempt_footprint_.AddRead(read_arg.table, read_arg.columns);
  }
  *cursor = std::make_unique<ReadStatsCursor>(
      std::move(*cursor), collector,
      backend::ReadExecution{
          .table = read_arg.table,
          .columns = read_arg.columns,
          .request_tag = stats_info.request_tag,
          .partitioned = stats_info.partitioned,
          .in_read_write_transaction = type_ == kReadWrite},
      start_cpu_time);
  return absl::OkStatus();
}

absl::StatusOr<backend::QueryResult> Transaction::ExecuteSql(
    const backend::Query& query) {
  return ExecuteSql(query, v1::ExecuteSqlRequest::NORMAL);
}

absl::StatusOr<backend::QueryResult> Transaction::ExecuteSql(
    const backend::Query& query, v1::ExecuteSqlRequest_QueryMode query_mode,
    const RequestStatsInfo& stats_info) {
  mu_.AssertHeld();
  backend::SystemStatsCollector* collector = stats_collector();
  if (collector == nullptr || query_mode == v1::ExecuteSqlRequest::PLAN) {
    return ExecuteSql(query, query_mode);
  }
  std::string transaction_type = "NONE";
  if (type_ == kReadWrite) {
    transaction_type = "READ_WRITE";
  } else if (type_ == kReadOnly && usage_type_ == kMultiUse) {
    transaction_type = "READ_ONLY";
  }
  const absl::Time start_time = absl::Now();
  const absl::Duration start_cpu_time = backend::ThreadCpuTime();
  const int64_t query_id = collector->StartQuery(
      {.text = query.sql,
       .request_tag = stats_info.request_tag,
       .session_id = stats_info.session_id,
       .priority = stats_info.priority,
       .transaction_type = transaction_type,
       .client_ip_address = stats_info.client_ip_address,
       .api_client_header = stats_info.api_client_header,
       .user_agent_header = stats_info.user_agent_header,
       .start_time = start_time});
  // SPANNER_SYS.ACTIVE_PARTITIONED_DMLS lists partitioned DMLs while they run.
  std::optional<int64_t> partitioned_dml_id;
  if (type_ == kPartitionedDml) {
    partitioned_dml_id = collector->StartPartitionedDml(
        {.text = query.sql,
         .session_id = stats_info.session_id,
         .start_time = start_time});
  }
  absl::StatusOr<backend::QueryResult> result = ExecuteSql(query, query_mode);
  if (partitioned_dml_id.has_value()) {
    collector->EndPartitionedDml(*partitioned_dml_id);
  }
  collector->EndQuery(query_id);

  backend::QueryExecution execution{
      .text = query.sql,
      .request_tag = stats_info.request_tag,
      .partitioned = stats_info.partitioned || type_ == kPartitionedDml,
      .in_read_write_transaction = type_ == kReadWrite,
      .status = result.status().code(),
      .latency = absl::Now() - start_time,
      .cpu_time = backend::ThreadCpuTime() - start_cpu_time};
  backend::AccessFootprint footprint;
  if (result.ok()) {
    execution.plan_creation_time = result->plan_creation_time;
    execution.rows_returned = result->num_output_rows;
    execution.bytes_returned = result->bytes_returned;
    execution.rows_scanned = result->rows_scanned;
    execution.rows_written = result->modified_row_count;
    execution.bytes_written = result->footprint.bytes_written;
    footprint = result->footprint;
    if (type_ == kReadWrite) {
      attempt_footprint_.Merge(footprint);
    }
  }
  collector->RecordQuery(absl::Now(), execution, footprint);
  return result;
}

absl::StatusOr<backend::QueryResult> Transaction::ExecuteSql(
    const backend::Query& query,
    const v1::ExecuteSqlRequest_QueryMode query_mode) {
  mu_.AssertHeld();
  GOOGLESQL_ASSIGN_OR_RETURN(std::optional<backend::AccessPolicy> access_policy,
                   GetAccessPolicy());
  const backend::AccessPolicy* access =
      access_policy.has_value() ? &*access_policy : nullptr;
  switch (type_) {
    case kReadOnly: {
      return query_engine_->ExecuteSql(
          query,
          backend::QueryContext{.schema = schema(),
                                .reader = read_only(),
                                .writer = nullptr,
                                .is_read_only_txn = true,
                                .access = access},
          query_mode);
    }
    case kReadWrite: {
      std::optional<backend::PlacementDmlRestrictions> placement_restrictions;
      if (config::enforce_placement_dml_restrictions()) {
        // An INSERT or DELETE on a placement table must be the only statement
        // in its transaction.
        if (placement_sole_statement_table_.has_value()) {
          return error::PlacementDmlMustBeOnlyStatement(
              *placement_sole_statement_table_);
        }
        placement_restrictions = backend::PlacementDmlRestrictions{
            .other_statements_in_transaction = executed_sql_statements_ > 0};
      }
      absl::StatusOr<backend::QueryResult> result = query_engine_->ExecuteSql(
          query,
          backend::QueryContext{
              .schema = schema(),
              .reader = read_write(),
              .writer = read_write(),
              .commit_timestamp_tracker =
                  read_write()->commit_timestamp_tracker(),
              .allow_read_write_only_functions = true,
              .is_read_only_txn = false,
              .placement_dml_restrictions = placement_restrictions,
              .access = access},
          query_mode);
      if (result.ok() && query_mode != v1::ExecuteSqlRequest::PLAN) {
        ++executed_sql_statements_;
        if (result->placement_sole_statement_table.has_value()) {
          placement_sole_statement_table_ =
              result->placement_sole_statement_table;
        }
      }
      return result;
    }
    case kPartitionedDml: {
      auto context = backend::QueryContext{
          .schema = schema(),
          .reader = read_write(),
          .writer = read_write(),
          .commit_timestamp_tracker = read_write()->commit_timestamp_tracker(),
          .allow_read_write_only_functions = true,
          .is_read_only_txn = false,
          .access = access};
      GOOGLESQL_RETURN_IF_ERROR(query_engine_->IsValidPartitionedDML(query, context));
      // PartitionedDml will auto-commit transactions and cannot be reused.
      GOOGLESQL_ASSIGN_OR_RETURN(backend::QueryResult result,
                       query_engine_->ExecuteSql(query, context, query_mode));
      GOOGLESQL_RETURN_IF_ERROR(read_write()->Commit());
      return result;
    }
  }
}

absl::Status Transaction::Write(const backend::Mutation& mutation) {
  mu_.AssertHeld();
  GOOGLESQL_ASSIGN_OR_RETURN(std::optional<backend::AccessPolicy> access,
                   GetAccessPolicy());
  if (access.has_value()) {
    GOOGLESQL_RETURN_IF_ERROR(CheckWriteAccess(*access, schema(), mutation));
  }
  if (type_ == kReadWrite) {
    if (backend::SystemStatsCollector* collector = stats_collector();
        collector != nullptr) {
      backend::AccessFootprint footprint;
      footprint.AddMutation(mutation);
      collector->RecordMutations(absl::Now(), footprint);
      attempt_footprint_.Merge(footprint);
    }
    return read_write()->Write(mutation);
  }
  return error::CannotCommitRollbackReadOnlyOrPartitionedDmlTransaction();
}

absl::Status Transaction::Commit() {
  mu_.AssertHeld();
  if (type_ == kReadWrite) {
    const absl::Time start_time = absl::Now();
    absl::Status status = read_write()->Commit();
    commit_latency_ = absl::Now() - start_time;
    return status;
  }
  return error::CannotCommitRollbackReadOnlyOrPartitionedDmlTransaction();
}

absl::Status Transaction::Invalidate() {
  mu_.AssertHeld();
  if (type_ == kReadWrite) {
    return read_write()->Invalidate();
  }
  return error::Internal("Read only transaction cannot be invalidated.");
}

absl::Status Transaction::Rollback() {
  mu_.AssertHeld();
  if (type_ == kReadWrite) {
    return read_write()->Rollback();
  }
  return error::CannotCommitRollbackReadOnlyOrPartitionedDmlTransaction();
}

absl::StatusOr<absl::Time> Transaction::GetReadTimestamp() const {
  if (type_ == kReadOnly) {
    return read_only()->read_timestamp();
  }
  return error::CannotReturnReadTimestampForReadWriteTransaction();
}

absl::StatusOr<absl::Time> Transaction::GetCommitTimestamp() const {
  mu_.AssertHeld();
  if (type_ == kReadWrite) {
    return read_write()->GetCommitTimestamp();
  }
  return error::CannotCommitRollbackReadOnlyOrPartitionedDmlTransaction();
}

absl::StatusOr<int64_t> Transaction::GetMutationCount() const {
  mu_.AssertHeld();
  if (type_ == kReadWrite) {
    return read_write()->GetMutationCount();
  }
  return error::CannotCommitRollbackReadOnlyOrPartitionedDmlTransaction();
}

absl::Status Transaction::Status() const {
  mu_.AssertHeld();

  return status_;
}

void Transaction::MaybeInvalidate(const absl::Status& status) {
  mu_.AssertHeld();

  if (HasPayload(status, kConstraintError)) {
    status_ = absl::Status(status.code(), status.message());
    Invalidate().IgnoreError();
  }
}

std::optional<Transaction::RequestReplayState>
Transaction::LookupOrRegisterDmlRequest(int64_t seqno, int64_t request_hash,
                                        const std::string& sql_statement) {
  mu_.AssertHeld();

  current_dml_seqno_ = seqno;
  const auto request = dml_requests_.find(seqno);
  if (request == dml_requests_.end()) {
    // If the request was not found, then it is a new request. Check to see that
    // it isn't out of order.
    if (!dml_requests_.empty() && seqno < dml_requests_.rbegin()->first) {
      Transaction::RequestReplayState state;
      state.status = error::DmlSequenceOutOfOrder(
          seqno, dml_requests_.rbegin()->first, sql_statement);
      // This is marked as a dml replay for error handling purposes. We do not
      // want this status to be recorded within SetDmlRequestReplayStatus.
      dml_error_mode_ = DMLErrorHandlingMode::kDmlRegistrationError;
      return state;
    }

    // Order was valid, so we record the new sequence number.
    dml_requests_.emplace(
        seqno, Transaction::RequestReplayState{.status = absl::OkStatus(),
                                               .request_hash = request_hash});
    dml_error_mode_ = DMLErrorHandlingMode::kDmlRequest;
    return std::nullopt;
  }

  // Request was found, check to see that the request hash matches.
  if (request_hash != request->second.request_hash) {
    Transaction::RequestReplayState state = request->second;
    state.status = error::ReplayRequestMismatch(seqno, sql_statement);
    dml_error_mode_ = DMLErrorHandlingMode::kDmlRegistrationError;
    return state;
  }

  // Return the saved status for this sequence.
  dml_error_mode_ = DMLErrorHandlingMode::kDmlReplay;
  return request->second;
}

void Transaction::SetDmlRequestReplayStatus(const absl::Status& status) {
  mu_.AssertHeld();

  // Ignore replays and registration errors.
  if (dml_error_mode_ == DMLErrorHandlingMode::kDmlReplay ||
      dml_error_mode_ == DMLErrorHandlingMode::kDmlRegistrationError) {
    return;
  }
  const auto request = dml_requests_.find(current_dml_seqno_);
  ABSL_DCHECK(request != dml_requests_.end());
  if (request != dml_requests_.end()) {
    request->second.status = status;
  }
}

void Transaction::SetDmlReplayOutcome(
    std::variant<spanner_api::ResultSet, spanner_api::ExecuteBatchDmlResponse>
        outcome) {
  mu_.AssertHeld();

  // Ignore invalid transactions.
  if (IsInvalid()) {
    return;
  }
  const auto request = dml_requests_.find(current_dml_seqno_);
  ABSL_DCHECK(request != dml_requests_.end())
      << "DML sequence number was not registered.";
  if (request != dml_requests_.end()) {
    request->second.outcome = outcome;
  }
}

Transaction::DMLErrorHandlingMode Transaction::DMLErrorType() const {
  return dml_error_mode_;
}

absl::Status Transaction::GuardedCall(OpType op,
                                      const std::function<absl::Status()>& fn) {
  absl::MutexLock lock(mu_);

  // Failed DML can still be read while its backend transaction is active.
  // Further DML replays the error in the handler, and a commit ends the
  // readable phase before replaying it.
  if (!status_.ok() && op != OpType::kDml && op != OpType::kRollback) {
    if (op == OpType::kCommit &&
        HasState(backend::ReadWriteTransaction::State::kActive)) {
      Invalidate().IgnoreError();
    }
    if ((op != OpType::kRead && op != OpType::kSql) ||
        !HasState(backend::ReadWriteTransaction::State::kActive)) {
      return status_;
    }
  }

  // We only want to record the status for non-read operations, since read-only
  // operations can never cause the transaction to be aborted and never repeat
  // status errors. Non-DML SQL statements are read-only.
  const absl::Status call_status = fn();

  if (!call_status.ok()) {
    if (op == OpType::kCommit || HasPayload(call_status, kConstraintError) ||
        call_status.code() == absl::StatusCode::kAborted) {
      status_ = absl::Status(call_status.code(), call_status.message());
      if (op != OpType::kDml ||
          call_status.code() == absl::StatusCode::kAborted ||
          !HasState(backend::ReadWriteTransaction::State::kActive)) {
        Invalidate().IgnoreError();
      }
    }
    if (op == OpType::kDml) {
      SetDmlRequestReplayStatus(call_status);
    }
  }
  if (op == OpType::kRollback) {
    status_ = call_status;
  }
  MaybeRecordAttempt(op, call_status);

  return call_status;
}

void Transaction::MaybeRecordAttempt(OpType op, const absl::Status& status) {
  mu_.AssertHeld();
  backend::SystemStatsCollector* collector = stats_collector();
  if (type_ != kReadWrite || collector == nullptr || attempt_recorded_) {
    return;
  }
  backend::TransactionAttempt::Outcome outcome;
  if (status.code() == absl::StatusCode::kAborted) {
    outcome = backend::TransactionAttempt::Outcome::kAborted;
  } else if (op != OpType::kCommit) {
    // The attempt continues, or ended without a commit (by a rollback).
    return;
  } else if (status.ok()) {
    // A commit request can also return before committing, e.g. to ask a
    // multiplexed session for a precommit token.
    if (!IsCommitted()) {
      return;
    }
    outcome = backend::TransactionAttempt::Outcome::kCommitted;
  } else if (status.code() == absl::StatusCode::kFailedPrecondition ||
             HasPayload(status, kConstraintError)) {
    outcome = backend::TransactionAttempt::Outcome::kCommitFailedPrecondition;
  } else {
    outcome = backend::TransactionAttempt::Outcome::kCommitFailed;
  }
  attempt_recorded_ = true;
  const absl::Time now = absl::Now();
  collector->RecordTransactionAttempt(
      now, {.transaction_tag = transaction_tag_,
            .footprint = attempt_footprint_,
            .outcome = outcome,
            .commit_attempted = op == OpType::kCommit,
            .retry = retry_,
            .concurrency_mode = ConcurrencyMode(options_),
            .total_latency = now - create_time_,
            .commit_latency = commit_latency_});
}

bool ShouldReturnTransaction(
    const google::spanner::v1::TransactionSelector& selector) {
  if (selector.selector_case() ==
      spanner_api::TransactionSelector::SelectorCase::kBegin) {
    return true;
  }
  if (selector.selector_case() ==
      spanner_api::TransactionSelector::SelectorCase::kSingleUse) {
    return selector.single_use().has_read_only() &&
           selector.single_use().read_only().return_read_timestamp();
  }
  return false;
}

}  // namespace frontend
}  // namespace emulator
}  // namespace spanner
}  // namespace google

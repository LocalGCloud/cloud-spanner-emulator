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

#include <memory>
#include <optional>
#include <string>
#include <utility>
#include <variant>
#include <vector>

#include "google/protobuf/struct.pb.h"
#include "google/spanner/v1/keys.pb.h"
#include "google/spanner/v1/query_plan.pb.h"
#include "google/spanner/v1/result_set.pb.h"
#include "google/spanner/v1/spanner.pb.h"
#include "google/spanner/v1/transaction.pb.h"
#include "googlesql/public/analyzer_options.h"
#include "absl/status/status.h"
#include "absl/status/statusor.h"
#include "absl/strings/cord.h"
#include "absl/strings/str_cat.h"
#include "absl/strings/str_format.h"
#include "absl/time/time.h"
#include "absl/types/optional.h"
#include "absl/types/variant.h"
#include "backend/access/read.h"
#include "backend/access/write.h"
#include "backend/query/change_stream/change_stream_query_validator.h"
#include "backend/query/query_engine.h"
#include "common/config.h"
#include "common/constants.h"
#include "common/errors.h"
#include "frontend/common/protos.h"
#include "frontend/common/validations.h"
#include "frontend/converters/partition.h"
#include "frontend/converters/query.h"
#include "frontend/converters/reads.h"
#include "frontend/converters/resume_tokens.h"
#include "frontend/converters/types.h"
#include "frontend/converters/values.h"
#include "frontend/entities/session.h"
#include "frontend/entities/transaction.h"
#include "frontend/handlers/change_streams.h"
#include "frontend/handlers/request_stats.h"
#include "frontend/proto/partition_token.pb.h"
#include "frontend/proto/resume_token.pb.h"
#include "frontend/server/handler.h"
#include "frontend/server/request_context.h"
#include "googlesql/base/status_macros.h"

namespace google {
namespace spanner {
namespace emulator {
namespace frontend {

namespace spanner_api = ::google::spanner::v1;

namespace {

absl::Duration kMaxFutureReadDuration = absl::Hours(1);

// A read at a future timestamp waits for that timestamp. Fails the read at
// once if the wait would outlast the server deadline or the call's deadline.
absl::Status ValidateReadTimestampNotTooFarInFuture(absl::Time read_timestamp,
                                                    absl::Time now,
                                                    absl::Time deadline) {
  if (read_timestamp - now > kMaxFutureReadDuration) {
    return error::ReadTimestampTooFarInFuture(read_timestamp);
  }
  if (read_timestamp >= deadline) {
    return error::ReadTimestampPastRequestDeadline(read_timestamp, deadline);
  }
  return absl::OkStatus();
}

// Writes the rows of an EXPORT DATA statement with format CLOUD_SPANNER back
// to the database. Like production, the write is not transactional: every row
// commits on its own, and rows that would violate a constraint, such as a
// missing row for update_ignore_all or a duplicate unique index key, are
// skipped. Any other error stops the write.
absl::Status WriteExportedRows(Session& session,
                               const backend::SpannerExport& spanner_export) {
  spanner_api::TransactionOptions options;
  options.mutable_read_write();
  for (const std::vector<googlesql::Value>& row : spanner_export.rows) {
    GOOGLESQL_ASSIGN_OR_RETURN(std::unique_ptr<Transaction> txn,
                     session.CreateSingleUseTransaction(options));
    backend::Mutation mutation;
    mutation.AddWriteOp(spanner_export.upsert
                            ? backend::MutationOpType::kInsertOrUpdate
                            : backend::MutationOpType::kUpdate,
                        spanner_export.table, spanner_export.columns, {row});
    const absl::Status status = txn->GuardedCall(
        Transaction::OpType::kCommit, [&]() -> absl::Status {
          GOOGLESQL_RETURN_IF_ERROR(txn->Write(mutation));
          return txn->Commit();
        });
    if (status.ok() || absl::IsNotFound(status) ||
        absl::IsAlreadyExists(status) || absl::IsFailedPrecondition(status)) {
      continue;
    }
    return absl::Status(
        status.code(),
        absl::StrCat("EXPORT DATA failed to write a row to table ",
                     spanner_export.table, ": ", status.message()));
  }
  return absl::OkStatus();
}

absl::Status ValidateTransactionSelectorForQuery(
    const spanner_api::TransactionSelector& selector, bool is_dml) {
  if (selector.selector_case() ==
          spanner_api::TransactionSelector::SelectorCase::kSingleUse &&
      selector.single_use().mode_case() != v1::TransactionOptions::kReadOnly) {
    return error::InvalidModeForReadOnlySingleUseTransaction();
  }
  if (is_dml) {
    if (selector.begin().mode_case() == v1::TransactionOptions::kReadOnly) {
      // ReadWrite and PartitionedDML transactions are currently allowed.
      return error::ReadOnlyTransactionDoesNotSupportDml("ReadOnly");
    }
    if (selector.selector_case() ==
        spanner_api::TransactionSelector::SelectorCase::kSingleUse) {
      return error::DmlDoesNotSupportSingleUseTransaction();
    }
  }
  return absl::OkStatus();
}

class SlicedRowCursor : public backend::RowCursor {
 public:
  SlicedRowCursor(std::unique_ptr<backend::RowCursor> cursor,
                  int partition_index, int num_partitions)
      : cursor_(std::move(cursor)),
        partition_index_(partition_index),
        num_partitions_(num_partitions),
        current_row_(-1) {}

  bool Next() override {
    while (cursor_->Next()) {
      ++current_row_;
      if (current_row_ % num_partitions_ == partition_index_) {
        return true;
      }
    }
    return false;
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
  int partition_index_;
  int num_partitions_;
  int64_t current_row_;
};

absl::Status ValidatePartitionToken(
    const PartitionToken& partition_token,
    const spanner_api::ExecuteSqlRequest* request) {
  if (request->query_mode() != v1::ExecuteSqlRequest::NORMAL) {
    return error::InvalidPartitionedQueryMode();
  }
  if (partition_token.session() != request->session()) {
    return error::ReadFromDifferentSession();
  }
  if (request->transaction().selector_case() != v1::TransactionSelector::kId ||
      partition_token.transaction_id() != request->transaction().id()) {
    return error::ReadFromDifferentTransaction();
  }

  if (!partition_token.has_query_params()) {
    return error::ReadFromDifferentParameters();
  }
  auto query_params = partition_token.query_params();

  if (query_params.sql() != request->sql()) {
    return error::ReadFromDifferentParameters();
  }

  if (query_params.params().fields_size() != request->params().fields_size()) {
    return error::ReadFromDifferentParameters();
  }
  for (const auto& field : query_params.params().fields()) {
    if (!request->params().fields().contains(field.first) ||
        field.second.SerializeAsString() !=
            request->params().fields().at(field.first).SerializeAsString()) {
      return error::ReadFromDifferentParameters();
    }
  }

  if (query_params.param_types_size() != request->param_types_size()) {
    return error::ReadFromDifferentParameters();
  }
  for (const auto& param_type : query_params.param_types()) {
    if (!request->param_types().contains(param_type.first) ||
        param_type.second.GetTypeName() !=
            request->param_types().at(param_type.first).GetTypeName()) {
      return error::ReadFromDifferentParameters();
    }
  }

  return absl::OkStatus();
}

// Formats a duration like Cloud Spanner query statistics, e.g. "1.23 msecs".
std::string FormatStatsDuration(absl::Duration duration) {
  if (duration < absl::Seconds(1)) {
    return absl::StrFormat("%.2f msecs", absl::ToDoubleMilliseconds(duration));
  }
  return absl::StrFormat("%.2f secs", absl::ToDoubleSeconds(duration));
}

void AddQueryStatsFromQueryResult(const backend::QueryResult& result,
                                  google::protobuf::Struct* stats) {
  auto& fields = *stats->mutable_fields();
  fields["rows_returned"].set_string_value(
      absl::StrCat(result.num_output_rows));
  fields["rows_scanned"].set_string_value(absl::StrCat(result.rows_scanned));
  fields["elapsed_time"].set_string_value(
      FormatStatsDuration(result.elapsed_time));
  fields["cpu_time"].set_string_value(FormatStatsDuration(result.cpu_time));
  fields["query_plan_creation_time"].set_string_value(
      FormatStatsDuration(result.plan_creation_time));
  fields["optimizer_version"].set_string_value(
      absl::StrCat(kDefaultOptimizerVersion));
}

// Adds the query plan and the overall statistics that `query_mode` asks for
// to the stats of `response`, a ResultSet or PartialResultSet.
template <typename Response>
void AddQueryModeStats(v1::ExecuteSqlRequest::QueryMode query_mode,
                       backend::QueryResult& result, Response* response) {
  if (result.query_plan.has_value()) {
    *response->mutable_stats()->mutable_query_plan() =
        *std::move(result.query_plan);
  }
  if (query_mode == spanner_api::ExecuteSqlRequest::PROFILE ||
      query_mode == spanner_api::ExecuteSqlRequest::WITH_STATS ||
      query_mode == spanner_api::ExecuteSqlRequest::WITH_PLAN_AND_STATS) {
    AddQueryStatsFromQueryResult(
        result, response->mutable_stats()->mutable_query_stats());
  }
}

absl::Status AddUndeclaredParametersFromQueryResult(
    googlesql::QueryParametersMap* cursor, v1::ResultSetMetadata* metadata_pb) {
  for (auto const& param : *cursor) {
    auto* field_pb = metadata_pb->mutable_undeclared_parameters()->add_fields();
    field_pb->set_name(param.first);
    GOOGLESQL_RETURN_IF_ERROR(TypeToProto(param.second, field_pb->mutable_type()))
        << " when converting param " << param.first << " of type "
        << param.second << " in row cursor";
  }
  return absl::OkStatus();
}

absl::StatusOr<backend::QueryResult> ExecuteQuery(
    const spanner_api::ExecuteBatchDmlRequest_Statement& statement,
    std::shared_ptr<Transaction> txn,
    const google::protobuf::Map<std::string, google::protobuf::Value>&
        secure_context,
    const RequestStatsInfo& stats_info) {
  GOOGLESQL_ASSIGN_OR_RETURN(
      const backend::Query query,
      QueryFromProto(statement.sql(), statement.params(),
                     statement.param_types(),
                     txn->query_engine()->type_factory(),
                     txn->schema()->proto_bundle(), secure_context));
  return txn->ExecuteSql(query, spanner_api::ExecuteSqlRequest::NORMAL,
                         stats_info);
}

// An INSERT or DELETE on a placement table must be the only statement in its
// read-write transaction (a geo-partitioning limit), so a batch that combines
// one with other statements fails before any statement runs.
absl::Status ValidatePlacementDmlBatch(
    const spanner_api::ExecuteBatchDmlRequest& request, Transaction* txn) {
  if (request.statements_size() < 2 || !txn->IsReadWrite() ||
      !config::enforce_placement_dml_restrictions()) {
    return absl::OkStatus();
  }
  for (const auto& statement : request.statements()) {
    // Statements that can't be converted or analyzed report their errors when
    // they run.
    absl::StatusOr<backend::Query> query = QueryFromProto(
        statement.sql(), statement.params(), statement.param_types(),
        txn->query_engine()->type_factory(), txn->schema()->proto_bundle(),
        request.request_options().client_context().secure_context());
    if (!query.ok()) {
      continue;
    }
    absl::StatusOr<std::optional<std::string>> table =
        txn->query_engine()->GetPlacementInsertOrDeleteTable(*query,
                                                              txn->schema());
    if (table.ok() && table->has_value()) {
      return error::PlacementDmlMustBeOnlyStatement(**table);
    }
  }
  return absl::OkStatus();
}

int64_t HashRequest(const spanner_api::ExecuteSqlRequest* request) {
  spanner_api::ExecuteSqlRequest copy = *request;
  // Clearing resume token and sequence number so that the hash is based
  // entirely on the sql statement.
  copy.clear_resume_token();
  copy.set_seqno(0);
  return DeterministicFingerprint(copy);
}

int64_t HashRequest(const spanner_api::ExecuteBatchDmlRequest* request) {
  spanner_api::ExecuteBatchDmlRequest copy = *request;
  // Clearing sequence number so that the hash is based entirely on the sql
  // statement.
  copy.set_seqno(0);
  return DeterministicFingerprint(copy);
}

}  //  namespace

// Executes a SQL statement, returning all results in a single reply.
absl::Status ExecuteSql(RequestContext* ctx,
                        const spanner_api::ExecuteSqlRequest* request,
                        spanner_api::ResultSet* response) {
  // Take shared ownerships of session and transaction so that they will keep
  // valid throughout this function.
  GOOGLESQL_ASSIGN_OR_RETURN(std::shared_ptr<Session> session,
                   GetSession(ctx, request->session()));

  // Get underlying transaction.
  bool is_dml_query = backend::IsDMLQuery(request->sql());
  GOOGLESQL_RETURN_IF_ERROR(ValidateTransactionSelectorForQuery(request->transaction(),
                                                      is_dml_query));
  GOOGLESQL_ASSIGN_OR_RETURN(std::shared_ptr<Transaction> txn,
                   session->FindOrInitTransaction(request->transaction()));
  GOOGLESQL_RETURN_IF_ERROR(
      ValidateDirectedReadsOption(request->directed_read_options(), txn));
  txn->SetTransactionTag(request->request_options().transaction_tag());

  // Wrap all operations on this transaction so they are atomic.
  return txn->GuardedCall(
      is_dml_query ? Transaction::OpType::kDml : Transaction::OpType::kSql,
      [&]() -> absl::Status {
        if (request->data_boost_enabled()) {
          if (request->partition_token().empty()) {
            return error::DataBoostRequiresPartitionToken();
          }
        }
        // Register DML request and check for status replay.
        if (is_dml_query) {
          const auto state = txn->LookupOrRegisterDmlRequest(
              request->seqno(), HashRequest(request), request->sql());
          if (state.has_value()) {
            if (!state->status.ok()) {
              return state->status;
            }
            if (!std::holds_alternative<spanner_api::ResultSet>(
                    state->outcome)) {
              return error::ReplayRequestMismatch(request->seqno(),
                                                  request->sql());
            }
            *response = std::get<spanner_api::ResultSet>(state->outcome);
            return state->status;
          }

          // DML needs to explicitly check the transaction status since
          // the DML sequence number replay should take priority over returning
          // a previously encountered error status.
          GOOGLESQL_RETURN_IF_ERROR(txn->Status());
        }

        // Cannot query after commit, rollback, or non-recoverable error.
        if (txn->IsInvalid()) {
          return error::CannotUseTransactionAfterConstraintError();
        }
        if (txn->IsCommitted() || txn->IsRolledback()) {
          if (txn->IsPartitionedDml()) {
            return error::CannotReusePartitionedDmlTransaction();
          }
          return error::CannotReadOrQueryAfterCommitOrRollback();
        }
        if (txn->IsReadOnly()) {
          if (is_dml_query) {
            return error::ReadOnlyTransactionDoesNotSupportDml("ReadOnly");
          }
          GOOGLESQL_ASSIGN_OR_RETURN(absl::Time read_timestamp, txn->GetReadTimestamp());
          GOOGLESQL_RETURN_IF_ERROR(ValidateReadTimestampNotTooFarInFuture(
              read_timestamp, ctx->env()->clock()->Now(), ctx->deadline()));
        }

        // Convert and execute provided SQL statement.
        GOOGLESQL_ASSIGN_OR_RETURN(
            const backend::Query query,
            QueryFromProto(
                request->sql(), request->params(), request->param_types(),
                txn->query_engine()->type_factory(),
                txn->schema()->proto_bundle(),
                request->request_options().client_context().secure_context()));
        auto maybe_result = txn->ExecuteSql(
            query, request->query_mode(),
            MakeRequestStatsInfo(ctx, request->session(),
                                 request->request_options(),
                                 request->partition_token()));
        if (!maybe_result.ok()) {
          absl::Status error = maybe_result.status();
          if (txn->IsPartitionedDml()) {
            // A Partitioned DML transaction will become invalidated on any
            // error.
            error.SetPayload(kConstraintError, absl::Cord(""));
          }
          if (ShouldReturnTransaction(request->transaction())) {
            // The transaction ID has not been returned to the user yet, so we
            // must rollback the transaction to avoid leaving it in an active
            // state.
            txn->Rollback().IgnoreError();
          }
          return error;
        }
        backend::QueryResult& result = maybe_result.value();
        if (result.spanner_export.has_value()) {
          GOOGLESQL_RETURN_IF_ERROR(
              WriteExportedRows(*session, *result.spanner_export));
        }

        // Populate transaction metadata.
        if (ShouldReturnTransaction(request->transaction())) {
          GOOGLESQL_ASSIGN_OR_RETURN(*response->mutable_metadata()->mutable_transaction(),
                           txn->ToProto());
        }

        if (txn->IsReadWrite() && session->multiplexed()) {
          // Set an empty precommit token.
          response->mutable_precommit_token();
        }

        // Return query parameter types.
        GOOGLESQL_RETURN_IF_ERROR(AddUndeclaredParametersFromQueryResult(
            &result.parameter_types, response->mutable_metadata()));

        if (is_dml_query) {
          if (txn->IsPartitionedDml()) {
            response->mutable_stats()->set_row_count_lower_bound(
                result.modified_row_count);
          } else {
            response->mutable_stats()->set_row_count_exact(
                result.modified_row_count);
          }

          std::optional<PartitionToken> partition_token;
          if (!request->partition_token().empty()) {
            GOOGLESQL_ASSIGN_OR_RETURN(
                PartitionToken parsed_token,
                PartitionTokenFromString(request->partition_token()));
            GOOGLESQL_RETURN_IF_ERROR(ValidatePartitionToken(parsed_token, request));
            if (parsed_token.has_query_partition_index() &&
                parsed_token.query_num_partitions() > 1 &&
                result.rows != nullptr) {
              result.rows = std::make_unique<SlicedRowCursor>(
                  std::move(result.rows),
                  parsed_token.query_partition_index(),
                  parsed_token.query_num_partitions());
            }
            partition_token = std::move(parsed_token);
          }

          if (result.rows == nullptr) {
            // Set empty row type.
            response->mutable_metadata()->mutable_row_type();
          } else {
            // It contains DML THEN RETURN row results.
            GOOGLESQL_RETURN_IF_ERROR(RowCursorToResultSetProto(result.rows.get(),
                                                      /*limit=*/0, response));
          }
        } else {
          std::optional<PartitionToken> partition_token;
          if (!request->partition_token().empty()) {
            GOOGLESQL_ASSIGN_OR_RETURN(
                PartitionToken parsed_token,
                PartitionTokenFromString(request->partition_token()));
            GOOGLESQL_RETURN_IF_ERROR(ValidatePartitionToken(parsed_token, request));
            if (parsed_token.has_query_partition_index() &&
                parsed_token.query_num_partitions() > 1 &&
                result.rows != nullptr) {
              result.rows = std::make_unique<SlicedRowCursor>(
                  std::move(result.rows),
                  parsed_token.query_partition_index(),
                  parsed_token.query_num_partitions());
            }
            partition_token = std::move(parsed_token);
          }

          GOOGLESQL_RETURN_IF_ERROR(RowCursorToResultSetProto(result.rows.get(),
                                                    /*limit=*/0, response));

          if (partition_token.has_value() && partition_token->empty_query_partition()) {
            response->clear_rows();
          }
        }

        AddQueryModeStats(request->query_mode(), result, response);

        if (is_dml_query) {
          txn->SetDmlReplayOutcome(*response);
        }
        return absl::OkStatus();
      });
}
REGISTER_GRPC_HANDLER(Spanner, ExecuteSql);

// Executes a SQL statement, returning all results as a stream.
//
// Query results are chunked into PartialResultSets of limited size. Each one
// that ends on a row boundary has a resume token, with which a resent request
// continues after that row. DML results have no resume tokens.
absl::Status ExecuteStreamingSql(
    RequestContext* ctx, const spanner_api::ExecuteSqlRequest* request,
    ServerStream<spanner_api::PartialResultSet>* stream) {
  // Take shared ownerships of session and transaction so that they will keep
  // valid throughout this function.
  GOOGLESQL_ASSIGN_OR_RETURN(std::shared_ptr<Session> session,
                   GetSession(ctx, request->session()));

  backend::ChangeStreamQueryValidator::ChangeStreamMetadata
      change_stream_metadata;

  // Get underlying transaction.
  bool is_dml_query = backend::IsDMLQuery(request->sql());

  GOOGLESQL_RETURN_IF_ERROR(ValidateTransactionSelectorForQuery(request->transaction(),
                                                      is_dml_query));

  // A request with a resume token resumes the stream that returned the token,
  // in the same transaction or at the same read timestamp.
  const uint64_t resume_fingerprint = ResumeFingerprint(*request);
  std::optional<ResumeToken> resume_token;
  spanner_api::TransactionSelector selector = request->transaction();
  if (!request->resume_token().empty()) {
    GOOGLESQL_ASSIGN_OR_RETURN(
        resume_token,
        ParseResumeToken(request->resume_token(), resume_fingerprint));
    if (resume_token->has_rows()) {
      GOOGLESQL_ASSIGN_OR_RETURN(selector,
                       ResumedTransactionSelector(selector, *resume_token));
    }
  }
  GOOGLESQL_ASSIGN_OR_RETURN(std::shared_ptr<Transaction> txn,
                   session->FindOrInitTransaction(selector));
  GOOGLESQL_RETURN_IF_ERROR(
      ValidateDirectedReadsOption(request->directed_read_options(), txn));
  txn->SetTransactionTag(request->request_options().transaction_tag());

  // Wrap all operations on this transaction so they are atomic.
  absl::Status status = txn->GuardedCall(
      is_dml_query ? Transaction::OpType::kDml : Transaction::OpType::kSql,
      [&]() -> absl::Status {
        if (request->data_boost_enabled()) {
          if (request->partition_token().empty()) {
            return error::DataBoostRequiresPartitionToken();
          }
        }
        // Register DML request and check for status replay.
        if (is_dml_query) {
          const auto state = txn->LookupOrRegisterDmlRequest(
              request->seqno(), HashRequest(request), request->sql());
          if (state.has_value()) {
            if (!state->status.ok()) {
              return state->status;
            }
            if (!std::holds_alternative<spanner_api::ResultSet>(
                    state->outcome)) {
              return error::ReplayRequestMismatch(request->seqno(),
                                                  request->sql());
            }
            spanner_api::PartialResultSet response;
            spanner_api::ResultSet replay_result =
                std::get<spanner_api::ResultSet>(state->outcome);
            *response.mutable_stats() = replay_result.stats();
            *response.mutable_metadata() = replay_result.metadata();
            if (session->multiplexed() && txn->IsReadWrite()) {
              response.mutable_precommit_token();
            }
            stream->Send(response);
            return state->status;
          }

          // DML needs to explicitly check the transaction status since
          // the DML sequence number replay should take priority over returning
          // a previously encountered error status.
          GOOGLESQL_RETURN_IF_ERROR(txn->Status());
        }

        // Cannot query after commit, rollback, or non-recoverable error.
        if (txn->IsInvalid()) {
          return error::CannotUseTransactionAfterConstraintError();
        }
        if (txn->IsCommitted() || txn->IsRolledback()) {
          if (txn->IsPartitionedDml()) {
            return error::CannotReusePartitionedDmlTransaction();
          }
          return error::CannotReadOrQueryAfterCommitOrRollback();
        }
        if (txn->IsReadOnly()) {
          if (is_dml_query) {
            return error::ReadOnlyTransactionDoesNotSupportDml("ReadOnly");
          }
          GOOGLESQL_ASSIGN_OR_RETURN(absl::Time read_timestamp, txn->GetReadTimestamp());
          GOOGLESQL_RETURN_IF_ERROR(ValidateReadTimestampNotTooFarInFuture(
              read_timestamp, ctx->env()->clock()->Now(), ctx->deadline()));
        }
        // Convert and execute provided SQL statement.
        GOOGLESQL_ASSIGN_OR_RETURN(
            const backend::Query query,
            QueryFromProto(
                request->sql(), request->params(), request->param_types(),
                txn->query_engine()->type_factory(),
                txn->schema()->proto_bundle(),
                request->request_options().client_context().secure_context()));
        bool in_read_write_txn = txn->IsReadWrite() || txn->IsPartitionedDml();
        GOOGLESQL_ASSIGN_OR_RETURN(change_stream_metadata,
                         backend::QueryEngine::TryGetChangeStreamMetadata(
                             query, txn->schema(), in_read_write_txn));
        // if current query is a change stream query, return and exit current
        // transaction lambda to avoid nested transaction call.
        if (change_stream_metadata.is_change_stream_query) {
          return txn->CheckChangeStreamReadAccess(
              change_stream_metadata.change_stream_name);
        }
        // The stream starts where the resumed stream stopped, or before the
        // first row of `txn`.
        ResumeToken start;
        if (resume_token.has_value()) {
          if (!resume_token->has_rows() ||
              (resume_token->rows().transaction_id() != 0 &&
               resume_token->rows().transaction_id() != txn->id())) {
            return error::ResumeTokenMismatch();
          }
          start = *resume_token;
        } else if (!is_dml_query) {
          start.set_request_fingerprint(resume_fingerprint);
          if (IsSingleUseTransaction(request->transaction())) {
            GOOGLESQL_ASSIGN_OR_RETURN(absl::Time read_timestamp,
                             txn->GetReadTimestamp());
            start.mutable_rows()->set_read_timestamp_micros(
                absl::ToUnixMicros(read_timestamp));
          } else {
            start.mutable_rows()->set_transaction_id(txn->id());
          }
        }
        auto maybe_result = txn->ExecuteSql(
            query, request->query_mode(),
            MakeRequestStatsInfo(ctx, request->session(),
                                 request->request_options(),
                                 request->partition_token()));
        if (!maybe_result.ok()) {
          absl::Status error = maybe_result.status();
          if (txn->IsPartitionedDml()) {
            // A Partitioned DML transaction will become invalidated on any
            // error.
            error.SetPayload(kConstraintError, absl::Cord(""));
          }
          if (ShouldReturnTransaction(request->transaction())) {
            // The transaction ID has not been returned to the user yet, so we
            // must rollback the transaction to avoid leaving it in an active
            // state.
            txn->Rollback().IgnoreError();
          }
          return error;
        }
        backend::QueryResult& result = maybe_result.value();
        if (result.spanner_export.has_value()) {
          GOOGLESQL_RETURN_IF_ERROR(
              WriteExportedRows(*session, *result.spanner_export));
        }

        std::optional<PartitionToken> partition_token;
        if (!request->partition_token().empty()) {
          GOOGLESQL_ASSIGN_OR_RETURN(
              PartitionToken parsed_token,
              PartitionTokenFromString(request->partition_token()));
          GOOGLESQL_RETURN_IF_ERROR(ValidatePartitionToken(parsed_token, request));
          if (parsed_token.has_query_partition_index() &&
              parsed_token.query_num_partitions() > 1 &&
              result.rows != nullptr) {
            result.rows = std::make_unique<SlicedRowCursor>(
                std::move(result.rows),
                parsed_token.query_partition_index(),
                parsed_token.query_num_partitions());
          }
          partition_token = std::move(parsed_token);
        }

        std::vector<spanner_api::PartialResultSet> responses;
        if (is_dml_query) {
          responses.emplace_back();
          if (result.rows == nullptr) {
            // Set empty row type.
            responses.back().mutable_metadata()->mutable_row_type();
          } else {
            // It contains DML THEN RETURN row results.
            GOOGLESQL_ASSIGN_OR_RETURN(responses, RowCursorToPartialResultSetProtos(
                                            result.rows.get(), /*limit=*/0));
          }
          if (txn->IsPartitionedDml()) {
            responses.back().mutable_stats()->set_row_count_lower_bound(
                result.modified_row_count);
          } else {
            responses.back().mutable_stats()->set_row_count_exact(
                result.modified_row_count);
          }
        } else {
          GOOGLESQL_ASSIGN_OR_RETURN(responses,
                           RowCursorToPartialResultSetProtos(
                               result.rows.get(), /*limit=*/0, start));
        }
        if (session->multiplexed() && txn->IsReadWrite()) {
          for (auto& response : responses) {
            response.mutable_precommit_token();
          }
        }

        if (partition_token.has_value() && partition_token->empty_query_partition()) {
          // Clear all partial responses except the first one. Return only
          // metadata in the first partial response.
          responses.resize(1);
          responses.front().clear_values();
          responses.front().clear_chunked_value();
          responses.front().clear_resume_token();
        }

        // Populate transaction metadata.
        if (ShouldReturnTransaction(request->transaction())) {
          GOOGLESQL_ASSIGN_OR_RETURN(
              *responses.front().mutable_metadata()->mutable_transaction(),
              txn->ToProto());
        }
        // Return query parameter types.
        GOOGLESQL_RETURN_IF_ERROR(AddUndeclaredParametersFromQueryResult(
            &result.parameter_types, responses.front().mutable_metadata()));

        // Statistics are sent only once, with the last response.
        AddQueryModeStats(request->query_mode(), result, &responses.back());

        // Send results back to client.
        for (const auto& response : responses) {
          stream->Send(response);
        }

        if (is_dml_query) {
          spanner_api::ResultSet replay_result;
          *replay_result.mutable_stats() = responses.back().stats();
          *replay_result.mutable_metadata() = responses[0].metadata();
          txn->SetDmlReplayOutcome(replay_result);
        }
        return absl::OkStatus();
      });
  if (change_stream_metadata.is_change_stream_query) {
    GOOGLESQL_RETURN_IF_ERROR(status);
    ChangeStreamsHandler change_streams_handler{change_stream_metadata};
    return change_streams_handler.ExecuteChangeStreamQuery(
        request, stream, session, resume_token);
  }
  return status;
}
REGISTER_GRPC_HANDLER(Spanner, ExecuteStreamingSql);

// Executes a batch of DML statements.
absl::Status ExecuteBatchDml(RequestContext* ctx,
                             const spanner_api::ExecuteBatchDmlRequest* request,
                             spanner_api::ExecuteBatchDmlResponse* response) {
  // Verify the request has DML statement(s).
  if (request->statements().empty()) {
    return error::InvalidBatchDmlRequest();
  }

  // Take shared ownerships of session and transaction so that they will keep
  // valid throughout this function.
  GOOGLESQL_ASSIGN_OR_RETURN(std::shared_ptr<Session> session,
                   GetSession(ctx, request->session()));

  // Get underlying transaction.
  GOOGLESQL_RETURN_IF_ERROR(ValidateTransactionSelectorForQuery(request->transaction(),
                                                      /*is_dml=*/true));
  GOOGLESQL_ASSIGN_OR_RETURN(std::shared_ptr<Transaction> txn,
                   session->FindOrInitTransaction(request->transaction()));

  if (txn->IsPartitionedDml()) {
    return error::BatchDmlOnlySupportsReadWriteTransaction();
  }
  txn->SetTransactionTag(request->request_options().transaction_tag());

  // Set default response status to OK. Any error will override this.
  *response->mutable_status() = StatusToProto(absl::OkStatus());

  // Wrap all operations on this transaction so they are atomic.
  return txn->GuardedCall(Transaction::OpType::kDml, [&]() -> absl::Status {
    // Register DML request and check for status replay.
    const auto state = txn->LookupOrRegisterDmlRequest(
        request->seqno(), HashRequest(request), request->statements(0).sql());
    if (state.has_value()) {
      if (!state->status.ok() &&
          txn->DMLErrorType() ==
              Transaction::DMLErrorHandlingMode::kDmlRegistrationError) {
        return state->status;
      }
      if (!std::holds_alternative<spanner_api::ExecuteBatchDmlResponse>(
              state->outcome)) {
        return error::ReplayRequestMismatch(request->seqno(),
                                            request->statements(0).sql());
      }
      *response =
          std::get<spanner_api::ExecuteBatchDmlResponse>(state->outcome);

      // BatchDml always returns OK status with the error being populated in the
      // response.
      return absl::OkStatus();
    }
    // DML needs to explicitly check the transaction status since
    // the DML sequence number replay should take priority over returning
    // a previously encountered error status.
    GOOGLESQL_RETURN_IF_ERROR(txn->Status());

    // Cannot query after commit, rollback, or non-recoverable error.
    if (txn->IsInvalid()) {
      return error::CannotUseTransactionAfterConstraintError();
    }
    if (txn->IsCommitted() || txn->IsRolledback()) {
      return error::CannotReadOrQueryAfterCommitOrRollback();
    }

    if (absl::Status placement_status =
            ValidatePlacementDmlBatch(*request, txn.get());
        !placement_status.ok()) {
      *response->mutable_status() = StatusToProto(placement_status);
      txn->SetDmlReplayOutcome(*response);
      if (ShouldReturnTransaction(request->transaction())) {
        // The transaction ID has not been returned to the user yet, so we
        // must rollback the transaction to avoid leaving it in an active
        // state.
        txn->Rollback().IgnoreError();
      }
      return absl::OkStatus();
    }

    for (int index = 0; index < request->statements_size(); ++index) {
      const auto& statement = request->statements(index);
      if (!backend::IsDMLQuery(statement.sql())) {
        absl::Status error = error::ExecuteBatchDmlOnlySupportsDmlStatements(
            index, statement.sql());
        *response->mutable_status() = StatusToProto(error);
        txn->SetDmlReplayOutcome(*response);
        return absl::OkStatus();
      }

      const auto maybe_result = ExecuteQuery(
          statement, txn,
          request->request_options().client_context().secure_context(),
          MakeRequestStatsInfo(ctx, request->session(),
                               request->request_options(),
                               /*partition_token=*/""));
      if (!maybe_result.ok() &&
          maybe_result.status().code() != absl::StatusCode::kAborted) {
        absl::Status error = maybe_result.status();
        *response->mutable_status() = StatusToProto(error);
        txn->SetDmlReplayOutcome(*response);
        txn->MaybeInvalidate(error);
        if (!txn->IsInvalid() &&
            ShouldReturnTransaction(request->transaction()) && index == 0) {
          // The transaction ID has not been returned to the user yet, so we
          // must rollback the transaction to avoid leaving it in an active
          // state.
          txn->Rollback().IgnoreError();
        }
        return absl::OkStatus();
      } else if (maybe_result.status().code() == absl::StatusCode::kAborted) {
        return maybe_result.status();
      }

      const auto& result = maybe_result.value();
      spanner_api::ResultSet* result_set = response->add_result_sets();
      result_set->mutable_stats()->set_row_count_exact(
          result.modified_row_count);

      // Only populate metadata for first result set.
      if (index == 0) {
        result_set->mutable_metadata()->mutable_row_type();
        if (ShouldReturnTransaction(request->transaction())) {
          GOOGLESQL_ASSIGN_OR_RETURN(
              *result_set->mutable_metadata()->mutable_transaction(),
              txn->ToProto());
        }
      }
    }

    if (txn->IsReadWrite() && session->multiplexed()) {
      response->mutable_precommit_token();
    }

    // Set the replay outcome.
    txn->SetDmlReplayOutcome(*response);
    return absl::OkStatus();
  });
}
REGISTER_GRPC_HANDLER(Spanner, ExecuteBatchDml);

}  // namespace frontend
}  // namespace emulator
}  // namespace spanner
}  // namespace google

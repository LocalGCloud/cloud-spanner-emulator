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

#include "frontend/converters/reads.h"

#include <cstdint>
#include <memory>
#include <optional>
#include <vector>

#include "google/spanner/v1/result_set.pb.h"
#include "google/spanner/v1/spanner.pb.h"
#include "google/spanner/v1/transaction.pb.h"
#include "backend/common/ids.h"
#include "common/errors.h"
#include "frontend/common/protos.h"
#include "frontend/common/validations.h"
#include "frontend/converters/resume_tokens.h"
#include "frontend/entities/session.h"
#include "frontend/entities/transaction.h"
#include "frontend/handlers/request_stats.h"
#include "frontend/proto/resume_token.pb.h"
#include "frontend/server/handler.h"
#include "frontend/server/request_context.h"
#include "googlesql/base/status_macros.h"
#include "absl/status/status.h"
#include "absl/time/time.h"

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

absl::Status ValidateTransactionSelectorForRead(
    const spanner_api::TransactionSelector& selector) {
  if (selector.selector_case() ==
          spanner_api::TransactionSelector::SelectorCase::kSingleUse &&
      selector.single_use().mode_case() != v1::TransactionOptions::kReadOnly) {
    return error::InvalidModeForReadOnlySingleUseTransaction();
  }
  return absl::OkStatus();
}

}  //  namespace

// Reads rows from the database, returning all results in a single reply.
absl::Status Read(RequestContext* ctx, const spanner_api::ReadRequest* request,
                  spanner_api::ResultSet* response) {
  // Get session information.
  GOOGLESQL_ASSIGN_OR_RETURN(std::shared_ptr<Session> session,
                   GetSession(ctx, request->session()));

  // Get underlying transaction.
  GOOGLESQL_RETURN_IF_ERROR(ValidateTransactionSelectorForRead(request->transaction()));
  GOOGLESQL_ASSIGN_OR_RETURN(std::shared_ptr<Transaction> txn,
                   session->FindOrInitTransaction(request->transaction()));
  GOOGLESQL_RETURN_IF_ERROR(
      ValidateDirectedReadsOption(request->directed_read_options(), txn));
  txn->SetTransactionTag(request->request_options().transaction_tag());

  // Wrap all operations on this transaction so they are atomic .
  return txn->GuardedCall(Transaction::OpType::kRead, [&]() -> absl::Status {
    if (request->data_boost_enabled()) {
      if (request->partition_token().empty()) {
        return error::DataBoostRequiresPartitionToken();
      }
    }
    // Cannot read after commit, rollback, or non-recoverable error.
    if (txn->IsInvalid()) {
      return error::CannotUseTransactionAfterConstraintError();
    }
    if (txn->IsCommitted() || txn->IsRolledback()) {
      return error::CannotReadOrQueryAfterCommitOrRollback();
    }
    if (txn->IsReadOnly()) {
      GOOGLESQL_ASSIGN_OR_RETURN(absl::Time read_timestamp, txn->GetReadTimestamp());
      GOOGLESQL_RETURN_IF_ERROR(ValidateReadTimestampNotTooFarInFuture(
          read_timestamp, ctx->env()->clock()->Now(), ctx->deadline()));
    }

    // Parse read request.
    backend::ReadArg read_arg;
    GOOGLESQL_RETURN_IF_ERROR(ReadArgFromProto(*txn->schema(), *request, &read_arg));

    // Execute read on backend.
    std::unique_ptr<backend::RowCursor> cursor;
    auto status = txn->Read(
        read_arg, &cursor,
        MakeRequestStatsInfo(ctx, request->session(),
                             request->request_options(),
                             request->partition_token()));
    if (!status.ok()) {
      if (ShouldReturnTransaction(request->transaction())) {
        // The transaction ID has not been returned to the user yet, so we
        // must rollback the transaction to avoid leaving it in an active
        // state.
        txn->Rollback().IgnoreError();
      }
      return status;
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

    // Convert read results to proto.
    return RowCursorToResultSetProto(cursor.get(), request->limit(), response);
  });
}
REGISTER_GRPC_HANDLER(Spanner, Read);

// Reads rows from the database, returning all results as a stream.
//
// Results are chunked into PartialResultSets of limited size. Each one that
// ends on a row boundary has a resume token, with which a resent request
// continues after that row.
absl::Status StreamingRead(
    RequestContext* ctx, const spanner_api::ReadRequest* request,
    ServerStream<spanner_api::PartialResultSet>* stream) {
  // Get session information.
  GOOGLESQL_ASSIGN_OR_RETURN(std::shared_ptr<Session> session,
                   GetSession(ctx, request->session()));

  // Get underlying transaction. A request with a resume token resumes the
  // stream that returned the token, in the same transaction or at the same
  // read timestamp.
  GOOGLESQL_RETURN_IF_ERROR(ValidateTransactionSelectorForRead(request->transaction()));
  const uint64_t resume_fingerprint = ResumeFingerprint(*request);
  std::optional<ResumeToken> resume_token;
  spanner_api::TransactionSelector selector = request->transaction();
  if (!request->resume_token().empty()) {
    GOOGLESQL_ASSIGN_OR_RETURN(
        resume_token,
        ParseResumeToken(request->resume_token(), resume_fingerprint));
    GOOGLESQL_ASSIGN_OR_RETURN(selector,
                     ResumedTransactionSelector(selector, *resume_token));
  }
  GOOGLESQL_ASSIGN_OR_RETURN(std::shared_ptr<Transaction> txn,
                   session->FindOrInitTransaction(selector));
  GOOGLESQL_RETURN_IF_ERROR(
      ValidateDirectedReadsOption(request->directed_read_options(), txn));
  txn->SetTransactionTag(request->request_options().transaction_tag());

  // Wrap all operations on this transaction so they are atomic.
  return txn->GuardedCall(Transaction::OpType::kRead, [&]() -> absl::Status {
    if (request->data_boost_enabled()) {
      if (request->partition_token().empty()) {
        return error::DataBoostRequiresPartitionToken();
      }
    }
    // Cannot read after commit, rollback, or non-recoverable error.
    if (txn->IsInvalid()) {
      return error::CannotUseTransactionAfterConstraintError();
    }
    if (txn->IsCommitted() || txn->IsRolledback()) {
      return error::CannotReadOrQueryAfterCommitOrRollback();
    }
    if (txn->IsReadOnly()) {
      GOOGLESQL_ASSIGN_OR_RETURN(absl::Time read_timestamp, txn->GetReadTimestamp());
      GOOGLESQL_RETURN_IF_ERROR(ValidateReadTimestampNotTooFarInFuture(
          read_timestamp, ctx->env()->clock()->Now(), ctx->deadline()));
    }

    // The stream starts where the resumed stream stopped, or before the first
    // row of `txn`.
    ResumeToken start;
    if (resume_token.has_value()) {
      if (resume_token->rows().transaction_id() != 0 &&
          resume_token->rows().transaction_id() != txn->id()) {
        return error::ResumeTokenMismatch();
      }
      start = *resume_token;
    } else {
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

    // Parse read request.
    backend::ReadArg read_arg;
    GOOGLESQL_RETURN_IF_ERROR(ReadArgFromProto(*txn->schema(), *request, &read_arg));

    // Execute read on backend.
    std::unique_ptr<backend::RowCursor> cursor;
    auto read_status = txn->Read(
        read_arg, &cursor,
        MakeRequestStatsInfo(ctx, request->session(),
                             request->request_options(),
                             request->partition_token()));
    if (!read_status.ok()) {
      if (ShouldReturnTransaction(request->transaction())) {
        // The transaction ID has not been returned to the user yet, so we
        // must rollback the transaction to avoid leaving it in an active
        // state.
        txn->Rollback().IgnoreError();
      }
      return read_status;
    }

    // Convert read results to protos.
    GOOGLESQL_ASSIGN_OR_RETURN(
        std::vector<spanner_api::PartialResultSet> responses,
        RowCursorToPartialResultSetProtos(cursor.get(), request->limit(),
                                          start));

    // Populate transaction metadata.
    if (ShouldReturnTransaction(request->transaction())) {
      GOOGLESQL_ASSIGN_OR_RETURN(
          *responses.front().mutable_metadata()->mutable_transaction(),
          txn->ToProto());
    }
    // Set an empty precommit token for multiplexed read-write transactions.
    if (session->multiplexed() && txn->IsReadWrite()) {
      for (auto& response : responses) {
        response.mutable_precommit_token();
      }
    }

    // Send results back to client.
    for (const auto& response : responses) {
      stream->Send(response);
    }
    return absl::OkStatus();
  });
}
REGISTER_GRPC_HANDLER(Spanner, StreamingRead);

}  // namespace frontend
}  // namespace emulator
}  // namespace spanner
}  // namespace google

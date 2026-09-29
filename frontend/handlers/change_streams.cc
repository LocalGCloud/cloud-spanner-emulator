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

#include "frontend/handlers/change_streams.h"

#include <algorithm>
#include <cstdint>
#include <memory>
#include <optional>
#include <string>
#include <utility>
#include <vector>

#include "google/spanner/v1/spanner.pb.h"
#include "google/spanner/v1/transaction.pb.h"
#include "absl/flags/flag.h"
#include "absl/log/log.h"
#include "absl/status/status.h"
#include "absl/strings/substitute.h"
#include "absl/time/time.h"
#include "backend/access/read.h"
#include "backend/query/change_stream/change_stream_query_validator.h"
#include "backend/query/query_engine.h"
#include "backend/schema/catalog/change_stream.h"
#include "backend/schema/catalog/schema.h"
#include "common/clock.h"
#include "common/errors.h"
#include "frontend/converters/change_streams.h"
#include "frontend/converters/pg_change_streams.h"
#include "frontend/converters/resume_tokens.h"
#include "frontend/converters/time.h"
#include "frontend/entities/session.h"
#include "frontend/entities/transaction.h"
#include "frontend/proto/resume_token.pb.h"
#include "frontend/server/handler.h"
#include "googlesql/base/ret_check.h"
#include "googlesql/base/status_macros.h"

ABSL_FLAG(bool, cloud_spanner_emulator_test_with_fake_partition_table, false,
          "TEST ONLY. Set to true to enable querying against mocked change "
          "stream internal partition table during test.");

ABSL_FLAG(
    absl::Duration, change_streams_partition_query_chop_interval,
    absl::Milliseconds(100),
    "Change streams chopped interval for partition query in milliseconds.");

namespace google {
namespace spanner {
namespace emulator {
namespace frontend {
namespace {
// We only allow single-use read-only strong transaction for change stream
// queries
absl::Status ValidateTransactionSelectorForChangeStreamQuery(
    const spanner_api::TransactionSelector& selector) {
  // Default transaction selector is single use read only strong transaction, so
  // directly returns ok
  if (selector.selector_case() ==
      spanner_api::TransactionSelector::SELECTOR_NOT_SET) {
    return absl::OkStatus();
  }
  if (selector.selector_case() !=
      spanner_api::TransactionSelector::SelectorCase::kSingleUse) {
    return error::ChangeStreamQueriesMustBeSingleUseOnly();
  }
  if (!selector.single_use().read_only().has_strong()) {
    return error::ChangeStreamQueriesMustBeStrongReads();
  }
  return absl::OkStatus();
}

absl::Status VerifyChangeStreamExistence(const std::string& change_stream_name,
                                         const backend::Schema* schema) {
  auto change_stream = schema->FindChangeStream(change_stream_name);
  if (change_stream != nullptr) {
    return absl::OkStatus();
  }
  return error::ChangeStreamNotFound(change_stream_name);
}

absl::StatusOr<absl::Duration> TryGetChangeStreamRetentionPeriod(
    const std::string& change_stream_name, std::shared_ptr<Session> session,
    absl::Time read_ts) {
  spanner_api::TransactionOptions txn_options;
  txn_options.mutable_read_only()->set_return_read_timestamp(false);
  // If the user provided tvf start time is past now, wait until this future
  // time to perform a read on partition token end time.
  GOOGLESQL_ASSIGN_OR_RETURN(
      *txn_options.mutable_read_only()->mutable_min_read_timestamp(),
      TimestampToProto(read_ts));
  GOOGLESQL_ASSIGN_OR_RETURN(auto txn, session->CreateSingleUseTransaction(txn_options));
  auto change_stream = txn->schema()->FindChangeStream(change_stream_name);
  if (change_stream != nullptr) {
    return absl::Seconds(change_stream->parsed_retention_period());
  }
  return error::ChangeStreamNotFound(change_stream_name);
}

bool IsQueryResultEmpty(backend::QueryResult& result) {
  return result.num_output_rows == 0;
}

// Forwards the rows of `rows` and collects the timestamps in `column`.
class TimestampCollectingRowCursor : public backend::RowCursor {
 public:
  TimestampCollectingRowCursor(backend::RowCursor* rows, int column,
                               std::vector<absl::Time>* timestamps)
      : rows_(rows), column_(column), timestamps_(timestamps) {}

  bool Next() override {
    if (!rows_->Next()) {
      return false;
    }
    timestamps_->push_back(rows_->ColumnValue(column_).ToTime());
    return true;
  }
  absl::Status Status() const override { return rows_->Status(); }
  int NumColumns() const override { return rows_->NumColumns(); }
  const std::string ColumnName(int i) const override {
    return rows_->ColumnName(i);
  }
  const googlesql::Value ColumnValue(int i) const override {
    return rows_->ColumnValue(i);
  }
  const googlesql::Type* ColumnType(int i) const override {
    return rows_->ColumnType(i);
  }

 private:
  backend::RowCursor* rows_;
  int column_;
  std::vector<absl::Time>* timestamps_;
};

// The columns of the change stream's internal tables with the timestamps of
// the records that their rows become: the start time of a partition, and the
// commit timestamp of a data change record.
constexpr int kPartitionStartTimeColumn = 0;
constexpr int kCommitTimestampColumn = 1;

absl::Status ValidateTokenInRetentionWindow(
    const absl::Time tvf_start, const absl::Time current_chopped_start,
    const absl::Time current_token_end,
    const absl::Duration current_retention) {
  absl::Time gc_time = Clock().Now() - current_retention;
  // If current chopped start is before gc_time, we know this token must be
  // invalid and need to identify the correct error type.
  if (current_chopped_start < gc_time) {
    // If current token already end before gc_time, this entire partition has
    // expired.
    if (current_token_end < gc_time) {
      return error::ChangeStreamStalePartition();
    }
    // Although token is not expired, the user provided tvf start time is too
    // old.
    return error::InvalidChangeStreamTvfArgumentStartTimestampTooOld(
        absl::FormatTime(gc_time), absl::FormatTime(tvf_start));
  }
  return absl::OkStatus();
}

}  // namespace

ChangeRecordSender::ChangeRecordSender(
    ServerStream<spanner_api::PartialResultSet>* stream, ResumeToken start)
    : stream_(stream),
      position_(std::move(start)),
      skip_timestamp_micros_(position_.change_stream().timestamp_micros()),
      records_to_skip_(position_.change_stream().record_index()) {}

absl::Status ChangeRecordSender::Send(
    std::vector<spanner_api::PartialResultSet> responses,
    absl::Span<const absl::Time> record_timestamps) {
  return SendRecords(std::move(responses), record_timestamps,
                     /*heartbeat=*/false);
}

absl::Status ChangeRecordSender::Send(
    std::vector<spanner_api::PartialResultSet> responses,
    absl::Time timestamp) {
  const std::vector<int64_t> records =
      CompletedRows(responses, /*num_columns=*/1);
  const int64_t record_count =
      records.empty() ? 0 : *std::max_element(records.begin(), records.end());
  return SendRecords(
      std::move(responses),
      std::vector<absl::Time>(std::max<int64_t>(record_count, 0), timestamp),
      /*heartbeat=*/false);
}

absl::Status ChangeRecordSender::SendHeartbeat(
    std::vector<spanner_api::PartialResultSet> responses,
    absl::Time timestamp) {
  return SendRecords(std::move(responses), {timestamp}, /*heartbeat=*/true);
}

absl::Status ChangeRecordSender::SendRecords(
    std::vector<spanner_api::PartialResultSet> responses,
    absl::Span<const absl::Time> record_timestamps, bool heartbeat) {
  // A resumed stream returns the records at its start timestamp again, in the
  // same order. Heartbeats are not counted, as they only occur later.
  int64_t skipped = 0;
  while (!heartbeat && records_to_skip_ > 0 &&
         skipped < record_timestamps.size() &&
         absl::ToUnixMicros(record_timestamps[skipped]) ==
             skip_timestamp_micros_) {
    ++skipped;
    --records_to_skip_;
  }
  RemoveFirstRows(skipped, &responses);
  record_timestamps.remove_prefix(skipped);

  ResumeToken::ChangeStreamPosition& position =
      *position_.mutable_change_stream();
  const std::vector<int64_t> records =
      CompletedRows(responses, /*num_columns=*/1);
  int64_t sent = 0;
  for (int i = 0; i < responses.size(); ++i) {
    for (; sent < records[i]; ++sent) {
      GOOGLESQL_RET_CHECK_LT(sent, record_timestamps.size());
      const int64_t micros = absl::ToUnixMicros(record_timestamps[sent]);
      if (heartbeat) {
        // No record at or before a heartbeat's timestamp follows it.
        position.set_timestamp_micros(micros + 1);
        position.set_record_index(0);
      } else if (micros == position.timestamp_micros()) {
        position.set_record_index(position.record_index() + 1);
      } else {
        position.set_timestamp_micros(micros);
        position.set_record_index(1);
      }
    }
    if (records[i] >= 0) {
      responses[i].set_resume_token(position_.SerializeAsString());
    }
    stream_->Send(responses[i]);
  }
  GOOGLESQL_RET_CHECK_EQ(sent, record_timestamps.size());
  return absl::OkStatus();
}

absl::Status ChangeStreamsHandler::ProcessDataChangeRecordsAndStreamBack(
    backend::QueryResult& result, const bool expect_heartbeat,
    const absl::Time scan_end, bool& expect_metadata,
    absl::Time* last_record_time, ChangeRecordSender& sender) {
  std::vector<spanner_api::PartialResultSet> responses;
  const bool mutable_key_range =
      metadata().partition_mode ==
      backend::kChangeStreamPartitionModeMutableKeyRange;
  if (IsQueryResultEmpty(result) && expect_heartbeat) {
    if (metadata().is_pg) {
      GOOGLESQL_ASSIGN_OR_RETURN(
          responses, mutable_key_range
                         ? ConvertHeartbeatTimestampToBytes(
                               scan_end, metadata().tvf_name, expect_metadata)
                         : ConvertHeartbeatTimestampToJson(
                               scan_end, metadata().tvf_name, expect_metadata));
    } else {
      GOOGLESQL_ASSIGN_OR_RETURN(
          responses,
          mutable_key_range
              ? ConvertHeartbeatTimestampToProto(scan_end, expect_metadata)
              : ConvertHeartbeatTimestampToStruct(scan_end, expect_metadata));
    }
    expect_metadata = false;
    *last_record_time = scan_end;
    return sender.SendHeartbeat(std::move(responses), scan_end);
  }
  if (IsQueryResultEmpty(result)) {
    return absl::OkStatus();
  }
  std::vector<absl::Time> commit_timestamps;
  TimestampCollectingRowCursor rows(result.rows.get(), kCommitTimestampColumn,
                                    &commit_timestamps);
  if (metadata().is_pg) {
    GOOGLESQL_ASSIGN_OR_RETURN(
        responses, mutable_key_range
                       ? ConvertDataTableRowCursorToBytes(
                             &rows, metadata().tvf_name, expect_metadata)
                       : ConvertDataTableRowCursorToJson(
                             &rows, metadata().tvf_name, expect_metadata));
  } else {
    GOOGLESQL_ASSIGN_OR_RETURN(
        responses,
        mutable_key_range
            ? ConvertDataTableRowCursorToProto(&rows, expect_metadata)
            : ConvertDataTableRowCursorToStruct(&rows, expect_metadata));
  }
  *last_record_time = scan_end;
  expect_metadata = false;
  return sender.Send(std::move(responses), commit_timestamps);
}

absl::Status ChangeStreamsHandler::ExecuteInitialQuery(
    std::shared_ptr<Session> session, ChangeRecordSender& sender) {
  spanner_api::TransactionOptions txn_options;
  GOOGLESQL_ASSIGN_OR_RETURN(
      *txn_options.mutable_read_only()->mutable_min_read_timestamp(),
      TimestampToProto(metadata().start_timestamp));
  txn_options.mutable_read_only()->set_return_read_timestamp(false);
  GOOGLESQL_ASSIGN_OR_RETURN(auto txn, session->CreateSingleUseTransaction(txn_options));
  return txn->GuardedCall(Transaction::OpType::kSql, [&]() -> absl::Status {
    GOOGLESQL_RETURN_IF_ERROR(VerifyChangeStreamExistence(metadata().change_stream_name,
                                                txn->schema()));
    backend::Query initial_query = backend::Query{absl::Substitute(
        "SELECT start_time, partition_token, parents "
        "FROM $0 "
        "WHERE '$1' >= start_time AND ( end_time IS NULL OR '$1' < end_time "
        ")  ORDER BY (partition_token)",
        partition_table_, metadata().start_timestamp)};
    initial_query.change_stream_internal_lookup = metadata().change_stream_name;
    GOOGLESQL_ASSIGN_OR_RETURN(auto partition_results, txn->ExecuteSql(initial_query));
    // Validation keeps start_timestamp at or after the change stream's
    // creation time, when its first partitions start. If the recorded
    // creation time is earlier than those partitions (for example in data
    // persisted before database create times were recorded), no partition
    // covers start_timestamp: report it as out of range, not internal.
    if (IsQueryResultEmpty(partition_results)) {
      return absl::OutOfRangeError(absl::Substitute(
          "Specified start_timestamp is before the first partition of change "
          "stream $0. Received start_timestamp: $1.",
          metadata().change_stream_name,
          absl::FormatTime(metadata().start_timestamp)));
    }

    const bool mutable_key_range =
        metadata().partition_mode ==
        backend::kChangeStreamPartitionModeMutableKeyRange;
    GOOGLESQL_ASSIGN_OR_RETURN(
        auto responses,
        metadata().is_pg
            ? (mutable_key_range
                   ? ConvertPartitionTableRowCursorToBytes(
                         partition_results.rows.get(),
                         metadata().start_timestamp, /*partition_token=*/"",
                         metadata().tvf_name,
                         /*need_metadata=*/true)
                   : ConvertPartitionTableRowCursorToJson(
                         partition_results.rows.get(),
                         metadata().start_timestamp, metadata().tvf_name,
                         /*need_metadata=*/true))
            : (mutable_key_range
                   ? ConvertPartitionTableRowCursorToProto(
                         partition_results.rows.get(),
                         metadata().start_timestamp, /*partition_token=*/"",
                         /*need_metadata=*/true)
                   : ConvertPartitionTableRowCursorToStruct(
                         partition_results.rows.get(),
                         metadata().start_timestamp, /*need_metadata=*/true)));
    return sender.Send(std::move(responses), metadata().start_timestamp);
  });
}

absl::StatusOr<absl::Time> ChangeStreamsHandler::TryGetPartitionTokenEndTime(
    std::shared_ptr<Session> session, absl::Time read_ts) const {
  absl::Time start, end;
  spanner_api::TransactionOptions txn_options;
  txn_options.mutable_read_only()->set_return_read_timestamp(false);
  // If the user provided tvf start time is past now, wait until this future
  // time to perform a read on partition token end time.
  GOOGLESQL_ASSIGN_OR_RETURN(*txn_options.mutable_read_only()->mutable_read_timestamp(),
                   TimestampToProto(read_ts));
  GOOGLESQL_ASSIGN_OR_RETURN(auto txn, session->CreateSingleUseTransaction(txn_options));
  GOOGLESQL_RETURN_IF_ERROR(
      txn->GuardedCall(Transaction::OpType::kSql, [&]() -> absl::Status {
        backend::Query get_partition_token_time_query =
            backend::Query{absl::Substitute(
                "SELECT start_time, end_time "
                "FROM $0 "
                "WHERE( partition_token = '$1' )",
                partition_table_, metadata().partition_token.value())};
        get_partition_token_time_query.change_stream_internal_lookup =
            metadata().change_stream_name;
        GOOGLESQL_ASSIGN_OR_RETURN(auto token_time_results,
                         txn->ExecuteSql(get_partition_token_time_query));
        if (IsQueryResultEmpty(token_time_results)) {
          return error::
              InvalidChangeStreamTvfArgumentPartitionTokenInvalidChangeStreamName(  // NOLINT
                  metadata().partition_token.value());
        }
        backend::RowCursor* cursor = token_time_results.rows.get();
        cursor->Next();
        start = cursor->ColumnValue(0).ToTime();
        // If the end_time of current partition token is null, set the returning
        // end time to InfiniteFuture().
        end = cursor->ColumnValue(1).is_null()
                  ? absl::InfiniteFuture()
                  : cursor->ColumnValue(1).ToTime();
        // Return error if user provided tvf start time is not within the
        // lifetime of the user provided partition token.
        if (metadata().start_timestamp < start ||
            metadata().start_timestamp > end) {
          return error::
              InvalidChangeStreamTvfArgumentStartTimestampForPartition(
                  absl::FormatTime(start), absl::FormatTime(end),
                  absl::FormatTime(metadata().start_timestamp));
        }
        return absl::OkStatus();
      }));
  return end;
}

backend::Query ChangeStreamsHandler::ConstructDataTablePartitionQuery(
    absl::Time start, absl::Time end) const {
  // If user passed end_timestamp is not null and current scan is the last scan
  // in query lifetime, we do an inclusive scan to include the data change
  // record with commit_timestamp exactly at the user passed end_timestamp. If
  // current scan is a middle chopped scan, we do an exclusive scan because all
  // data records of a partition token has a commit_timestamp in
  // [partition_start_time,partition_end_time).
  const bool is_inclusive_read = metadata().end_timestamp.has_value() &&
                                 metadata().end_timestamp.value() == end;
  backend::Query data_table_partition_query = backend::Query{absl::Substitute(
      "SELECT * "
      "FROM $0 "
      "WHERE( partition_token='$1' AND commit_timestamp >= '$2' AND "
      "commit_timestamp $3 '$4' ) ORDER BY partition_token, commit_timestamp, "
      "server_transaction_id,record_sequence",
      metadata().data_table, metadata().partition_token.value(), start,
      is_inclusive_read ? "<=" : "<", end)};
  data_table_partition_query.change_stream_internal_lookup =
      metadata().change_stream_name;
  return data_table_partition_query;
}

backend::Query ChangeStreamsHandler::ConstructPartitionTablePartitionQuery()
    const {
  backend::Query data_table_partition_query = backend::Query{absl::Substitute(
      "SELECT start_time, partition_token, parents FROM $0 "
      "WHERE (ARRAY_INCLUDES((SELECT children FROM $0 WHERE "
      "partition_token = '$1'), partition_token)) ORDER BY(partition_token)",
      partition_table_, metadata().partition_token.value())};
  data_table_partition_query.change_stream_internal_lookup =
      metadata().change_stream_name;
  return data_table_partition_query;
}

backend::Query
ChangeStreamsHandler::ConstructQueryStartPartitionTablePartitionQuery() const {
  backend::Query query_partition_record = backend::Query{
      absl::Substitute("SELECT start_time, partition_token, parents FROM $0 "
                       "WHERE partition_token = '$1'",
                       partition_table_, metadata().partition_token.value())};
  query_partition_record.change_stream_internal_lookup =
      metadata().change_stream_name;
  return query_partition_record;
}

absl::Status ChangeStreamsHandler::ExecutePartitionQuery(
    absl::Time start, ChangeRecordSender& sender,
    std::shared_ptr<Session> session) {
  const bool mutable_key_range =
      metadata().partition_mode ==
      backend::kChangeStreamPartitionModeMutableKeyRange;
  const absl::Time tvf_end = metadata().end_timestamp.has_value()
                                 ? metadata().end_timestamp.value()
                                 : absl::InfiniteFuture();
  const absl::Time now = Clock().Now();
  const absl::Duration heartbeat_interval =
      absl::Milliseconds(metadata().heartbeat_milliseconds);
  absl::Time last_record_time = now;
  absl::Time partition_token_end_time = absl::InfiniteFuture();
  absl::Time current_start = start;
  absl::Time current_end = std::min(
      std::max(now,
               current_start +
                   absl::GetFlag(
                       FLAGS_change_streams_partition_query_chop_interval)),
      tvf_end);
  // Metadata is only expected for the first response to users in a single
  // query's lifetime.
  bool expect_metadata = true;
  while (current_start <= tvf_end && current_start < partition_token_end_time) {
    // For historical queries where tvf end is in the past, set the read
    // transaction snapshot time to now to prevent >1h stale read, which is now
    // allowed.
    absl::Time current_txn_snapshot_time = std::max(current_end, now);
    // Get the newest retention period so most up to date retention will apply
    // to curent running query.
    GOOGLESQL_ASSIGN_OR_RETURN(
        absl::Duration current_retention,
        TryGetChangeStreamRetentionPeriod(metadata().change_stream_name,
                                          session, current_txn_snapshot_time));
    spanner_api::TransactionOptions txn_options;
    // If the partition token hasn't been churned yet, we re-scan the partition
    // table to see if the end time has been churned and update the partition
    // end time.
    if (partition_token_end_time == absl::InfiniteFuture()) {
      GOOGLESQL_ASSIGN_OR_RETURN(
          partition_token_end_time,
          TryGetPartitionTokenEndTime(session, current_txn_snapshot_time));
    }
    GOOGLESQL_RETURN_IF_ERROR(ValidateTokenInRetentionWindow(
        metadata().start_timestamp, current_start, partition_token_end_time,
        current_retention));
    // Only scan data records up to minimum of current chopped end time
    // and end time of current partition token.
    const absl::Time scan_end = std::min(partition_token_end_time, current_end);
    const bool expect_heartbeat =
        current_end - last_record_time >= heartbeat_interval;
    // This transaction will be blocked until now passes current_end.
    GOOGLESQL_ASSIGN_OR_RETURN(*txn_options.mutable_read_only()->mutable_read_timestamp(),
                     TimestampToProto(current_txn_snapshot_time));
    GOOGLESQL_ASSIGN_OR_RETURN(auto txn,
                     session->CreateSingleUseTransaction(txn_options));
    absl::Status status =
        txn->GuardedCall(Transaction::OpType::kSql, [&]() -> absl::Status {
          // The partition's start records precede its other records, so they
          // are returned from the first scan only.
          if (mutable_key_range && current_start == start) {
            backend::Query head_query_partition_table =
                ConstructQueryStartPartitionTablePartitionQuery();
            GOOGLESQL_ASSIGN_OR_RETURN(auto head_partition_records_results,
                             txn->ExecuteSql(head_query_partition_table));
            if (!IsQueryResultEmpty(head_partition_records_results)) {
              std::vector<absl::Time> partition_start;
              TimestampCollectingRowCursor rows(
                  head_partition_records_results.rows.get(),
                  kPartitionStartTimeColumn, &partition_start);
              GOOGLESQL_ASSIGN_OR_RETURN(
                  auto responses,
                  metadata().is_pg
                      ? ConvertQueryStartPartitionTableRowCursorToBytes(
                            &rows, start, metadata().tvf_name,
                            expect_metadata)
                      : ConvertQueryStartPartitionTableRowCursorToProto(
                            &rows, start, expect_metadata));
              if (!responses.empty()) {
                expect_metadata = false;
                // The query reads the one row of the partition.
                GOOGLESQL_RETURN_IF_ERROR(sender.Send(std::move(responses),
                                            partition_start.front()));
              }
            }
          }
          backend::Query read_data_query =
              ConstructDataTablePartitionQuery(current_start, scan_end);
          GOOGLESQL_ASSIGN_OR_RETURN(auto data_records_results,
                           txn->ExecuteSql(read_data_query));
          GOOGLESQL_RETURN_IF_ERROR(ProcessDataChangeRecordsAndStreamBack(
              data_records_results, expect_heartbeat, scan_end, expect_metadata,
              &last_record_time, sender));
          if (partition_token_end_time <= current_end) {
            // Get child partition records after all data records are returned
            // in current query.
            backend::Query tail_query_partition_table =
                ConstructPartitionTablePartitionQuery();
            GOOGLESQL_ASSIGN_OR_RETURN(auto tail_partition_records_results,
                             txn->ExecuteSql(tail_query_partition_table));
            GOOGLESQL_RET_CHECK(!IsQueryResultEmpty(tail_partition_records_results));
            GOOGLESQL_ASSIGN_OR_RETURN(
                auto responses,
                metadata().is_pg
                    ? (mutable_key_range
                           ? ConvertPartitionTableRowCursorToBytes(
                                 tail_partition_records_results.rows.get(),
                                 /*initial_start_time=*/std::nullopt,
                                 metadata().partition_token.value(),
                                 metadata().tvf_name, expect_metadata)
                           : ConvertPartitionTableRowCursorToJson(
                                 tail_partition_records_results.rows.get(),
                                 /*initial_start_time=*/std::nullopt,
                                 metadata().tvf_name, expect_metadata))
                    : (mutable_key_range
                           ? ConvertPartitionTableRowCursorToProto(
                                 tail_partition_records_results.rows.get(),
                                 /*initial_start_time=*/std::nullopt,
                                 metadata().partition_token.value(),
                                 expect_metadata)
                           : ConvertPartitionTableRowCursorToStruct(
                                 tail_partition_records_results.rows.get(),
                                 /*initial_start_time=*/std::nullopt,
                                 expect_metadata)));
            expect_metadata = false;
            return sender.Send(std::move(responses), partition_token_end_time);
          }
          return absl::OkStatus();
        });
    GOOGLESQL_RETURN_IF_ERROR(status);
    if (scan_end >= tvf_end || scan_end >= partition_token_end_time ||
        scan_end <= current_start) {
      break;
    }
    // Advance to scan_end directly without a gap to ensure no boundary commits are skipped.
    current_start = scan_end;
    current_end = std::min(
        {current_start +
             absl::GetFlag(FLAGS_change_streams_partition_query_chop_interval),
         tvf_end, partition_token_end_time});
  }

  // If expect_metadata is still true, stub a heartbeat record.
  if (expect_metadata == true) {
    GOOGLESQL_ASSIGN_OR_RETURN(
        auto extra_heartbeat,
        metadata().is_pg
            ? (mutable_key_range
                   ? ConvertHeartbeatTimestampToBytes(
                         tvf_end, metadata().tvf_name, expect_metadata)
                   : ConvertHeartbeatTimestampToJson(
                         tvf_end, metadata().tvf_name, expect_metadata))
            : (mutable_key_range
                   ? ConvertHeartbeatTimestampToProto(tvf_end, expect_metadata)
                   : ConvertHeartbeatTimestampToStruct(tvf_end,
                                                       expect_metadata)));
    GOOGLESQL_RETURN_IF_ERROR(
        sender.SendHeartbeat(std::move(extra_heartbeat), tvf_end));
  }
  return absl::OkStatus();
}

absl::Status ChangeStreamsHandler::ExecuteChangeStreamQuery(
    const spanner_api::ExecuteSqlRequest* request,
    ServerStream<spanner_api::PartialResultSet>* stream,
    std::shared_ptr<Session> session,
    const std::optional<ResumeToken>& resume_token) {
  GOOGLESQL_RETURN_IF_ERROR(
      ValidateTransactionSelectorForChangeStreamQuery(request->transaction()));
  if (request->query_mode() == spanner_api::ExecuteSqlRequest::PLAN) {
    return error::EmulatorDoesNotSupportQueryPlans();
  }
  const std::string partition_token =
      metadata().partition_token.value_or("");
  ResumeToken start;
  if (resume_token.has_value()) {
    if (!resume_token->has_change_stream() ||
        resume_token->change_stream().partition_token() != partition_token) {
      return error::ResumeTokenMismatch();
    }
    start = *resume_token;
  } else {
    start.set_request_fingerprint(ResumeFingerprint(*request));
    start.mutable_change_stream()->set_partition_token(partition_token);
  }
  ChangeRecordSender sender(stream, start);
  if (!metadata().partition_token.has_value()) {
    return ExecuteInitialQuery(session, sender);
  }
  // A resumed query continues at the timestamp of its resume token.
  return ExecutePartitionQuery(
      std::max(metadata().start_timestamp,
               absl::FromUnixMicros(start.change_stream().timestamp_micros())),
      sender, session);
}
}  // namespace frontend
}  // namespace emulator
}  // namespace spanner
}  // namespace google

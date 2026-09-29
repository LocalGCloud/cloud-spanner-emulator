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

#ifndef THIRD_PARTY_CLOUD_SPANNER_EMULATOR_FRONTEND_HANDLERS_CHANGE_STREAMS_H_
#define THIRD_PARTY_CLOUD_SPANNER_EMULATOR_FRONTEND_HANDLERS_CHANGE_STREAMS_H_

#include <cstdint>
#include <memory>
#include <optional>
#include <string>
#include <vector>

#include "google/spanner/v1/result_set.pb.h"
#include "google/spanner/v1/spanner.pb.h"
#include "absl/flags/declare.h"
#include "absl/flags/flag.h"
#include "absl/status/status.h"
#include "absl/status/statusor.h"
#include "absl/time/time.h"
#include "absl/types/span.h"
#include "backend/query/change_stream/change_stream_query_validator.h"
#include "backend/query/query_engine.h"
#include "frontend/entities/session.h"
#include "frontend/proto/resume_token.pb.h"
#include "frontend/server/handler.h"

ABSL_DECLARE_FLAG(bool, cloud_spanner_emulator_test_with_fake_partition_table);
ABSL_DECLARE_FLAG(absl::Duration,
                  change_streams_partition_query_chop_interval);

namespace google {
namespace spanner {
namespace emulator {
namespace frontend {

// Streams the change records of a change stream query. Each PartialResultSet
// that ends on a record boundary gets a resume token with the position after
// its records. A query that resumes a stream skips the records that the
// stream returned before the position of its resume token.
class ChangeRecordSender {
 public:
  // Starts streaming at the position of `start`.
  ChangeRecordSender(ServerStream<spanner_api::PartialResultSet>* stream,
                     ResumeToken start);

  // Sends `responses`, which hold change records with `record_timestamps`.
  absl::Status Send(std::vector<spanner_api::PartialResultSet> responses,
                    absl::Span<const absl::Time> record_timestamps);

  // Sends `responses`, which hold change records at `timestamp`.
  absl::Status Send(std::vector<spanner_api::PartialResultSet> responses,
                    absl::Time timestamp);

  // Sends `responses`, which hold a heartbeat record at `timestamp`. A stream
  // that resumes after it continues after `timestamp`.
  absl::Status SendHeartbeat(
      std::vector<spanner_api::PartialResultSet> responses,
      absl::Time timestamp);

 private:
  absl::Status SendRecords(std::vector<spanner_api::PartialResultSet> responses,
                           absl::Span<const absl::Time> record_timestamps,
                           bool heartbeat);

  ServerStream<spanner_api::PartialResultSet>* stream_;
  // The position after the records sent so far.
  ResumeToken position_;
  // How many more records at `skip_timestamp_micros_` the resumed stream
  // returned already.
  int64_t skip_timestamp_micros_;
  int64_t records_to_skip_;
};

// Sub-handler for change stream queries. There is no direct grpc request
// registered with this handler. Rather, if an incoming sql query is detected
// as a change stream query, we wire the query from the generic
// ExecuteStreamingSql handler to this specific handler.
class ChangeStreamsHandler {
 public:
  static constexpr char kTestPartitionTable[] = "partition_table";
  static constexpr char kTestDataTable[] = "data_table";
  explicit ChangeStreamsHandler(
      backend::ChangeStreamQueryValidator::ChangeStreamMetadata& metadata)
      : metadata_(metadata) {
    // Name of the partition&data table to be read from. For certain test cases
    // this need to be set to test only mock tables.
    partition_table_ =
        absl::GetFlag(
            FLAGS_cloud_spanner_emulator_test_with_fake_partition_table)
            ? kTestPartitionTable
            : metadata.partition_table;
  }

  // Executes the change stream query `request`, which resumes the stream that
  // returned `resume_token`, if any.
  absl::Status ExecuteChangeStreamQuery(
      const spanner_api::ExecuteSqlRequest* request,
      ServerStream<spanner_api::PartialResultSet>* stream,
      std::shared_ptr<Session> session,
      const std::optional<ResumeToken>& resume_token);

  // Execute change stream initial query when partition token is null.
  absl::Status ExecuteInitialQuery(std::shared_ptr<Session> session,
                                   ChangeRecordSender& sender);

  absl::StatusOr<absl::Time> TryGetPartitionTokenEndTime(
      std::shared_ptr<Session> session, absl::Time read_ts) const;

  // Execute change stream partition query when partition token is non null,
  // returning the records from `start` on.
  absl::Status ExecutePartitionQuery(absl::Time start,
                                     ChangeRecordSender& sender,
                                     std::shared_ptr<Session> session);

  backend::Query ConstructPartitionTablePartitionQuery() const;

  // Constructs a query on the partition table to retrieve the partition record
  // (start_time, partition_token, parents) for the current partition. Used in
  // mutable key range mode at query start to emit move-in partition event
  // records if the query start time is at or before the partition start time.
  backend::Query ConstructQueryStartPartitionTablePartitionQuery() const;

  backend::Query ConstructDataTablePartitionQuery(absl::Time start,
                                                  absl::Time end) const;

  absl::Status ProcessDataChangeRecordsAndStreamBack(
      backend::QueryResult& result, bool expect_heartbeat, absl::Time scan_end,
      bool& expect_metadata, absl::Time* last_record_time,
      ChangeRecordSender& sender);

  const backend::ChangeStreamQueryValidator::ChangeStreamMetadata& metadata()
      const {
    return metadata_;
  }

 private:
  const backend::ChangeStreamQueryValidator::ChangeStreamMetadata& metadata_;
  std::string partition_table_;
};
}  // namespace frontend
}  // namespace emulator
}  // namespace spanner
}  // namespace google

#endif  // THIRD_PARTY_CLOUD_SPANNER_EMULATOR_FRONTEND_HANDLERS_CHANGE_STREAMS_H_

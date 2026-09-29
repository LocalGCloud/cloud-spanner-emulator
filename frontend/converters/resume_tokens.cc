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

#include "frontend/converters/resume_tokens.h"

#include <algorithm>
#include <cstdint>
#include <string>
#include <vector>

#include "google/protobuf/io/coded_stream.h"
#include "google/protobuf/io/zero_copy_stream_impl_lite.h"
#include "google/protobuf/message.h"
#include "google/spanner/v1/result_set.pb.h"
#include "google/spanner/v1/spanner.pb.h"
#include "google/spanner/v1/transaction.pb.h"
#include "absl/status/statusor.h"
#include "absl/strings/string_view.h"
#include "absl/time/time.h"
#include "absl/types/span.h"
#include "common/errors.h"
#include "farmhash.h"
#include "frontend/converters/time.h"
#include "frontend/proto/resume_token.pb.h"
#include "googlesql/base/status_macros.h"

namespace google {
namespace spanner {
namespace emulator {
namespace frontend {

namespace spanner_api = ::google::spanner::v1;

uint64_t DeterministicFingerprint(const google::protobuf::Message& message) {
  // Message::SerializeToString() does not guarantee a deterministic order of
  // map fields. Flush the output stream before fingerprinting.
  std::string serialized;
  {
    google::protobuf::io::StringOutputStream stream(&serialized);
    google::protobuf::io::CodedOutputStream output(&stream);
    output.SetSerializationDeterministic(true);
    message.SerializeToCodedStream(&output);
  }
  return farmhash::Fingerprint64(serialized);
}

uint64_t ResumeFingerprint(const spanner_api::ExecuteSqlRequest& request) {
  spanner_api::ExecuteSqlRequest copy = request;
  copy.clear_resume_token();
  copy.clear_transaction();
  copy.clear_request_options();
  copy.clear_seqno();
  return DeterministicFingerprint(copy);
}

uint64_t ResumeFingerprint(const spanner_api::ReadRequest& request) {
  spanner_api::ReadRequest copy = request;
  copy.clear_resume_token();
  copy.clear_transaction();
  copy.clear_request_options();
  return DeterministicFingerprint(copy);
}

bool IsSingleUseTransaction(const spanner_api::TransactionSelector& selector) {
  return selector.selector_case() ==
             spanner_api::TransactionSelector::kSingleUse ||
         selector.selector_case() ==
             spanner_api::TransactionSelector::SELECTOR_NOT_SET;
}

absl::StatusOr<ResumeToken> ParseResumeToken(absl::string_view token,
                                             uint64_t request_fingerprint) {
  ResumeToken resume_token;
  if (!resume_token.ParseFromString(token) ||
      resume_token.position_case() == ResumeToken::POSITION_NOT_SET) {
    return error::InvalidResumeToken();
  }
  if (resume_token.request_fingerprint() != request_fingerprint) {
    return error::ResumeTokenMismatch();
  }
  return resume_token;
}

absl::StatusOr<spanner_api::TransactionSelector> ResumedTransactionSelector(
    const spanner_api::TransactionSelector& selector,
    const ResumeToken& token) {
  if (!token.has_rows()) {
    return error::ResumeTokenMismatch();
  }
  if (!IsSingleUseTransaction(selector)) {
    // The stream's transaction must be selected by its ID, which the handler
    // checks. Beginning a transaction would resume in another one.
    if (selector.selector_case() != spanner_api::TransactionSelector::kId ||
        token.rows().transaction_id() == 0) {
      return error::ResumeTokenMismatch();
    }
    return selector;
  }
  if (token.rows().transaction_id() != 0) {
    return error::ResumeTokenMismatch();
  }
  spanner_api::TransactionSelector resumed = selector;
  GOOGLESQL_ASSIGN_OR_RETURN(
      *resumed.mutable_single_use()
           ->mutable_read_only()
           ->mutable_read_timestamp(),
      TimestampToProto(
          absl::FromUnixMicros(token.rows().read_timestamp_micros())));
  return resumed;
}

std::vector<int64_t> CompletedRows(
    absl::Span<const spanner_api::PartialResultSet> responses,
    int num_columns) {
  std::vector<int64_t> completed_rows;
  completed_rows.reserve(responses.size());
  // Values started so far. A chunked value continues in the next response.
  int64_t values = 0;
  bool continues_value = false;
  for (const spanner_api::PartialResultSet& response : responses) {
    values += response.values_size() - (continues_value ? 1 : 0);
    continues_value = response.chunked_value();
    const int64_t complete_values = values - (continues_value ? 1 : 0);
    const bool row_boundary = !continues_value && num_columns > 0 &&
                              response.values_size() > 0 &&
                              complete_values % num_columns == 0;
    completed_rows.push_back(row_boundary ? complete_values / num_columns
                                          : -1);
  }
  return completed_rows;
}

void RemoveFirstRows(int64_t count,
                     std::vector<spanner_api::PartialResultSet>* responses) {
  for (spanner_api::PartialResultSet& response : *responses) {
    if (count == 0) {
      break;
    }
    // Each value completes a row, except for a chunked last value, which the
    // next response continues.
    const int values = response.values_size();
    int removed = 0;
    while (removed < values && count > 0) {
      if (removed < values - 1 || !response.chunked_value()) {
        --count;
      }
      ++removed;
    }
    response.mutable_values()->DeleteSubrange(0, removed);
    if (removed == values) {
      response.set_chunked_value(false);
    }
  }
  responses->erase(
      std::remove_if(responses->begin(), responses->end(),
                     [](const spanner_api::PartialResultSet& response) {
                       return response.values().empty() &&
                              !response.has_metadata() &&
                              !response.has_stats();
                     }),
      responses->end());
}

}  // namespace frontend
}  // namespace emulator
}  // namespace spanner
}  // namespace google

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

#ifndef THIRD_PARTY_CLOUD_SPANNER_EMULATOR_FRONTEND_CONVERTERS_RESUME_TOKENS_H_
#define THIRD_PARTY_CLOUD_SPANNER_EMULATOR_FRONTEND_CONVERTERS_RESUME_TOKENS_H_

#include <cstdint>
#include <vector>

#include "google/protobuf/message.h"
#include "google/spanner/v1/result_set.pb.h"
#include "google/spanner/v1/spanner.pb.h"
#include "google/spanner/v1/transaction.pb.h"
#include "absl/status/statusor.h"
#include "absl/strings/string_view.h"
#include "absl/types/span.h"
#include "frontend/proto/resume_token.pb.h"

namespace google {
namespace spanner {
namespace emulator {
namespace frontend {

// Returns a fingerprint of `message` that is the same for equal messages.
uint64_t DeterministicFingerprint(const google::protobuf::Message& message);

// Returns the fingerprint that ties resume tokens to the request of their
// stream. It covers the request fields that determine the stream's results. It
// doesn't cover the transaction selector, which a client switches from
// beginning a transaction to the transaction's ID when it resumes a stream,
// nor the request options and the sequence number.
uint64_t ResumeFingerprint(
    const google::spanner::v1::ExecuteSqlRequest& request);
uint64_t ResumeFingerprint(const google::spanner::v1::ReadRequest& request);

// Returns true if `selector` selects a single-use transaction, which a request
// without a selector uses too.
bool IsSingleUseTransaction(
    const google::spanner::v1::TransactionSelector& selector);

// Parses the resume token of a request with the ResumeFingerprint()
// `request_fingerprint`.
absl::StatusOr<ResumeToken> ParseResumeToken(absl::string_view token,
                                             uint64_t request_fingerprint);

// Returns the transaction in which a stream of rows resumes at `token`: the
// transaction of the stream that returned the token, as `selector` selects
// it. A single-use transaction reads at the timestamp of that stream.
absl::StatusOr<google::spanner::v1::TransactionSelector>
ResumedTransactionSelector(
    const google::spanner::v1::TransactionSelector& selector,
    const ResumeToken& token);

// Returns, for each of `responses`, consecutive PartialResultSets of a stream
// with `num_columns` columns, the number of rows that the responses complete
// by its end, or -1 if it does not end on a row boundary. Clients resume
// streams only after responses that end on a row boundary and have values.
std::vector<int64_t> CompletedRows(
    absl::Span<const google::spanner::v1::PartialResultSet> responses,
    int num_columns);

// Removes the first `count` rows of `responses`, consecutive PartialResultSets
// of a stream with a single column. Keeps responses that carry metadata or
// statistics even if they no longer have values.
void RemoveFirstRows(
    int64_t count,
    std::vector<google::spanner::v1::PartialResultSet>* responses);

}  // namespace frontend
}  // namespace emulator
}  // namespace spanner
}  // namespace google

#endif  // THIRD_PARTY_CLOUD_SPANNER_EMULATOR_FRONTEND_CONVERTERS_RESUME_TOKENS_H_

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

#include <cstdint>
#include <string>
#include <vector>

#include "google/spanner/v1/result_set.pb.h"
#include "google/spanner/v1/spanner.pb.h"
#include "google/spanner/v1/transaction.pb.h"
#include "gmock/gmock.h"
#include "gtest/gtest.h"
#include "googlesql/base/testing/status_matchers.h"
#include "googlesql/public/types/type_factory.h"
#include "googlesql/public/value.h"
#include "tests/common/proto_matchers.h"
#include "absl/status/status.h"
#include "absl/strings/str_cat.h"
#include "absl/time/time.h"
#include "common/limits.h"
#include "frontend/converters/reads.h"
#include "frontend/proto/resume_token.pb.h"
#include "tests/common/chunking.h"
#include "tests/common/row_cursor.h"

namespace google {
namespace spanner {
namespace emulator {
namespace frontend {

namespace {

namespace spanner_api = ::google::spanner::v1;

using ::google::spanner::emulator::test::EqualsProto;
using ::googlesql_base::testing::IsOkAndHolds;
using ::googlesql_base::testing::StatusIs;
using ::testing::ElementsAre;

spanner_api::PartialResultSet Response(int values, bool chunked_value) {
  spanner_api::PartialResultSet response;
  for (int i = 0; i < values; ++i) {
    response.add_values()->set_string_value(std::to_string(i));
  }
  response.set_chunked_value(chunked_value);
  return response;
}

TEST(ResumeTokensTest, FindsRowBoundaries) {
  // Two columns: rows end after values 2, 4 and 6. The second response ends
  // inside a row, and the third one ends inside a chunked value.
  EXPECT_THAT(CompletedRows({Response(2, false), Response(1, false),
                             Response(2, true), Response(2, false),
                             Response(0, false)},
                            /*num_columns=*/2),
              ElementsAre(1, -1, -1, 3, -1));
}

TEST(ResumeTokensTest, RemovesFirstRowsOfSingleColumnStreams) {
  std::vector<spanner_api::PartialResultSet> responses = {
      Response(2, false), Response(2, true), Response(2, false)};
  responses[0].mutable_metadata();
  // Removes rows 0 to 3. Row 3 is chunked across the last two responses.
  RemoveFirstRows(4, &responses);
  ASSERT_EQ(responses.size(), 2);
  EXPECT_TRUE(responses[0].has_metadata());
  EXPECT_TRUE(responses[0].values().empty());
  EXPECT_FALSE(responses[0].chunked_value());
  EXPECT_THAT(responses[1], EqualsProto(R"pb(values { string_value: "1" })pb"));
}

TEST(ResumeTokensTest, ParsesTokensOfTheSameRequestOnly) {
  spanner_api::ExecuteSqlRequest request;
  request.set_session("session");
  request.set_sql("SELECT 1");
  ResumeToken token;
  token.set_request_fingerprint(ResumeFingerprint(request));
  token.mutable_rows()->set_row_count(1);

  // The transaction, options and sequence number don't matter.
  spanner_api::ExecuteSqlRequest resumed = request;
  resumed.mutable_transaction()->set_id("transaction");
  resumed.mutable_request_options()->set_request_tag("retry");
  resumed.set_seqno(2);
  EXPECT_THAT(ParseResumeToken(token.SerializeAsString(),
                               ResumeFingerprint(resumed)),
              IsOkAndHolds(EqualsProto(token)));

  resumed.set_sql("SELECT 2");
  EXPECT_THAT(ParseResumeToken(token.SerializeAsString(),
                               ResumeFingerprint(resumed)),
              StatusIs(absl::StatusCode::kInvalidArgument));
  EXPECT_THAT(ParseResumeToken("\xff", ResumeFingerprint(request)),
              StatusIs(absl::StatusCode::kInvalidArgument));
}

TEST(ResumeTokensTest, ResumesSingleUseTransactionsAtTheirReadTimestamp) {
  ResumeToken token;
  token.mutable_rows()->set_read_timestamp_micros(1234567);
  spanner_api::TransactionSelector selector;
  selector.mutable_single_use()->mutable_read_only()->set_strong(true);
  EXPECT_THAT(ResumedTransactionSelector(selector, token),
              IsOkAndHolds(EqualsProto(R"pb(
                single_use {
                  read_only { read_timestamp { seconds: 1 nanos: 234567000 } }
                }
              )pb")));

  // A stream in a multi-use transaction resumes in it only.
  token.mutable_rows()->set_transaction_id(7);
  EXPECT_THAT(ResumedTransactionSelector(selector, token),
              StatusIs(absl::StatusCode::kInvalidArgument));
  selector.set_id("7");
  GOOGLESQL_EXPECT_OK(ResumedTransactionSelector(selector, token));
  selector.mutable_begin()->mutable_read_only();
  EXPECT_THAT(ResumedTransactionSelector(selector, token),
              StatusIs(absl::StatusCode::kInvalidArgument));
}

TEST(ResumeTokensTest, ResumesRowsAfterTheirToken) {
  const std::string value(limits::kMaxStreamingChunkSize / 3, 'x');
  std::vector<std::vector<googlesql::Value>> rows;
  for (int i = 0; i < 10; ++i) {
    rows.push_back({googlesql::values::Int64(i), googlesql::values::String(
                                                     value)});
  }
  auto cursor = [&rows] {
    return test::TestRowCursor(
        {"key", "value"},
        {googlesql::types::Int64Type(), googlesql::types::StringType()}, rows);
  };
  ResumeToken start;
  start.mutable_rows();
  test::TestRowCursor all_rows = cursor();
  GOOGLESQL_ASSERT_OK_AND_ASSIGN(
      std::vector<spanner_api::PartialResultSet> original,
      RowCursorToPartialResultSetProtos(&all_rows, /*limit=*/0, start));
  // Responses hold as many whole rows as fit.
  ASSERT_EQ(original.size(), 5);
  for (const spanner_api::PartialResultSet& response : original) {
    EXPECT_EQ(response.values_size(), 4);
    EXPECT_FALSE(response.chunked_value());
    EXPECT_FALSE(response.resume_token().empty());
  }
  EXPECT_TRUE(original.front().has_metadata());
  EXPECT_FALSE(original.back().has_metadata());

  int resumed_streams = 0;
  for (const spanner_api::PartialResultSet& response : original) {
    if (response.resume_token().empty()) {
      continue;
    }
    ++resumed_streams;
    ASSERT_TRUE(start.ParseFromString(response.resume_token()));
    test::TestRowCursor rest = cursor();
    GOOGLESQL_ASSERT_OK_AND_ASSIGN(
        std::vector<spanner_api::PartialResultSet> resumed,
        RowCursorToPartialResultSetProtos(&rest, /*limit=*/0, start));
    GOOGLESQL_ASSERT_OK_AND_ASSIGN(
        spanner_api::ResultSet result,
        backend::test::MergePartialResultSets(resumed, /*columns_per_row=*/2));
    ASSERT_EQ(result.rows_size(), rows.size() - start.rows().row_count());
    if (result.rows_size() > 0) {
      EXPECT_THAT(result.rows(0).values(0),
                  EqualsProto(absl::StrCat("string_value: \"",
                                           start.rows().row_count(), "\"")));
    }
  }
  EXPECT_EQ(resumed_streams, 5);

  // Other rows before the token don't resume the stream.
  rows[0][1] = googlesql::values::String("changed");
  test::TestRowCursor changed = cursor();
  EXPECT_THAT(RowCursorToPartialResultSetProtos(&changed, /*limit=*/0, start),
              StatusIs(absl::StatusCode::kFailedPrecondition));
}

}  // namespace

}  // namespace frontend
}  // namespace emulator
}  // namespace spanner
}  // namespace google

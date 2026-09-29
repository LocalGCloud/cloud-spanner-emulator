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

#ifndef THIRD_PARTY_CLOUD_SPANNER_EMULATOR_FRONTEND_COMMON_LIST_FILTER_H_
#define THIRD_PARTY_CLOUD_SPANNER_EMULATOR_FRONTEND_COMMON_LIST_FILTER_H_

#include <string>
#include <string_view>
#include <vector>

#include "google/longrunning/operations.pb.h"
#include "google/protobuf/map.h"
#include "absl/functional/function_ref.h"
#include "absl/status/status.h"
#include "absl/status/statusor.h"

namespace google {
namespace spanner {
namespace emulator {
namespace frontend {

// Resolves one `field op value` predicate against the item being filtered.
// `field` is lower-cased and `op` is one of = != < <= > >= :. Returns
// INVALID_ARGUMENT for an unsupported field or a malformed value.
using ListFilterResolver = absl::FunctionRef<absl::StatusOr<bool>(
    std::string_view field, std::string_view op, std::string_view value)>;

// The filter language of the Cloud Spanner List* RPCs (AIP-160 subset).
//
// A predicate is `field op value`. The value is a bare word or a string in
// double or single quotes (backslash escapes the quote and the backslash).
// Predicates combine with AND, OR, NOT and parentheses, and adjacent
// predicates are joined with an implicit AND. Following AIP-160, OR binds
// tighter than AND, so `a AND b OR c` means `a AND (b OR c)`. Keywords and
// field names are case-insensitive.
class ListFilter {
 public:
  // Parses `expression`. Returns INVALID_ARGUMENT for invalid syntax or an
  // expression over the length, term or nesting limits. An empty expression
  // matches everything.
  static absl::StatusOr<ListFilter> Parse(std::string_view expression);

  // Evaluates the filter, resolving each predicate with `resolver`. Every
  // predicate is resolved, so resolver errors do not depend on the data.
  absl::StatusOr<bool> Matches(ListFilterResolver resolver) const;

 private:
  struct Token {
    std::string text;
    bool quoted;
  };
  class Evaluator;

  ListFilter() = default;
  absl::Status Tokenize(std::string_view expression);

  std::vector<Token> tokens_;
};

// Compares `left op right`. Returns false for an unknown operator.
template <typename T>
bool CompareOrdered(const T& left, const T& right, std::string_view op) {
  if (op == "=") return left == right;
  if (op == "!=") return left != right;
  if (op == "<") return left < right;
  if (op == "<=") return left <= right;
  if (op == ">") return left > right;
  if (op == ">=") return left >= right;
  return false;
}

// Compares strings ignoring ASCII case. `:` is "contains", and `:*` tests that
// `left` is not empty.
bool CompareString(std::string_view left, std::string_view right,
                   std::string_view op);

// Resolves `labels.<key>` for a resource with `labels`: `:*` tests that the
// label exists, and other operators compare its value with CompareString. A
// missing label only satisfies `!=`.
absl::StatusOr<bool> MatchLabel(
    const google::protobuf::Map<std::string, std::string>& labels,
    std::string_view key, std::string_view op, std::string_view value);

// Evaluates `filter` against a long-running operation. The fields are the ones
// documented for the Spanner operation lists: name, done, error,
// metadata.@type, metadata.<path>, response.@type and response.<path>, plus
// error.code and error.message. Field paths resolve by reflection on the
// packed metadata and response messages; segments match field names ignoring
// case and underscores. Strings, numbers, booleans, enums (by name) and
// Timestamps (RFC 3339) compare with the filter operators, and `:*` tests
// presence. A repeated field matches when any element does (`!=` when none is
// equal). `error:*` tests for an error, and any other `error` predicate
// compares the error message. A path that the packed message type does not
// have does not match; other unknown fields are INVALID_ARGUMENT.
absl::StatusOr<bool> FilterMatchesOperation(
    const ListFilter& filter, const google::longrunning::Operation& operation);

// Returns a page token that resumes a list at `cursor`. Like ListBackups
// tokens, it is only accepted again with the same `filter`.
std::string MakeListPageToken(std::string_view filter, std::string_view cursor);

// Returns the cursor of a MakeListPageToken token. Returns INVALID_ARGUMENT if
// the token is malformed or was issued for a different filter.
absl::StatusOr<std::string> ParseListPageToken(std::string_view token,
                                               std::string_view filter);

}  // namespace frontend
}  // namespace emulator
}  // namespace spanner
}  // namespace google

#endif  // THIRD_PARTY_CLOUD_SPANNER_EMULATOR_FRONTEND_COMMON_LIST_FILTER_H_

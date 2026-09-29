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

#include "backend/query/search/score_ngrams_evaluator.h"

#include <algorithm>
#include <cstdint>
#include <string>
#include <vector>

#include "googlesql/public/value.h"
#include "absl/container/flat_hash_set.h"
#include "absl/status/status.h"
#include "absl/status/statusor.h"
#include "absl/strings/match.h"
#include "absl/strings/str_split.h"
#include "absl/strings/string_view.h"
#include "absl/types/span.h"
#include "backend/query/search/ngrams_tokenizer.h"
#include "backend/query/search/tokenizer.h"
#include "common/errors.h"
#include "googlesql/base/ret_check.h"
#include "googlesql/base/status_macros.h"

namespace google {
namespace spanner {
namespace emulator {
namespace backend {
namespace query {
namespace search {

namespace {

// The ways SCORE_NGRAMS scores a TOKENLIST of an array.
constexpr absl::string_view kFlattenAggregator = "flatten";
constexpr absl::string_view kMaxElementAggregator = "max_element";

}  // namespace

absl::Status ScoreNgramsEvaluator::BuildTrigrams(
    const googlesql::Value& tokenlist, absl::string_view query,
    bool& source_is_null, bool& is_scalar,
    std::vector<absl::flat_hash_set<std::string>>& element_trigrams,
    absl::flat_hash_set<std::string>& query_trigrams) {
  // substring and ngrams tokenizer signature indexes.
  constexpr int64_t kTokenizerSignatureArgumentSize = 4;
  constexpr int kIsNullIndex = 3;

  GOOGLESQL_ASSIGN_OR_RETURN(auto tokens, StringsFromTokenList(tokenlist));
  bool has_values = false;
  bool is_array = false;
  element_trigrams.emplace_back();
  for (int i = 0; i < tokens.size(); ++i) {
    if (IsTokenizerSignature(tokens[i])) {
      // There will be multiple signatures when tokenlist is concatenated.
      if (!absl::StartsWith(tokens[i], kSubstringTokenizer) &&
          !absl::StartsWith(tokens[i], kNgramsTokenizer)) {
        return error::TokenListNotMatchSearch(
            "SCORE_NGRAMS", "TOKENIZE_SUBSTRING or TOKENIZE_NGRAMS");
      }
      // both substring and ngrams signatures start with:
      //   [substring|ngrams]-ngram_size_max-ngram_size_min-is_source_null
      std::vector<std::string> signature =
          absl::StrSplit(tokens[i], absl::ByChar('-'), absl::SkipEmpty());
      GOOGLESQL_RET_CHECK(signature.size() == kTokenizerSignatureArgumentSize ||
                          signature.size() == kTokenizerSignatureArgumentSize + 1 ||
                          signature.size() == kSubstringTokenizerSignatureArgumentSize ||
                          signature.size() == kSubstringTokenizerSignatureArgumentSize + 1);
      source_is_null = signature[kIsNullIndex] != "0";

      std::vector<googlesql::Value> args{
          googlesql::Value::String(query), googlesql::Value::Int64(kTrigrams),
          googlesql::Value::Int64(kTrigrams), googlesql::Value::Bool(false)};
      GOOGLESQL_ASSIGN_OR_RETURN(auto result, NgramsTokenizer::Tokenize(args));
      GOOGLESQL_ASSIGN_OR_RETURN(auto ngrams, StringsFromTokenList(result));
      for (auto it = ngrams.begin() + 1; it != ngrams.end(); ++it) {
        query_trigrams.insert(*it);
      }
    } else if (tokens[i] == kGapString) {
      // A gap ends an array element.
      is_array = true;
      if (!element_trigrams.back().empty()) {
        element_trigrams.emplace_back();
      }
    } else if (i == 0) {
      return error::TokenListNotMatchSearch(
          "SCORE_NGRAMS", "TOKENIZE_SUBSTRING or TOKENIZE_NGRAMS");
    } else {
      has_values = true;
      std::vector<googlesql::Value> args{googlesql::Value::String(tokens[i]),
                                         googlesql::Value::Int64(kTrigrams),
                                         googlesql::Value::Int64(kTrigrams),
                                         googlesql::Value::Bool(false)};
      GOOGLESQL_ASSIGN_OR_RETURN(auto result, NgramsTokenizer::Tokenize(args));
      GOOGLESQL_ASSIGN_OR_RETURN(auto ngrams, StringsFromTokenList(result));
      for (auto it = ngrams.begin() + 1; it != ngrams.end(); ++it) {
        element_trigrams.back().insert(*it);
      }
    }
  }
  is_scalar = has_values && !is_array;
  return absl::OkStatus();
}

double ScoreNgramsEvaluator::Score(
    const absl::flat_hash_set<std::string>& source_trigrams,
    const absl::flat_hash_set<std::string>& query_trigrams) {
  if (source_trigrams.empty() && query_trigrams.empty()) {
    return 0.0;
  }
  int64_t match_count = 0;
  for (const auto& trigram : query_trigrams) {
    if (source_trigrams.contains(trigram)) {
      ++match_count;
    }
  }
  return static_cast<double>(match_count) /
         (static_cast<double>(query_trigrams.size()) +
          static_cast<double>(source_trigrams.size()) -
          static_cast<double>(match_count));
}

absl::StatusOr<googlesql::Value> ScoreNgramsEvaluator::Evaluate(
    absl::Span<const googlesql::Value> args) {
  constexpr int kAlgorithm = 2;
  constexpr int kArrayAggregator = 4;
  const googlesql::Value& tokenlist = args[0];
  const googlesql::Value& query = args[1];

  if (tokenlist.is_null() || query.is_null()) {
    return googlesql::Value::Double(0.0);
  }

  if (!tokenlist.type()->IsTokenList()) {
    return error::ColumnNotSearchable(tokenlist.type()->DebugString());
  }

  if (!query.type()->IsString()) {
    return error::InvalidQueryType(query.type()->DebugString());
  }

  if (args.size() > kAlgorithm && !args[kAlgorithm].is_null() &&
      (!args[kAlgorithm].type()->IsString() ||
       args[kAlgorithm].string_value() != "trigrams")) {
    return absl::InvalidArgumentError(
        "SCORE_NGRAMS algorithm must be trigrams");
  }

  const bool has_array_aggregator =
      args.size() > kArrayAggregator && !args[kArrayAggregator].is_null();
  const bool max_element =
      has_array_aggregator &&
      args[kArrayAggregator].string_value() == kMaxElementAggregator;
  if (has_array_aggregator && !max_element &&
      args[kArrayAggregator].string_value() != kFlattenAggregator) {
    return absl::InvalidArgumentError(
        "SCORE_NGRAMS array_aggregator must be flatten or max_element");
  }

  std::vector<absl::flat_hash_set<std::string>> element_trigrams;
  absl::flat_hash_set<std::string> query_trigrams;
  bool source_is_null = false;
  bool is_scalar = false;
  GOOGLESQL_ASSIGN_OR_RETURN(bool remove_diacritics,
                            TokenListRemovesDiacritics(tokenlist));
  GOOGLESQL_ASSIGN_OR_RETURN(
      std::string normalized_query,
      NormalizeSearchText(query.string_value(), remove_diacritics));
  GOOGLESQL_RETURN_IF_ERROR(BuildTrigrams(tokenlist, normalized_query,
                                          source_is_null, is_scalar,
                                          element_trigrams, query_trigrams));
  if (has_array_aggregator && is_scalar) {
    return absl::InvalidArgumentError(
        "SCORE_NGRAMS array_aggregator can only be used with a TOKENLIST of "
        "an array column");
  }

  if (source_is_null) {
    return googlesql::Value::Double(0.0);
  }

  if (max_element) {
    double score = 0.0;
    for (const auto& trigrams : element_trigrams) {
      score = std::max(score, Score(trigrams, query_trigrams));
    }
    return googlesql::Value::Double(score);
  }

  // Flatten the array: score the trigrams of all elements together.
  absl::flat_hash_set<std::string> source_trigrams;
  for (auto& trigrams : element_trigrams) {
    source_trigrams.insert(trigrams.begin(), trigrams.end());
  }
  return googlesql::Value::Double(Score(source_trigrams, query_trigrams));
}

}  // namespace search
}  // namespace query
}  // namespace backend
}  // namespace emulator
}  // namespace spanner
}  // namespace google

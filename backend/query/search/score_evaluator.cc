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

#include "backend/query/search/score_evaluator.h"

#include <array>
#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

#include "googlesql/public/json_value.h"
#include "googlesql/public/value.h"
#include "googlesql/public/simple_token_list.h"
#include "absl/algorithm/container.h"
#include "absl/status/status.h"
#include "absl/status/statusor.h"
#include "absl/strings/cord.h"
#include "absl/strings/str_cat.h"
#include "absl/strings/string_view.h"
#include "absl/types/span.h"
#include "backend/query/search/search_evaluator_helpers.h"
#include "backend/query/search/search_util.h"
#include "backend/query/search/tokenizer.h"
#include "common/errors.h"
#include "third_party/spanner_pg/datatypes/extended/pg_jsonb_type.h"
#include "googlesql/base/status_macros.h"

namespace google {
namespace spanner {
namespace emulator {
namespace backend {
namespace query {
namespace search {

namespace {

// The token categories of TOKENIZE_FULLTEXT, which are the token attributes.
constexpr std::array<absl::string_view, 4> kTokenCategories = {
    "small", "medium", "large", "title"};

// The SCORE options that the local scorer applies.
struct ScoreOptions {
  // A multiplier per token category, indexed by the category's attribute.
  std::array<double, kTokenCategories.size()> category_weights = {1.0, 1.0,
                                                                  1.0, 1.0};
};

absl::StatusOr<double> JsonNumber(googlesql::JSONValueConstRef value) {
  if (value.IsInt64()) return static_cast<double>(value.GetInt64());
  if (value.IsUInt64()) return static_cast<double>(value.GetUInt64());
  if (value.IsDouble()) return value.GetDouble();
  return absl::InvalidArgumentError("not a number");
}

// Parses the documented SCORE options. The local scorer has one scoring
// algorithm, so it accepts every documented `version`. It has no bigram or
// term frequency statistics, so it rejects `bigram_weight` and `idf_weight`
// instead of ignoring them.
absl::StatusOr<ScoreOptions> ParseScoreOptions(
    googlesql::JSONValueConstRef options) {
  if (!options.IsObject()) {
    return absl::InvalidArgumentError("SCORE options must be a JSON object");
  }
  ScoreOptions result;
  for (const auto& [name, value] : options.GetMembers()) {
    if (name == "version") {
      if (!value.IsInt64() ||
          (value.GetInt64() != 1 && value.GetInt64() != 2)) {
        return absl::InvalidArgumentError(
            "SCORE option version must be 1 or 2");
      }
    } else if (name == "token_category_weights") {
      if (!value.IsObject()) {
        return absl::InvalidArgumentError(
            "SCORE option token_category_weights must be a JSON object");
      }
      for (const auto& [category, weight] : value.GetMembers()) {
        auto it = absl::c_find(kTokenCategories, category);
        if (it == kTokenCategories.end()) {
          return absl::InvalidArgumentError(
              absl::StrCat("Invalid token category in SCORE option "
                           "token_category_weights: ",
                           category));
        }
        absl::StatusOr<double> number = JsonNumber(weight);
        if (!number.ok() || *number < 0) {
          return absl::InvalidArgumentError(absl::StrCat(
              "SCORE option token_category_weights must map ", category,
              " to a non-negative number"));
        }
        result.category_weights[it - kTokenCategories.begin()] = *number;
      }
    } else if (name == "bigram_weight" || name == "idf_weight") {
      return absl::UnimplementedError(absl::StrCat(
          "SCORE option ", name, " is not supported by the local scorer"));
    } else {
      return absl::InvalidArgumentError(
          absl::StrCat("Unknown SCORE option: ", name));
    }
  }
  return result;
}

absl::StatusOr<double> EvaluateWordsPhraseScore(
    absl::string_view query, const googlesql::Value& tokenlist) {
  GOOGLESQL_ASSIGN_OR_RETURN(const std::vector<std::string> terms,
                   GetNormalizedTerms(query));
  if (terms.empty()) {
    return 0.0;
  }
  GOOGLESQL_ASSIGN_OR_RETURN(std::vector<std::string> tokens,
                   StringsFromTokenList(tokenlist));

  double score = 0.0;
  if (tokens.size() >= terms.size()) {
    for (size_t i = 0; i <= tokens.size() - terms.size(); ++i) {
      if (absl::c_equal(terms,
                        absl::MakeSpan(tokens).subspan(i, terms.size()))) {
        score += 1.0;
      }
    }
  }
  return score;
}

// Returns the options of the SCORE call with arguments `args`. GoogleSQL
// passes `options` as the sixth argument, as JSON. PostgreSQL passes
// `enhance_query_options` and `options` as the sixth and seventh arguments, as
// JSONB.
absl::StatusOr<ScoreOptions> ScoreOptionsFromArguments(
    absl::Span<const googlesql::Value> args) {
  constexpr int kGoogleSqlOptions = 5;
  constexpr int kPostgreSqlOptions = 6;
  if (args.size() > kPostgreSqlOptions &&
      args[kPostgreSqlOptions].type() ==
          postgres_translator::spangres::datatypes::GetPgJsonbType()) {
    const googlesql::Value& options = args[kPostgreSqlOptions];
    if (options.is_null()) {
      return ScoreOptions();
    }
    GOOGLESQL_ASSIGN_OR_RETURN(
        absl::Cord jsonb,
        postgres_translator::spangres::datatypes::GetPgJsonbNormalizedValue(
            options));
    GOOGLESQL_ASSIGN_OR_RETURN(
        googlesql::JSONValue parsed,
        googlesql::JSONValue::ParseJSONString(std::string(jsonb)));
    return ParseScoreOptions(parsed.GetConstRef());
  }

  if (args.size() <= kGoogleSqlOptions || args[kGoogleSqlOptions].is_null()) {
    return ScoreOptions();
  }
  const googlesql::Value& options = args[kGoogleSqlOptions];
  if (!options.type()->IsJson()) {
    return absl::InvalidArgumentError("SCORE options must be JSON");
  }
  if (options.is_unparsed_json()) {
    GOOGLESQL_ASSIGN_OR_RETURN(
        googlesql::JSONValue parsed,
        googlesql::JSONValue::ParseJSONString(options.json_value_unparsed()));
    return ParseScoreOptions(parsed.GetConstRef());
  }
  return ParseScoreOptions(options.json_value());
}

}  // namespace

absl::StatusOr<googlesql::Value> ScoreEvaluator::Evaluate(
    absl::Span<const googlesql::Value> args) {
  const googlesql::Value tokenlist = args[0];
  const googlesql::Value query_string = args[1];

  if (!tokenlist.type()->IsTokenList()) {
    return error::ColumnNotSearchable(tokenlist.type()->DebugString());
  }

  if (!query_string.type()->IsString()) {
    return error::InvalidQueryType(query_string.type()->DebugString());
  }

  bool source_is_null = false;
  if (!tokenlist.is_null()) {
    GOOGLESQL_RETURN_IF_ERROR(
        SearchHelper::BuildTokenMap(tokenlist, "SCORE", source_is_null)
            .status());
  }

  if (source_is_null || query_string.is_null()) {
    return googlesql::Value::Double(0.0);
  }

  GOOGLESQL_ASSIGN_OR_RETURN(const ScoreOptions score_options,
                             ScoreOptionsFromArguments(args));

  std::string query = query_string.string_value();
  if (!tokenlist.is_null()) {
    GOOGLESQL_ASSIGN_OR_RETURN(bool remove_diacritics,
                              TokenListRemovesDiacritics(tokenlist));
    if (remove_diacritics) {
      GOOGLESQL_ASSIGN_OR_RETURN(query,
                                NormalizeSearchText(query, true, "", false));
    }
  }

  Dialect dialect = Dialect::RQUERY;
  if (args.size() > 4 && !args[4].is_null()) {
    GOOGLESQL_ASSIGN_OR_RETURN(dialect, ParseDialect(args[4].string_value()));
  }

  // In the RQUERY and WORDS dialects (which the emulator evaluates alike),
  // each hit on a query term scores 1 plus its token category (small=0,
  // medium=1, large=2, title=3). WORDS_PHRASE scores 1 per phrase match, and
  // each hit on a phrase term adds its token category. The category's weight
  // in the token_category_weights option multiplies the score of a hit.
  double score = 0.0;
  if (dialect == Dialect::WORDS_PHRASE) {
    GOOGLESQL_ASSIGN_OR_RETURN(score, EvaluateWordsPhraseScore(
                                query, tokenlist));
    if (score == 0) {
      return googlesql::Value::Double(score);
    }
  }

  if (!tokenlist.is_null()) {
    GOOGLESQL_ASSIGN_OR_RETURN(auto terms, GetNormalizedTerms(query));
    GOOGLESQL_ASSIGN_OR_RETURN(auto iter,
                              tokenlist.tokenlist_value().GetIterator());
    googlesql::tokens::TextToken token;
    while (!iter.done()) {
      GOOGLESQL_RETURN_IF_ERROR(iter.Next(token));
      const int64_t hits = absl::c_count(terms, token.text());
      if (hits == 0) continue;
      const int64_t category = token.attribute();
      const double weight =
          category >= 0 &&
                  category < static_cast<int64_t>(kTokenCategories.size())
              ? score_options.category_weights[category]
              : 1.0;
      const double hit_score =
          dialect == Dialect::WORDS_PHRASE ? category : 1 + category;
      score += hits * hit_score * weight;
    }
  }

  return googlesql::Value::Double(score);
}

}  // namespace search
}  // namespace query
}  // namespace backend
}  // namespace emulator
}  // namespace spanner
}  // namespace google

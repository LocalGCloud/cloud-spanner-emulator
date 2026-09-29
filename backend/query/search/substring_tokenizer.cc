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

#include "backend/query/search/substring_tokenizer.h"

#include <string>
#include <vector>

#include "googlesql/public/types/type_factory.h"
#include "googlesql/public/value.h"
#include "absl/status/status.h"
#include "absl/status/statusor.h"
#include "absl/strings/match.h"
#include "absl/strings/str_cat.h"
#include "absl/strings/string_view.h"
#include "absl/types/span.h"
#include "backend/query/search/tokenizer.h"
#include "common/errors.h"
#include "googlesql/base/status_macros.h"

namespace google {
namespace spanner {
namespace emulator {
namespace backend {
namespace query {
namespace search {

namespace {
// In addition to the relative search types, TOKENIZE_SUBSTRING accepts a
// special flag, "all", which means that all relative search types are
// supported.
constexpr absl::string_view kTokenizeSubstringRelativeSearchTypeAll = "all";
}  // namespace

absl::Status SubstringTokenizer::ValidateNgramSize(int ngram_size_min,
                                                   int ngram_size_max) {
  if (ngram_size_max <= 0) {
    return error::InvalidNgramSize("ngram_size_max must be greater than 0.");
  }
  if (ngram_size_min <= 0) {
    return error::InvalidNgramSize("ngram_size_min must be greater than 0.");
  }
  if (ngram_size_min > ngram_size_max) {
    return error::InvalidNgramSize(
        "ngram_size_min cannot be greater than ngram_size_max.");
  }
  if (ngram_size_max > kMaxAllowedNgramSizeMax) {
    return error::InvalidNgramSize(
        absl::StrCat("ngram_size_max cannot be greater than ",
                     kMaxAllowedNgramSizeMax, "."));
  }

  return absl::OkStatus();
}

absl::StatusOr<int> SubstringTokenizer::ParseRelativeSearchTypes(
    absl::Span<const googlesql::Value> args) {
  constexpr int kSupportRelativeSearch = 3;
  constexpr int kRelativeSearchTypes = 5;

  // Default relative search type is none if the argument is not present.
  int supported_types = RelativeSearchType::None;
  googlesql::Value support_relative_search =
      args.size() > kSupportRelativeSearch ? args[kSupportRelativeSearch]
                                           : googlesql::Value::NullBool();
  googlesql::Value relative_search_types =
      args.size() > kRelativeSearchTypes
          ? args[kRelativeSearchTypes]
          : googlesql::Value::Null(googlesql::types::BoolArrayType());

  // Only one of support_relative_search and relative_search_types is supported.
  if (!support_relative_search.is_null() && !relative_search_types.is_null()) {
    return error::SearchSubstringSupportRelativeSearchTypeArgConflict();
  }

  // parse relative_search_types if it is present.
  if (!relative_search_types.is_null()) {
    for (auto& type : args[kRelativeSearchTypes].elements()) {
      if (absl::EqualsIgnoreCase(type.string_value(),
                                 kTokenizeSubstringRelativeSearchTypeAll)) {
        supported_types = RelativeSearchType::All;
        // All is requested, no need to parse the rest.
        break;
      }
      GOOGLESQL_ASSIGN_OR_RETURN(RelativeSearchType parsed_type,
                       ParseRelativeSearchType(type.string_value()));
      supported_types = supported_types | parsed_type;
    }
  }

  // supports all relative search types if support_relative_search presents and
  // the value is true.
  if (!support_relative_search.is_null() &&
      support_relative_search.bool_value()) {
    supported_types = RelativeSearchType::All;
  }

  return supported_types;
}

absl::StatusOr<googlesql::Value> SubstringTokenizer::Tokenize(
    absl::Span<const googlesql::Value> args) {
  // argument indexes
  constexpr int kValue = 0;
  constexpr int kNgramMax = 1;
  constexpr int kNgramMin = 2;
  constexpr int kContentType = 4;
  constexpr int kRemoveDiacritics = 6;
  constexpr int kShortTokensOnlyForAnchors = 7;
  constexpr int kLanguageTag = 8;

  int ngram_size_max =
      GetIntParameterValue(args, kNgramMax, kDefaultNgramSizeMax);
  int ngram_size_min =
      GetIntParameterValue(args, kNgramMin, kDefaultNgramSizeMin);
  GOOGLESQL_RETURN_IF_ERROR(ValidateNgramSize(ngram_size_min, ngram_size_max));
  std::string content_type =
      args.size() > kContentType && !args[kContentType].is_null()
          ? args[kContentType].string_value()
          : "text/plain";
  if (content_type != "text/plain" && content_type != "text/html") {
    return absl::InvalidArgumentError("Invalid content_type");
  }
  std::string language_tag =
      args.size() > kLanguageTag && !args[kLanguageTag].is_null()
          ? args[kLanguageTag].string_value()
          : "";
  bool remove_diacritics =
      GetBoolParameterValue(args, kRemoveDiacritics, false);
  bool short_tokens_only_for_anchors =
      GetBoolParameterValue(args, kShortTokensOnlyForAnchors, false);

  GOOGLESQL_ASSIGN_OR_RETURN(auto relative_search_types, ParseRelativeSearchTypes(args));

  std::vector<std::string> token_list;
  const googlesql::Value& text = args[kValue];
  if (text.is_null()) return googlesql::Value::NullTokenList();
  // Keep a signature for non-NULL values, including empty strings.
  std::string tokenize_signature =
      absl::StrCat(kSubstringTokenizer, "-", ngram_size_max, "-",
                   ngram_size_min, "-0-",
                   std::to_string(relative_search_types),
                   remove_diacritics ? "-d" : "",
                   short_tokens_only_for_anchors ? "-a" : "");
  token_list.push_back(tokenize_signature);

  auto process_single_value =
      [&](const googlesql::Value& value) -> absl::Status {
    std::string source = value.string_value();
    if (content_type == "text/html") {
      source.clear();
      for (const auto& segment : ExtractHtmlText(value.string_value())) {
        if (!source.empty()) source.push_back(' ');
        source.append(segment.text);
      }
    }
    GOOGLESQL_ASSIGN_OR_RETURN(
        std::string normalized,
        NormalizeSearchText(source, remove_diacritics, language_tag));
    token_list.push_back(std::move(normalized));
    return absl::OkStatus();
  };

  if (text.type()->IsArray()) {
    for (auto& value : text.elements()) {
      GOOGLESQL_RETURN_IF_ERROR(process_single_value(value));
      // Add array gap so evaluator will handle cross array phrase search.
      token_list.push_back(kGapString);
    }
  } else {
    GOOGLESQL_RETURN_IF_ERROR(process_single_value(text));
  }

  return TokenListFromStrings(token_list);
}

}  // namespace search
}  // namespace query
}  // namespace backend
}  // namespace emulator
}  // namespace spanner
}  // namespace google

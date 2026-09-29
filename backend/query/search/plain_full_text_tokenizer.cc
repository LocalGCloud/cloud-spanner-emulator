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

#include "backend/query/search/plain_full_text_tokenizer.h"

#include <cstdint>
#include <string>
#include <utility>
#include <vector>

#include "googlesql/public/simple_token_list.h"
#include "googlesql/public/value.h"
#include "absl/status/status.h"
#include "absl/status/statusor.h"
#include "absl/strings/ascii.h"
#include "absl/strings/match.h"
#include "absl/strings/str_cat.h"
#include "absl/strings/string_view.h"
#include "absl/types/span.h"
#include "backend/query/search/tokenizer.h"
#include "googlesql/base/status_macros.h"

namespace google {
namespace spanner {
namespace emulator {
namespace backend {
namespace query {
namespace search {

absl::StatusOr<googlesql::Value> PlainFullTextTokenizer::Tokenize(
    absl::Span<const googlesql::Value> args) {
  constexpr int kLanguageTag = 1;
  constexpr int kContentType = 2;
  constexpr int kTokenCategory = 3;
  constexpr int kRemoveDiacritics = 4;

  std::string language_tag =
      args.size() > kLanguageTag && !args[kLanguageTag].is_null()
          ? args[kLanguageTag].string_value()
          : "";
  std::string content_type =
      args.size() > kContentType && !args[kContentType].is_null()
          ? args[kContentType].string_value()
          : "text/plain";
  if (content_type != "text/plain" && content_type != "text/html") {
    return absl::InvalidArgumentError("Invalid content_type");
  }
  int category_override = -1;
  if (args.size() > kTokenCategory && !args[kTokenCategory].is_null()) {
    std::string category = args[kTokenCategory].string_value();
    if (category == "small") category_override = 0;
    else if (category == "medium") category_override = 1;
    else if (category == "large") category_override = 2;
    else if (category == "title") category_override = 3;
    else return absl::InvalidArgumentError("Invalid token_category");
  }
  bool remove_diacritics = GetBoolParameterValue(args, kRemoveDiacritics,
                                                 false);

  const googlesql::Value& text = args[0];
  if (text.is_null()) return googlesql::Value::NullTokenList();
  // Keep a signature for non-NULL values, including empty strings.
  googlesql::tokens::TokenListBuilder builder;
  builder.Add(googlesql::tokens::TextToken::Make(absl::StrCat(
      kFullTextTokenizer, "-0",
      remove_diacritics ? "-d" : "")));

  auto tokenize_value = [&](absl::string_view value) -> absl::Status {
    std::vector<HtmlTextSegment> segments =
        content_type == "text/html"
            ? ExtractHtmlText(value)
            : std::vector<HtmlTextSegment>{{std::string(value), 0}};
    std::vector<std::string> words;
    std::vector<bool> hashtags;
    std::vector<int> categories;
    for (const auto& segment : segments) {
      GOOGLESQL_ASSIGN_OR_RETURN(
          std::string normalized,
          NormalizeSearchText(segment.text, remove_diacritics, language_tag));
      GOOGLESQL_RETURN_IF_ERROR(
          TokenizeWords(normalized, language_tag, words, &hashtags));
      categories.resize(words.size(), category_override >= 0
                                          ? category_override
                                          : segment.category);
    }
    for (size_t i = 0; i < words.size(); ++i) {
      // The first and last words of a value are boundary tokens, and a
      // hashtag is indexed both with and without its '#'.
      const uint64_t index_attribute =
          i == 0 || i + 1 == words.size() ? kBoundaryIndexAttribute : 0;
      std::vector<googlesql::tokens::Token> index_tokens;
      if (hashtags[i]) {
        index_tokens.emplace_back(absl::StrCat("#", words[i]),
                                  index_attribute);
      }
      if (hashtags[i] || index_attribute != 0) {
        index_tokens.emplace_back(words[i], index_attribute);
      }
      builder.Add(googlesql::tokens::TextToken::Make(
          std::move(words[i]), categories[i], std::move(index_tokens)));
    }
    return absl::OkStatus();
  };

  if (text.type()->IsArray()) {
    for (auto& value : text.elements()) {
      if (!value.is_null()) {
        GOOGLESQL_RETURN_IF_ERROR(tokenize_value(value.string_value()));
      }
      // Add array gap so evaluator will handle cross array phrase.
      // TODO: handle array gap in search evaluator.
      builder.Add(googlesql::tokens::TextToken::Make(kGapString));
    }
  } else {
    GOOGLESQL_RETURN_IF_ERROR(tokenize_value(text.string_value()));
  }

  return googlesql::Value::TokenList(builder.Build());
}

}  // namespace search
}  // namespace query
}  // namespace backend
}  // namespace emulator
}  // namespace spanner
}  // namespace google

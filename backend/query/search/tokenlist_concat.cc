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

#include "backend/query/search/tokenlist_concat.h"

#include <string>
#include <vector>

#include "googlesql/public/value.h"
#include "googlesql/public/simple_token_list.h"
#include "absl/status/statusor.h"
#include "absl/strings/str_split.h"
#include "absl/types/span.h"
#include "backend/query/search/tokenizer.h"
#include "common/errors.h"
#include "googlesql/base/ret_check.h"
#include "googlesql/base/status_macros.h"

namespace google::spanner::emulator::backend::query::search {

namespace {

absl::StatusOr<googlesql::tokens::TextToken> WithoutDiacritics(
    const googlesql::tokens::TextToken& token) {
  GOOGLESQL_ASSIGN_OR_RETURN(std::string text,
                   NormalizeSearchText(token.text(), /*remove_diacritics=*/true,
                                       /*language_tag=*/"",
                                       /*lowercase=*/false));
  std::vector<googlesql::tokens::Token> index_tokens;
  for (const googlesql::tokens::Token& index_token : token.index_tokens()) {
    GOOGLESQL_ASSIGN_OR_RETURN(
        std::string index_text,
        NormalizeSearchText(index_token.text(), /*remove_diacritics=*/true,
                            /*language_tag=*/"", /*lowercase=*/false));
    index_tokens.emplace_back(std::move(index_text), index_token.attribute());
  }
  return googlesql::tokens::TextToken::Make(std::move(text), token.attribute(),
                                            std::move(index_tokens));
}

}  // namespace

absl::StatusOr<googlesql::Value> TokenlistConcat::Concat(
    absl::Span<const googlesql::Value> args) {
  GOOGLESQL_RET_CHECK(args.size() == 1 && args[0].type()->IsArray());
  const auto& arg = args[0];
  if (arg.is_null()) {
    return googlesql::Value::NullTokenList();
  }
  // SEARCH removes diacritics from the query when any of the concatenated
  // tokenlists does. The tokens of the other tokenlists then lose their
  // diacritics too, so that they can still match.
  bool removes_diacritics = false;
  for (const auto& tokenlist : arg.elements()) {
    if (tokenlist.is_null()) continue;
    GOOGLESQL_ASSIGN_OR_RETURN(bool removes, TokenListRemovesDiacritics(tokenlist));
    removes_diacritics |= removes;
  }
  googlesql::tokens::TokenListBuilder builder;
  std::vector<std::string> first_signature;
  for (const auto& tokenlist : arg.elements()) {
    if (tokenlist.is_null()) {
      // NULL TOKENLIST inside array will just be skipped.
      continue;
    }
    GOOGLESQL_ASSIGN_OR_RETURN(auto tokens, StringsFromTokenList(tokenlist));
    if (first_signature.empty()) {
      first_signature = absl::StrSplit(tokens[0], '-');
    } else {
      std::vector<std::string> current_signature =
          absl::StrSplit(tokens[0], '-');
      if (current_signature[0] != first_signature[0]) {
        return error::TokenlistTypeMergeConflict();
      }
      if (current_signature[0] == kNgramsTokenizer ||
          current_signature[0] == kSubstringTokenizer) {
        // For substring and ngram tokenizer, also need to check ngram sizes.
        if (current_signature[1] != first_signature[1] ||
            current_signature[2] != first_signature[2]) {
          return error::TokenlistTypeMergeConflict();
        }
      }
    }
    GOOGLESQL_ASSIGN_OR_RETURN(bool removes, TokenListRemovesDiacritics(tokenlist));
    const bool strip_diacritics = removes_diacritics && !removes;
    GOOGLESQL_ASSIGN_OR_RETURN(auto iter, tokenlist.tokenlist_value().GetIterator());
    googlesql::tokens::TextToken token;
    while (!iter.done()) {
      GOOGLESQL_RETURN_IF_ERROR(iter.Next(token));
      if (strip_diacritics && !IsTokenizerSignature(token.text()) &&
          token.text() != kGapString) {
        GOOGLESQL_ASSIGN_OR_RETURN(token, WithoutDiacritics(token));
      }
      builder.Add(token);
    }
    builder.Add(googlesql::tokens::TextToken::Make(kGapString));
  }
  return googlesql::Value::TokenList(builder.Build());
}

}  // namespace google::spanner::emulator::backend::query::search

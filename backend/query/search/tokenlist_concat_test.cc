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

#include "googlesql/public/types/type_factory.h"
#include "googlesql/public/simple_token_list.h"
#include "googlesql/public/value.h"
#include "gmock/gmock.h"
#include "gtest/gtest.h"
#include "googlesql/base/testing/status_matchers.h"
#include "absl/status/status.h"
#include "absl/status/statusor.h"
#include "backend/query/search/plain_full_text_tokenizer.h"
#include "backend/query/search/substring_tokenizer.h"
#include "backend/query/search/tokenizer.h"

namespace google::spanner::emulator::backend::query::search {

using testing::HasSubstr;
using googlesql_base::testing::StatusIs;

void ValidateTokenlistConcat(googlesql::Value tokenlist1,
                             googlesql::Value tokenlist2,
                             googlesql::Value concat_tokenlist) {
  std::vector<std::string> expected_tokens;
  const auto tokenlist1_tokens = StringsFromTokenList(tokenlist1);
  const auto tokenlist2_tokens = StringsFromTokenList(tokenlist2);
  expected_tokens.insert(expected_tokens.end(), tokenlist1_tokens->begin(),
                         tokenlist1_tokens->end());
  expected_tokens.push_back(kGapString);
  expected_tokens.insert(expected_tokens.end(), tokenlist2_tokens->begin(),
                         tokenlist2_tokens->end());
  expected_tokens.push_back(kGapString);

  EXPECT_EQ(expected_tokens, *StringsFromTokenList(concat_tokenlist));
}

TEST(TokenlistConcatTest, ConcatFulltext) {
  const auto tokenlist1 =
      PlainFullTextTokenizer::Tokenize({googlesql::Value::String("foo bar")});
  const auto tokenlist2 = PlainFullTextTokenizer::Tokenize(
      {googlesql::Value::String("hello world")});

  // Create a tokenlist array. null tokenlist will be ignored.
  absl::StatusOr<googlesql::Value> tokenlist_array =
      googlesql::Value::MakeArray(
          googlesql::types::TokenListArrayType(),
          {tokenlist1.value(), googlesql::Value::NullTokenList(),
           tokenlist2.value()});
  const auto concat_tokenlist =
      TokenlistConcat::Concat({tokenlist_array.value()});

  ValidateTokenlistConcat(*tokenlist1, *tokenlist2, *concat_tokenlist);
}

TEST(TokenlistConcatTest, PreservesTokenCategories) {
  GOOGLESQL_ASSERT_OK_AND_ASSIGN(
      auto title,
      PlainFullTextTokenizer::Tokenize(
          {googlesql::Value::String("Apple"), googlesql::Value::NullString(),
           googlesql::Value::NullString(), googlesql::Value::String("title")}));
  GOOGLESQL_ASSERT_OK_AND_ASSIGN(
      auto body,
      PlainFullTextTokenizer::Tokenize({googlesql::Value::String("Apple")}));
  GOOGLESQL_ASSERT_OK_AND_ASSIGN(
      auto values,
      googlesql::Value::MakeArray(googlesql::types::TokenListArrayType(),
                                  {title, body}));
  GOOGLESQL_ASSERT_OK_AND_ASSIGN(auto joined,
                                TokenlistConcat::Concat({values}));
  GOOGLESQL_ASSERT_OK_AND_ASSIGN(auto iter,
                                joined.tokenlist_value().GetIterator());
  googlesql::tokens::TextToken token;
  ASSERT_TRUE(iter.Next(token).ok());  // first signature
  ASSERT_TRUE(iter.Next(token).ok());
  EXPECT_EQ(token.text(), "apple");
  EXPECT_EQ(token.attribute(), 3);
}

TEST(TokenlistConcatTest, ConcatSubstring) {
  const auto tokenlist1 =
      SubstringTokenizer::Tokenize({googlesql::Value::String("foobar")});
  const auto tokenlist2 =
      SubstringTokenizer::Tokenize({googlesql::Value::String("helloworld")});

  absl::StatusOr<googlesql::Value> tokenlist_array =
      googlesql::Value::MakeArray(googlesql::types::TokenListArrayType(),
                                  {tokenlist1.value(), tokenlist2.value()});
  const auto concat_tokenlist =
      TokenlistConcat::Concat({tokenlist_array.value()});

  ValidateTokenlistConcat(*tokenlist1, *tokenlist2, *concat_tokenlist);
}

TEST(TokenlistConcatTest, ConcatNull) {
  const auto concat_tokenlist = TokenlistConcat::Concat(
      {googlesql::Value::Null(googlesql::types::TokenListArrayType())});
  GOOGLESQL_EXPECT_OK(concat_tokenlist.status());
  EXPECT_TRUE(concat_tokenlist->type()->IsTokenList());
  EXPECT_TRUE(concat_tokenlist->is_null());
}

TEST(TokenlistConcatTest, UnmatchSubstringSignature) {
  const auto tokenlist1 = SubstringTokenizer::Tokenize(
      {googlesql::Value::String("foobar"), googlesql::Value::Int64(3),
       googlesql::Value::Int64(2)});
  const auto tokenlist2 = SubstringTokenizer::Tokenize(
      {googlesql::Value::String("foobar"), googlesql::Value::Int64(4),
       googlesql::Value::Int64(1)});
  absl::StatusOr<googlesql::Value> tokenlist_array =
      googlesql::Value::MakeArray(googlesql::types::TokenListArrayType(),
                                  {tokenlist1.value(), tokenlist2.value()});
  EXPECT_THAT(
      TokenlistConcat::Concat({tokenlist_array.value()}),
      StatusIs(absl::StatusCode::kInvalidArgument,
               HasSubstr("All elements to TOKENLIST_CONCAT must be produced by "
                         "the same kind of tokenization function.")));
}

TEST(TokenlistConcatTest, MixedRemoveDiacriticsRemovesThemEverywhere) {
  GOOGLESQL_ASSERT_OK_AND_ASSIGN(
      auto kept,
      PlainFullTextTokenizer::Tokenize({googlesql::Value::String("Café")}));
  GOOGLESQL_ASSERT_OK_AND_ASSIGN(
      auto removed,
      PlainFullTextTokenizer::Tokenize(
          {googlesql::Value::String("Crème"), googlesql::Value::NullString(),
           googlesql::Value::NullString(), googlesql::Value::NullString(),
           googlesql::Value::Bool(true)}));
  GOOGLESQL_ASSERT_OK_AND_ASSIGN(
      auto values,
      googlesql::Value::MakeArray(googlesql::types::TokenListArrayType(),
                                  {kept, removed}));
  GOOGLESQL_ASSERT_OK_AND_ASSIGN(auto joined, TokenlistConcat::Concat({values}));
  // SEARCH removes the diacritics from queries on the result, so the tokens
  // of the first tokenlist lose theirs too.
  EXPECT_THAT(StringsFromTokenList(joined),
              googlesql_base::testing::IsOkAndHolds(testing::ElementsAre(
                  "fulltext-0", "cafe", kGapString, "fulltext-0-d", "creme",
                  kGapString)));
  EXPECT_THAT(TokenListRemovesDiacritics(joined),
              googlesql_base::testing::IsOkAndHolds(true));
}

}  // namespace google::spanner::emulator::backend::query::search

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

#include <string>
#include <vector>

#include "gmock/gmock.h"
#include "gtest/gtest.h"
#include "googlesql/public/simple_token_list.h"
#include "googlesql/base/testing/status_matchers.h"
#include "absl/status/statusor.h"
#include "backend/query/search/tokenizer.h"

namespace google {
namespace spanner {
namespace emulator {
namespace backend {
namespace query {
namespace search {

struct TokenizeTestCase {
  std::vector<std::string> inputs;
  std::vector<std::string> expected_result;
  bool is_array_input;

  TokenizeTestCase(std::vector<std::string> inputs,
                   std::vector<std::string> expected)
      : TokenizeTestCase(inputs, expected, /*is_array_input=*/false) {}

  TokenizeTestCase(std::vector<std::string> inputs,
                   std::vector<std::string> expected, bool is_array_input)
      : inputs(inputs),
        expected_result(expected),
        is_array_input(is_array_input) {}
};

void VerifyTestCase(TokenizeTestCase& tc) {
  googlesql::Value value;
  if (tc.is_array_input) {
    value = googlesql::values::StringArray(tc.inputs);
  } else {
    value = googlesql::Value::String(tc.inputs[0]);
  }

  absl::StatusOr<googlesql::Value> result =
      PlainFullTextTokenizer::Tokenize({value});
  GOOGLESQL_EXPECT_OK(result.status());

  googlesql::Value token_list = result.value();
  EXPECT_TRUE(token_list.type()->IsTokenList());

  // Always expect the tokenlist has at least one token
  // which stores tokenizer information.
  GOOGLESQL_ASSERT_OK_AND_ASSIGN(auto tokens, StringsFromTokenList(token_list));
  ASSERT_FALSE(tokens.empty());
  EXPECT_EQ(tokens[0], "fulltext-0");

  std::vector<std::string> token_texts;
  token_texts.insert(token_texts.end(), tokens.begin() + 1, tokens.end());

  EXPECT_EQ(tc.expected_result, token_texts);
}

TEST(PlainFullTextTokenizerTest, BasicTokenize) {
  std::vector<TokenizeTestCase> test_cases = {
      TokenizeTestCase(
          {"third~party!cloud@spanner#emulator$backend%query^search&simple"},
          {"third", "party", "cloud", "spanner", "emulator", "backend", "query",
           "search", "simple"}),
      TokenizeTestCase(
          {"third*party(cloud)spanner_emulator+backend-query=search|simple"},
          {"third", "party", "cloud", "spanner", "emulator", "backend", "query",
           "search", "simple"}),
      TokenizeTestCase(
          {"third{party}cloud[spanner]emulator\\backend/query\"search`simple"},
          {"third", "party", "cloud", "spanner", "emulator", "backend", "query",
           "search", "simple"}),
      TokenizeTestCase({"third:party;cloud<spanner>emulator,backend.query"},
                       {"third", "party", "cloud", "spanner", "emulator",
                        "backend", "query"}),
      TokenizeTestCase({"third!\"party--cloud!?/"
                        "spanner({})emulator[<backend>]query+=search"},
                       {"third", "party", "cloud", "spanner", "emulator",
                        "backend", "query", "search"}),
  };

  for (auto& tc : test_cases) {
    VerifyTestCase(tc);
  }
}

TEST(PlainFullTextTokenizerTest, WhiteSpaces) {
  std::vector<TokenizeTestCase> test_cases = {
      TokenizeTestCase({"third\rparty\ncloud spanner\temulator"},
                       {"third", "party", "cloud", "spanner", "emulator"}),
      TokenizeTestCase({"third\r\nparty\r cloud \nspanner\t\temulator"},
                       {"third", "party", "cloud", "spanner", "emulator"}),
      TokenizeTestCase({"third \tparty\r\n\r\ncloud\r\n\tspanner\r\n emulator"},
                       {"third", "party", "cloud", "spanner", "emulator"}),
      TokenizeTestCase({" third  \r\n\tparty \r\n cloud \t spanner! emulator"},
                       {"third", "party", "cloud", "spanner", "emulator"}),
      TokenizeTestCase({" third -- party. \"cloud \" spanner += emulator"},
                       {"third", "party", "cloud", "spanner", "emulator"}),
  };

  for (auto& tc : test_cases) {
    VerifyTestCase(tc);
  }
}

TEST(PlainFullTextTokenizerTest, NullTokenList) {
  std::vector<std::string> test_cases = {
      "", " ", "   ", " \t", "\r\n", "!?", " \"\"", "-\"\r\n\" !", " +\t@\r."};

  for (auto& tc : test_cases) {
    absl::StatusOr<googlesql::Value> result =
        PlainFullTextTokenizer::Tokenize({googlesql::Value::String(tc)});
    GOOGLESQL_EXPECT_OK(result.status());

    googlesql::Value token_list = result.value();
    EXPECT_TRUE(token_list.type()->IsTokenList());

    GOOGLESQL_ASSERT_OK_AND_ASSIGN(auto tokens, StringsFromTokenList(token_list));
    ASSERT_EQ(tokens.size(), 1);
    // The sources have value as empty strings but not null. The signature uses
    // 0 to mark it.
    EXPECT_EQ(tokens[0], "fulltext-0");
  }
}

TEST(PlainFullTextTokenizerTest, AlphaNumeric) {
  TokenizeTestCase test_cases =
      TokenizeTestCase({"3rd g00gle! 1234"}, {"3rd", "g00gle", "1234"});

  VerifyTestCase(test_cases);
}

TEST(PlainFullTextTokenizerTest, ToLowerCases) {
  TokenizeTestCase test_cases = TokenizeTestCase(
      {"GooGle gOOgLE! GOOGLE"}, {"google", "google", "google"});

  VerifyTestCase(test_cases);
}

TEST(PlainFullTextTokenizerTest, NullInputValue) {
  absl::StatusOr<googlesql::Value> result =
      PlainFullTextTokenizer::Tokenize({googlesql::Value::NullString()});
  GOOGLESQL_EXPECT_OK(result.status());
  EXPECT_TRUE(result->type()->IsTokenList());
  EXPECT_TRUE(result->is_null());
}

TEST(PlainFullTextTokenizerTest, TokenizeArray) {
  std::vector<TokenizeTestCase> test_cases = {
      TokenizeTestCase(
          {"third~party", "!cloud@spanner", "emulator$backend%query",
           "^search&simple"},
          {"third", "party", "\x1", "cloud", "spanner", "\x1", "emulator",
           "backend", "query", "\x1", "search", "simple", "\x1"},
          true),
      TokenizeTestCase({"third*party(cloud)spanner_emulator", "",
                        "backend-query=search|simple"},
                       {"third", "party", "cloud", "spanner", "emulator", "\x1",
                        "\x1", "backend", "query", "search", "simple", "\x1"},
                       true),
      TokenizeTestCase(
          {"third{party}cloud[spanner]emulator\\backend/query\"search`simple"},
          {"third", "party", "cloud", "spanner", "emulator", "backend", "query",
           "search", "simple", "\x1"},
          true),
      TokenizeTestCase({"*&&", "  *&", ")(&(^*&^&%))"}, {"\x1", "\x1", "\x1"},
                       true),
  };

  for (auto& tc : test_cases) {
    VerifyTestCase(tc);
  }
}

TEST(PlainFullTextTokenizerTest, DiacriticsAndTurkishCase) {
  GOOGLESQL_ASSERT_OK_AND_ASSIGN(
      auto folded,
      PlainFullTextTokenizer::Tokenize(
          {googlesql::Value::String("Crème BRÛLÉE"),
           googlesql::Value::NullString(), googlesql::Value::NullString(),
           googlesql::Value::NullString(), googlesql::Value::Bool(true)}));
  EXPECT_EQ(*StringsFromTokenList(folded),
            (std::vector<std::string>{"fulltext-0-d", "creme", "brulee"}));

  GOOGLESQL_ASSERT_OK_AND_ASSIGN(
      auto turkish,
      PlainFullTextTokenizer::Tokenize(
          {googlesql::Value::String("I İ"), googlesql::Value::String("tr")}));
  EXPECT_EQ(*StringsFromTokenList(turkish),
            (std::vector<std::string>{"fulltext-0", "ı", "i"}));
}

TEST(PlainFullTextTokenizerTest, BoundariesAndHashtags) {
  // The DEBUG_TOKENLIST example of the documentation.
  GOOGLESQL_ASSERT_OK_AND_ASSIGN(
      auto result, PlainFullTextTokenizer::Tokenize(
                       {googlesql::Value::String("Hello DB #World")}));
  EXPECT_THAT(DebugTokenList(result),
              googlesql_base::testing::IsOkAndHolds(
                  "hello(boundary), db, [#world, world](boundary)"));
  // Searches see the word without '#'.
  EXPECT_THAT(StringsFromTokenList(result),
              googlesql_base::testing::IsOkAndHolds(
                  testing::ElementsAre("fulltext-0", "hello", "db", "world")));
}

TEST(PlainFullTextTokenizerTest, LanguageTag) {
  GOOGLESQL_ASSERT_OK_AND_ASSIGN(
      auto japanese,
      PlainFullTextTokenizer::Tokenize({googlesql::Value::String("東京タワー"),
                                        googlesql::Value::String("ja")}));
  EXPECT_THAT(StringsFromTokenList(japanese),
              googlesql_base::testing::IsOkAndHolds(
                  testing::ElementsAre("fulltext-0", "東京", "タワー")));
  GOOGLESQL_ASSERT_OK_AND_ASSIGN(
      auto french,
      PlainFullTextTokenizer::Tokenize(
          {googlesql::Value::String("L'Été indien"),
           googlesql::Value::String("fr")}));
  EXPECT_THAT(StringsFromTokenList(french),
              googlesql_base::testing::IsOkAndHolds(testing::ElementsAre(
                  "fulltext-0", "l", "été", "indien")));
}

TEST(PlainFullTextTokenizerTest, HtmlAndTokenCategory) {
  GOOGLESQL_ASSERT_OK_AND_ASSIGN(
      auto result,
      PlainFullTextTokenizer::Tokenize(
          {googlesql::Value::String("<h1>Apple</h1><p>&amp; <b>Crème</b></p>"),
           googlesql::Value::NullString(), googlesql::Value::String("text/html")}));
  GOOGLESQL_ASSERT_OK_AND_ASSIGN(auto iter,
                                result.tokenlist_value().GetIterator());
  googlesql::tokens::TextToken token;
  ASSERT_TRUE(iter.Next(token).ok());  // signature
  ASSERT_TRUE(iter.Next(token).ok());
  EXPECT_EQ(token.text(), "apple");
  EXPECT_EQ(token.attribute(), 3);
  ASSERT_TRUE(iter.Next(token).ok());
  EXPECT_EQ(token.text(), "crème");
  EXPECT_EQ(token.attribute(), 1);
  EXPECT_TRUE(iter.done());

  GOOGLESQL_ASSERT_OK_AND_ASSIGN(
      auto overridden,
      PlainFullTextTokenizer::Tokenize(
          {googlesql::Value::String("<h1>Apple</h1>"),
           googlesql::Value::NullString(), googlesql::Value::String("text/html"),
           googlesql::Value::String("medium")}));
  GOOGLESQL_ASSERT_OK_AND_ASSIGN(iter,
                                overridden.tokenlist_value().GetIterator());
  ASSERT_TRUE(iter.Next(token).ok());
  ASSERT_TRUE(iter.Next(token).ok());
  EXPECT_EQ(token.attribute(), 1);
}

}  // namespace search
}  // namespace query
}  // namespace backend
}  // namespace emulator
}  // namespace spanner
}  // namespace google

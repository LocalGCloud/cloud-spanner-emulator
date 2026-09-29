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

#include "backend/query/search/tokenizer.h"

#include <string>
#include <vector>

#include "gmock/gmock.h"
#include "gtest/gtest.h"
#include "googlesql/public/simple_token_list.h"
#include "googlesql/public/value.h"
#include "googlesql/base/testing/status_matchers.h"

namespace google {
namespace spanner {
namespace emulator {
namespace backend {
namespace query {
namespace search {
namespace {

using ::testing::ElementsAre;
using ::testing::UnorderedElementsAre;

TEST(TokenizerTest, TokenizeSubstringEmpty) {
  std::vector<std::string> tokens;
  GOOGLESQL_ASSERT_OK(TokenizeSubstring("", tokens));
  EXPECT_TRUE(tokens.empty());
}

TEST(TokenizerTest, TokenizeSubstringSimple) {
  std::vector<std::string> tokens;
  GOOGLESQL_ASSERT_OK(TokenizeSubstring("Hello World", tokens));
  EXPECT_THAT(tokens, ElementsAre("Hello", "World"));
}

TEST(TokenizerTest, TokenizeSubstringWithDelimiters) {
  std::vector<std::string> tokens;
  GOOGLESQL_ASSERT_OK(TokenizeSubstring("foo-bar baz,qux", tokens));
  EXPECT_THAT(tokens, ElementsAre("foo", "bar", "baz", "qux"));
}

TEST(TokenizerTest, TokenizeSubstringSkipsWhitespace) {
  std::vector<std::string> tokens;
  GOOGLESQL_ASSERT_OK(
      TokenizeSubstring("  leading\n trailing  \tmultiple---delims", tokens));
  EXPECT_THAT(tokens, ElementsAre("leading", "trailing", "multiple", "delims"));
}

TEST(TokenizerTest, TokenizeSubstringKeepsNonAscii) {
  std::vector<std::string> tokens;
  GOOGLESQL_ASSERT_OK(TokenizeSubstring("Grüße-Welt", tokens));
  EXPECT_THAT(tokens, ElementsAre("Grüße", "Welt"));
}

TEST(TokenizerTest, TokenizeSubstringAppends) {
  std::vector<std::string> tokens = {"existing"};
  GOOGLESQL_ASSERT_OK(TokenizeSubstring("new tokens", tokens));
  EXPECT_THAT(tokens, ElementsAre("existing", "new", "tokens"));
}

TEST(TokenizerTest, TokenizeSubstringWithCJK) {
  std::vector<std::string> tokens;
  GOOGLESQL_ASSERT_OK(TokenizeSubstring("你好-世界", tokens));
  EXPECT_THAT(tokens, ElementsAre("你好", "世界"));
}

TEST(TokenizerTest, TokenizeSubstringWithMoreDelimiters) {
  std::vector<std::string> tokens;
  GOOGLESQL_ASSERT_OK(TokenizeSubstring("a@b#c_d=e[f]g", tokens));
  EXPECT_THAT(tokens, ElementsAre("a", "b", "c", "d", "e", "f", "g"));
}

TEST(TokenizerTest, TokenizeWordsFindsHashtags) {
  std::vector<std::string> tokens;
  std::vector<bool> hashtags;
  GOOGLESQL_ASSERT_OK(TokenizeWords("hello #world a#b", "", tokens, &hashtags));
  EXPECT_THAT(tokens, ElementsAre("hello", "world", "a", "b"));
  EXPECT_THAT(hashtags, ElementsAre(false, true, false, false));
}

TEST(TokenizerTest, TokenizeWordsSegmentsByLanguage) {
  std::vector<std::string> tokens;
  GOOGLESQL_ASSERT_OK(TokenizeWords("東京タワー", "ja", tokens));
  EXPECT_THAT(tokens, ElementsAre("東京", "タワー"));
  tokens.clear();
  GOOGLESQL_ASSERT_OK(TokenizeWords("l'été dernier", "fr", tokens));
  EXPECT_THAT(tokens, ElementsAre("l", "été", "dernier"));
}

TEST(TokenizerTest, ExtractHtmlTextSkipsScriptsAndStyles) {
  std::vector<HtmlTextSegment> segments = ExtractHtmlText(
      "<html><head><style>p { color: red; }</style>"
      "<SCRIPT type=\"text/javascript\">if (a < b) alert('x');</SCRIPT>"
      "</head><body><p title=\"a > b\">Hello</p>1 < 2</body></html>");
  std::vector<std::string> texts;
  for (const HtmlTextSegment& segment : segments) texts.push_back(segment.text);
  EXPECT_THAT(texts, ElementsAre("Hello", "1 < 2"));
}

TEST(TokenizerTest, ExtractHtmlTextDecodesCharacterReferences) {
  std::vector<HtmlTextSegment> segments = ExtractHtmlText(
      "caf&eacute; &euro;5 &#233;&#xE9; &copy &unknown; &amp;&lt;&gt; "
      "&#0; &#x110000;");
  ASSERT_EQ(segments.size(), 1);
  EXPECT_EQ(segments[0].text,
            "café €5 éé © &unknown; &<> \uFFFD \uFFFD");
}

TEST(TokenizerTest, DebugTokenListShowsBoundariesAndAlternatives) {
  googlesql::tokens::TokenListBuilder builder;
  builder.Add(googlesql::tokens::TextToken::Make("fulltext-0"));
  builder.Add(googlesql::tokens::TextToken::Make(
      "hello", 0, {googlesql::tokens::Token("hello", kBoundaryIndexAttribute)}));
  builder.Add(googlesql::tokens::TextToken::Make("db"));
  builder.Add(googlesql::tokens::TextToken::Make(
      "world", 0,
      {googlesql::tokens::Token("#world", kBoundaryIndexAttribute),
       googlesql::tokens::Token("world", kBoundaryIndexAttribute)}));
  builder.Add(googlesql::tokens::TextToken::Make(kGapString));
  EXPECT_THAT(
      DebugTokenList(googlesql::Value::TokenList(builder.Build())),
      ::googlesql_base::testing::IsOkAndHolds(
          "hello(boundary), db, [#world, world](boundary)"));
}

TEST(TokenizerTest, TokenizeNgramsEmpty) {
  std::vector<std::string> tokens;
  GOOGLESQL_ASSERT_OK(TokenizeNgrams("", 1, 1, tokens));
  EXPECT_TRUE(tokens.empty());
}

TEST(TokenizerTest, TokenizeNgramsSimple) {
  std::vector<std::string> tokens;
  GOOGLESQL_ASSERT_OK(TokenizeNgrams("abc", 1, 1, tokens));
  EXPECT_THAT(tokens, ElementsAre("a", "b", "c"));
}

TEST(TokenizerTest, TokenizeNgramsMinMax) {
  std::vector<std::string> tokens;
  GOOGLESQL_ASSERT_OK(TokenizeNgrams("google", 3, 4, tokens));
  EXPECT_THAT(tokens, UnorderedElementsAre("goo", "oog", "ogl", "gle", "goog",
                                           "oogl", "ogle"));
}

TEST(TokenizerTest, TokenizeNgramsMinGreaterThanMax) {
  std::vector<std::string> tokens;
  GOOGLESQL_ASSERT_OK(TokenizeNgrams("google", 4, 3, tokens));
  EXPECT_TRUE(tokens.empty());
}

TEST(TokenizerTest, TokenizeNgramsSizeGreaterThanStringLength) {
  std::vector<std::string> tokens;
  GOOGLESQL_ASSERT_OK(TokenizeNgrams("abc", 4, 5, tokens));
  EXPECT_TRUE(tokens.empty());
}

TEST(TokenizerTest, TokenizeNgramsHandlesUppercase) {
  std::vector<std::string> tokens;
  GOOGLESQL_ASSERT_OK(TokenizeNgrams("ABC", 2, 2, tokens));
  EXPECT_THAT(tokens, ElementsAre("AB", "BC"));
}

TEST(TokenizerTest, TokenizeNgramsAppends) {
  std::vector<std::string> tokens = {"existing"};
  GOOGLESQL_ASSERT_OK(TokenizeNgrams("abc", 1, 1, tokens));
  EXPECT_THAT(tokens, ElementsAre("existing", "a", "b", "c"));
}

}  // namespace
}  // namespace search
}  // namespace query
}  // namespace backend
}  // namespace emulator
}  // namespace spanner
}  // namespace google

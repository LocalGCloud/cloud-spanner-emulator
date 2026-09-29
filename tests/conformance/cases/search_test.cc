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

#include <string>
#include <vector>

#include "gmock/gmock.h"
#include "gtest/gtest.h"
#include "googlesql/base/testing/status_matchers.h"
#include "tests/common/proto_matchers.h"
#include "absl/status/status.h"
#include "absl/strings/str_cat.h"
#include "absl/strings/substitute.h"
#include "tests/common/scoped_feature_flags_setter.h"
#include "tests/conformance/common/database_test_base.h"
#include "tests/conformance/common/query_translator.h"
#include "googlesql/base/status_macros.h"

namespace google {
namespace spanner {
namespace emulator {
namespace test {

namespace {

using testing::ContainsRegex;
using testing::HasSubstr;
using testing::Matcher;
using googlesql_base::testing::IsOkAndHolds;
using googlesql_base::testing::StatusIs;

class SearchTest
    : public DatabaseTest,
      public ::testing::WithParamInterface<database_api::DatabaseDialect> {
 public:
  SearchTest()
      : feature_flags_({.enable_postgresql_interface = true,
                        .enable_search_index = true,
                        .enable_hidden_column = true}) {}

  void SetUp() override {
    dialect_ = GetParam();
    DatabaseTest::SetUp();
  }

  absl::Status SetUpDatabase() override {
    GOOGLESQL_RETURN_IF_ERROR(SetSchemaFromFile("search.test"));
    return PopulateDatabase();
  }

 protected:
  absl::Status PopulateDatabase() {
    std::vector<std::string> one_track = {"track1 track2"};
    std::vector<std::string> two_tracks = {"track1", "track2"};
    std::vector<std::string> single_track = {"track1"};
    GOOGLESQL_RETURN_IF_ERROR(
        MultiInsert(
            "albums",
            {"albumid", "userid", "releasetimestamp", "uid", "name", "tracks",
             "summary", "summary2"},
            {{0, 1, 0, 0, "name1", one_track, "", ""},
             {1, 1, 11, 0, "name1", one_track, "global top 50 song", "foo bar"},
             {2, 1, 9, 0, "name1", two_tracks, "global top 100 song", ""},
             {3, 2, 3, 0, "name2", two_tracks, "US top 50 song", "bar baz"},
             {4, 1, 2, 0, "name1", two_tracks, "Song at top 1000 in US", "foo"},
             {5, 2, 13, 0, "name1 name2", one_track, "global and US top song",
              ""},
             {6, 1, 5, 0, "name3 name2 name1", two_tracks, "popular song", ""},
             {7, 1, 8, 0, "name1", one_track, "US top 1000 song", ""},
             {8, 2, 4, 1, "name1", two_tracks, "global top 1000 song", ""},
             {9, 1, 7, 2, "name1", two_tracks, "", ""},
             {10, 1, 6, 3, "name1 name2", one_track, "", ""},
             {11, 2, 14, 0, "", std::vector<std::string>{}, "", ""},
             {12, 1, 19, 0, " ", single_track, "", ""},
             {13, 1, 17, 0, "  ", std::vector<std::string>{}, "", ""},
             {16, 1, 20, 0, "classic rock", std::vector<std::string>{}, "", ""},
             {17, 1, 21, 0, "electronic rock", std::vector<std::string>{}, "",
              ""}})
            .status());

    // Insert a row with NULL name
    GOOGLESQL_RETURN_IF_ERROR(
        Insert("albums",
               {"albumid", "userid", "releasetimestamp", "uid", "summary"},
               {14, 2, 9, 0, ""})
            .status());

    // Insert a row with NULL summary
    GOOGLESQL_RETURN_IF_ERROR(
        Insert("albums",
               {"albumid", "userid", "releasetimestamp", "uid", "name"},
               {15, 1, 12, 0, ""})
            .status());
    // Insert a row meant to test TOKENIZE_SUBSTRING with SEARCH_NGRAMS.
    GOOGLESQL_RETURN_IF_ERROR(Insert("AccountIds", {"AccountId"}, {1}).status());
    GOOGLESQL_RETURN_IF_ERROR(Insert("AccountSearchTerms", {"AccountId", "SearchPhrase"},
                           {1, "name individual@email.com"})
                        .status());

    // Data for fuzzy search test.
    return MultiInsert("Test", {"Id", "SearchField"},
                       {{"id1", "oh hello there"},
                        {"id2", "oh hallo ther"},
                        {"id3", "oh hello there"},
                        {"id4", "something else entirely"}})
        .status();
  }

  bool IsGoogleStandardSql() const {
    return dialect_ == database_api::DatabaseDialect::GOOGLE_STANDARD_SQL;
  }

  std::string GetSqlQueryString(const std::string& query_string) {
    if (IsGoogleStandardSql()) {
      return query_string;
    }

    return QueryTranslator()
        .UsesHintSyntax()
        .UsesLiteralCasts()
        .UsesArgumentStringLiterals()
        .UsesStringNullLiteral()
        .UsesStringArrayLiterals()
        .UsesNamespacedFunctions()
        .Translate(query_string);
  }

 private:
  test::ScopedEmulatorFeatureFlagsSetter feature_flags_;
};

INSTANTIATE_TEST_SUITE_P(
    PerDialectSearchTests, SearchTest,
    testing::Values(database_api::DatabaseDialect::GOOGLE_STANDARD_SQL,
                    database_api::DatabaseDialect::POSTGRESQL),
    [](const testing::TestParamInfo<SearchTest::ParamType>& info) {
      return database_api::DatabaseDialect_Name(info.param);
    });

TEST_P(SearchTest, NullTokenizerInputsReturnSqlNull) {
  if (!IsGoogleStandardSql()) {
    EXPECT_THAT(Query(R"sql(SELECT spanner.token(null::text) IS NULL,
        spanner.tokenize_bool(null::bool) IS NULL,
        spanner.tokenize_fulltext(null::text) IS NULL,
        spanner.tokenize_fulltext(null::text[]) IS NULL,
        spanner.tokenize_jsonb(null::jsonb) IS NULL,
        spanner.tokenize_ngrams(null::text) IS NULL,
        spanner.tokenize_number(null::bigint) IS NULL,
        spanner.tokenize_substring(null::text) IS NULL,
        spanner.tokenlist_concat(null::spanner.tokenlist[]) IS NULL)sql"),
                IsOkAndHoldsRows(
                    {{true, true, true, true, true, true, true, true, true}}));
    EXPECT_THAT(Query(R"sql(SELECT spanner.tokenize_fulltext(''::text) IS NULL,
        spanner.tokenize_jsonb('null'::jsonb) IS NULL)sql"),
                IsOkAndHoldsRows({{false, false}}));
    return;
  }
  const std::vector<std::string> expressions = {
      "TOKEN(CAST(NULL AS STRING))",
      "TOKENIZE_BOOL(CAST(NULL AS BOOL))",
      "TOKENIZE_FULLTEXT(CAST(NULL AS STRING))",
      "TOKENIZE_FULLTEXT(CAST(NULL AS ARRAY<STRING>))",
      "TOKENIZE_JSON(CAST(NULL AS JSON))",
      "TOKENIZE_NGRAMS(CAST(NULL AS STRING))",
      "TOKENIZE_NUMBER(CAST(NULL AS INT64))",
      "TOKENIZE_SUBSTRING(CAST(NULL AS STRING))",
      "TOKENLIST_CONCAT(CAST(NULL AS ARRAY<TOKENLIST>))",
  };
  for (const std::string& expression : expressions) {
    SCOPED_TRACE(expression);
    EXPECT_THAT(Query(absl::StrCat("SELECT ", expression, " IS NULL")),
                IsOkAndHoldsRows({{true}}));
  }
  EXPECT_THAT(Query("SELECT TOKENIZE_FULLTEXT('') IS NULL"),
              IsOkAndHoldsRows({{false}}));
  EXPECT_THAT(Query("SELECT TOKENIZE_JSON(JSON 'null') IS NULL"),
              IsOkAndHoldsRows({{false}}));
}

TEST_P(SearchTest, SearchFunctionSupportOptionalArguments) {
  std::string query1 = R"sql(
          SELECT albumid
          FROM albums@{force_index=albumindex}
          WHERE SEARCH(summary_tokens, "top", enhance_query=>true)
            AND userid = 1
          ORDER BY albumid ASC)sql";
  GOOGLESQL_EXPECT_OK(Query(GetSqlQueryString(query1)));

  std::string query2 = R"sql(
          SELECT albumid
          FROM albums@{force_index=albumindex}
          WHERE SEARCH(summary_tokens,
                       "top",
                       enhance_query=>true,
                       language_tag=>"en-us")
            AND userid = 1
          ORDER BY albumid ASC)sql";
  GOOGLESQL_EXPECT_OK(Query(GetSqlQueryString(query2)));

  std::string query3 = R"sql(
          SELECT albumid
          FROM albums@{force_index=albumindex}
          WHERE SEARCH(summary_tokens,
                       "top",
                      enhance_query=>false,
                      language_tag=>"en-us")
            AND userid = 1
          ORDER BY albumid ASC)sql";
  GOOGLESQL_EXPECT_OK(Query(GetSqlQueryString(query3)));
}

// This only checks that the argument is accepted.
// TODO: Add support for logic.
TEST_P(SearchTest, SearchFunctionSupportDictionaryArgument) {
  if (dialect_ == database_api::DatabaseDialect::POSTGRESQL) {
    GTEST_SKIP() << "Skip PG for now.";
  }

  std::string query_search = R"sql(
          SELECT albumid
          FROM albums@{force_index=albumindex}
          WHERE SEARCH(summary_tokens, "top", dictionary=>"dict_table")
            AND userid = 1
          ORDER BY albumid ASC)sql";
  GOOGLESQL_EXPECT_OK(Query(GetSqlQueryString(query_search)));

  std::string query_score = R"sql(
          SELECT albumid
          FROM albums@{force_index=albumindex}
          WHERE SCORE(summary_tokens, "top", dictionary=>"dict_table") >= 0
            AND userid = 1
            AND SEARCH(summary_tokens, "top", dictionary=>"dict_table")
          ORDER BY albumid ASC)sql";
  GOOGLESQL_EXPECT_OK(Query(GetSqlQueryString(query_score)));

  std::string query_snippet = R"sql(
          SELECT albumid, SNIPPET(summary, "top", dictionary=>"dict_table")
          FROM albums@{force_index=albumindex}
          WHERE userid = 1
            AND SEARCH(summary_tokens, "top", dictionary=>"dict_table")
          ORDER BY albumid ASC)sql";
  GOOGLESQL_EXPECT_OK(Query(GetSqlQueryString(query_snippet)));
}

TEST_P(SearchTest, SearchFunctionWrongArguments) {
  std::string query = R"sql(
          SELECT albumid
          FROM albums@{force_index=albumindex}
          WHERE SEARCH(summary_tokens)
            AND userid = 1
          ORDER BY albumid ASC)sql";
  EXPECT_THAT(
      Query(GetSqlQueryString(query)),
      StatusIs(absl::StatusCode::kInvalidArgument,
               HasSubstr(IsGoogleStandardSql() ? "function SPANNER:SEARCH"
                                               : "function spanner.search")));
}

TEST_P(SearchTest, SearchFunctionDefaultDialect) {
  std::string query = R"sql(
          SELECT albumid
          FROM albums@{force_index=albumindex}
          WHERE SEARCH(summary_tokens, "global OR US")
            AND userid = 1
          ORDER BY albumid ASC)sql";
  EXPECT_THAT(Query(GetSqlQueryString(query)),
              IsOkAndHoldsRows({{1}, {2}, {4}, {7}}));
}

TEST_P(SearchTest, TokenizerOptionsAffectSearchQueries) {
  EXPECT_THAT(
      Query(GetSqlQueryString(R"sql(SELECT albumid FROM albums WHERE albumid = 0
                  AND SEARCH(TOKENIZE_FULLTEXT('Crème',
                                               remove_diacritics => TRUE),
                             'crème'))sql")),
      IsOkAndHoldsRows({{0}}));
  if (!IsGoogleStandardSql()) {
    GTEST_SKIP() << "The other options are covered for GoogleSQL only.";
  }
  EXPECT_THAT(
      Query(R"sql(SELECT albumid FROM albums WHERE albumid = 0
                  AND SEARCH(TOKENIZE_FULLTEXT('<h1>Café</h1>',
                                               content_type => 'text/html'),
                             'café', dialect => 'words'))sql"),
      IsOkAndHoldsRows({{0}}));
  EXPECT_THAT(
      Query(R"sql(SELECT albumid FROM albums WHERE albumid = 0
                  AND SEARCH_SUBSTRING(TOKENIZE_SUBSTRING('Café',
                                              remove_diacritics => TRUE),
                                       'càfe'))sql"),
      IsOkAndHoldsRows({{0}}));
  EXPECT_THAT(
      Query(R"sql(SELECT albumid FROM albums WHERE albumid = 0
                  AND SEARCH_NGRAMS(TOKENIZE_NGRAMS('Café',
                                            ngram_size_min => 4,
                                            ngram_size_max => 4,
                                            remove_diacritics => TRUE),
                                     'càfe', min_ngrams => 1))sql"),
      IsOkAndHoldsRows({{0}}));
}

TEST_P(SearchTest, SearchFunctionRQueryDialect) {
  std::string query = R"sql(
          SELECT albumid
          FROM albums@{force_index=albumindex}
          WHERE SEARCH(summary_tokens, "global OR US", dialect=>"rquery")
            AND userid = 1
          ORDER BY albumid ASC)sql";
  EXPECT_THAT(Query(GetSqlQueryString(query)),
              IsOkAndHoldsRows({{1}, {2}, {4}, {7}}));
}

TEST_P(SearchTest, SearchFunctionWordsDialect) {
  // No match because "foo" is not present in the documents.
  std::string query = R"sql(
          SELECT albumid
          FROM albums@{force_index=albumindex}
          WHERE SEARCH(summary_tokens, "global foo", dialect=>"words")
            AND userid = 1
          ORDER BY albumid ASC)sql";
  EXPECT_THAT(Query(GetSqlQueryString(query)), IsOkAndHoldsRows({}));

  // No match because "or" is a normal term and not present in the documents.
  query = R"sql(
          SELECT albumid
          FROM albums@{force_index=albumindex}
          WHERE SEARCH(summary_tokens, "top OR global", dialect=>"words")
            AND userid = 1
          ORDER BY albumid ASC)sql";
  EXPECT_THAT(Query(GetSqlQueryString(query)), IsOkAndHoldsRows({}));

  query = R"sql(
          SELECT albumid
          FROM albums@{force_index=albumindex}
          WHERE SEARCH(summary_tokens, "top global", dialect=>"words")
            AND userid = 1
          ORDER BY albumid ASC)sql";
  EXPECT_THAT(Query(GetSqlQueryString(query)), IsOkAndHoldsRows({{1}, {2}}));
}

TEST_P(SearchTest, SearchFunctionWordsPhraseDialect) {
  // No match because terms are not in order.
  std::string query = R"sql(
          SELECT albumid
          FROM albums@{force_index=albumindex}
          WHERE SEARCH(summary_tokens, "top global", dialect=>"words_phrase")
            AND userid = 1
          ORDER BY albumid ASC)sql";
  EXPECT_THAT(Query(GetSqlQueryString(query)), IsOkAndHoldsRows({}));

  query = R"sql(
          SELECT albumid
          FROM albums@{force_index=albumindex}
          WHERE SEARCH(summary_tokens, "top 50", dialect=>"words_phrase")
            AND userid = 1
          ORDER BY albumid ASC)sql";
  EXPECT_THAT(Query(GetSqlQueryString(query)), IsOkAndHoldsRows({{1}}));
}

TEST_P(SearchTest, SearchFunctionDialectIsCaseInsensitive) {
  std::string query = R"sql(
          SELECT albumid
          FROM albums@{force_index=albumindex}
          WHERE SEARCH(summary_tokens, "top global", dialect=>"WORDS")
            AND userid = 1
          ORDER BY albumid ASC)sql";
  EXPECT_THAT(Query(GetSqlQueryString(query)), IsOkAndHoldsRows({{1}, {2}}));

  query = R"sql(
          SELECT albumid
          FROM albums@{force_index=albumindex}
          WHERE SEARCH(summary_tokens, "top 50", dialect=>"WoRdS_PhRaSe")
            AND userid = 1
          ORDER BY albumid ASC)sql";
  EXPECT_THAT(Query(GetSqlQueryString(query)), IsOkAndHoldsRows({{1}}));

  query = R"sql(
          SELECT albumid
          FROM albums@{force_index=albumindex}
          WHERE SEARCH(summary_tokens, "global | US", dialect=>"RQuery")
            AND userid = 1
          ORDER BY albumid ASC)sql";
  EXPECT_THAT(Query(GetSqlQueryString(query)),
              IsOkAndHoldsRows({{1}, {2}, {4}, {7}}));
}

TEST_P(SearchTest, SearchFunctionInvalidDialect) {
  // Test invalid dialect value
  std::string query = R"sql(
          SELECT albumid
          FROM albums@{force_index=albumindex}
          WHERE SEARCH(summary_tokens, "top 50", dialect=>"invalid_dialect")
            AND userid = 1
          ORDER BY albumid ASC)sql";
  EXPECT_THAT(Query(GetSqlQueryString(query)),
              StatusIs(absl::StatusCode::kInvalidArgument,
                       HasSubstr("Invalid dialect: invalid_dialect")));
}

TEST_P(SearchTest, SearchSubStringRelativeSearch) {
  std::string query = R"sql(
          SELECT albumid
          FROM albums@{force_index=albumindex}
          WHERE SEARCH_SUBSTRING(summary_substr_relative_tokens, "son",
                                 relative_search_type=>"word_prefix")
            AND userid = 1
          ORDER BY albumid ASC)sql";
  GOOGLESQL_EXPECT_OK(Query(GetSqlQueryString(query)));
}

TEST_P(SearchTest, SearchSubStringRelativeSearchNotSupported) {
  std::string query = R"sql(
          SELECT albumid
          FROM albums@{force_index=albumindex}
          WHERE SEARCH_SUBSTRING(summary_substr_tokens, "son",
                                 relative_search_type=>"word_prefix")
            AND userid = 1
          ORDER BY albumid ASC)sql";
  EXPECT_THAT(Query(GetSqlQueryString(query)),
              StatusIs(absl::StatusCode::kInvalidArgument));
}

TEST_P(SearchTest, SearchSubstringWrongArguments) {
  std::string query1 = R"sql(
          SELECT albumid
          FROM albums@{force_index=albumindex}
          WHERE SEARCH_SUBSTRING(summary_substr_relative_tokens,
                                  "son",
                                  relative_search_type=>"word_prefix",
                                  language_tag=>"en-us",
                                  invalid_argument=>true)
            AND userid = 1
          ORDER BY albumid ASC)sql";

  auto expected_status = StatusIs(
      absl::StatusCode::kInvalidArgument,
      HasSubstr(IsGoogleStandardSql() ? "function SPANNER:SEARCH_SUBSTRING"
                                      : "function spanner.search_substring"));
  EXPECT_THAT(Query(GetSqlQueryString(query1)), expected_status);

  std::string query2 = R"sql(
          SELECT albumid
          FROM albums@{force_index=albumindex}
          WHERE SEARCH_SUBSTRING(summary_substr_tokens)
            AND userid = 1
          ORDER BY albumid ASC)sql";
  EXPECT_THAT(Query(GetSqlQueryString(query2)), expected_status);
}

TEST_P(SearchTest, UnableToSearchOnExactToken) {
  std::string query = R"sql(
          SELECT albumid
          FROM albums@{force_index=albumindex}
          WHERE SEARCH(name_tokens, 'name1')
            AND userid = 1
          ORDER BY albumid ASC)sql";
  EXPECT_THAT(Query(GetSqlQueryString(query)),
              StatusIs(absl::StatusCode::kInvalidArgument));
}

TEST_P(SearchTest, UnableToSearchOnNumericToken) {
  std::string query = R"sql(
          SELECT albumid
          FROM albums@{force_index=albumindex}
          WHERE SEARCH(length_tokens, 'name1')
            AND userid = 1
          ORDER BY albumid ASC)sql";
  EXPECT_THAT(Query(GetSqlQueryString(query)),
              StatusIs(absl::StatusCode::kInvalidArgument));
}

TEST_P(SearchTest, UnableToSearchSubstringOnNumericToken) {
  std::string query = R"sql(
          SELECT albumid
          FROM albums@{force_index=albumindex}
          WHERE SEARCH_SUBSTRING(length_tokens, 'name1')
            AND userid = 1
          ORDER BY albumid ASC)sql";
  EXPECT_THAT(Query(GetSqlQueryString(query)),
              StatusIs(absl::StatusCode::kInvalidArgument));
}

// failed comformance
TEST_P(SearchTest, UnableToSearchOnMixedConcatToken) {
  std::string query = R"sql(
          SELECT albumid
          FROM albums@{force_index=albumindex}
          WHERE SEARCH(
                  TOKENLIST_CONCAT([summary_tokens, summary_substr_tokens]),
                  'foo')
            AND userid = 1
          ORDER BY albumid ASC)sql";
  EXPECT_THAT(Query(GetSqlQueryString(query)),
              StatusIs(absl::StatusCode::kInvalidArgument));
}

// failed comformance
TEST_P(SearchTest, UnableToSearchSubstringOnMixedConcatToken) {
  std::string query = R"sql(
          SELECT albumid
          FROM albums@{force_index=albumindex}
          WHERE SEARCH_SUBSTRING(
                  TOKENLIST_CONCAT([summary_substr_tokens, summary_tokens]),
                  'foo')
            AND userid = 1
          ORDER BY albumid ASC)sql";
  EXPECT_THAT(Query(GetSqlQueryString(query)),
              StatusIs(absl::StatusCode::kInvalidArgument));
}

TEST_P(SearchTest, BasicSearch) {
  std::string query = R"sql(
          SELECT albumid
          FROM albums@{force_index=albumindex}
          WHERE SEARCH(summary_tokens, "global")
            AND userid = 1
          ORDER BY albumid ASC)sql";
  EXPECT_THAT(Query(GetSqlQueryString(query)), IsOkAndHoldsRows({{1}, {2}}));
}

TEST_P(SearchTest, BasicSearchOnTokenlistConcat) {
  std::string query = R"sql(
          SELECT albumid
          FROM albums@{force_index=albumindex}
          WHERE SEARCH(Summaries_Tokens, "global foo")
            AND userid = 1
          ORDER BY albumid ASC)sql";
  EXPECT_THAT(Query(GetSqlQueryString(query)), IsOkAndHoldsRows({{1}}));
}

TEST_P(SearchTest, BasicSearchConnectedPhraseOnTokenlistConcat) {
  // Phrase will not match across tokenlists.
  std::string query = R"sql(
          SELECT albumid
          FROM albums@{force_index=albumindex}
          WHERE SEARCH(Summaries_Tokens, "global-foo")
            AND userid = 1
          ORDER BY albumid ASC)sql";
  EXPECT_THAT(Query(GetSqlQueryString(query)), IsOkAndHoldsRows({}));
}

TEST_P(SearchTest, BasicSearchAnd) {
  std::string query = R"sql(
          SELECT albumid
          FROM albums@{force_index=albumindex}
          WHERE SEARCH(summary_tokens, "global US")
            AND userid = 2
          ORDER BY albumid ASC)sql";
  EXPECT_THAT(Query(GetSqlQueryString(query)), IsOkAndHoldsRows({{5}}));
}

TEST_P(SearchTest, BasicSearchOr) {
  std::string query = R"sql(
          SELECT albumid
          FROM albums@{force_index=albumindex}
          WHERE SEARCH(summary_tokens, "global | US")
            AND userid = 1
          ORDER BY albumid ASC)sql";
  EXPECT_THAT(Query(GetSqlQueryString(query)),
              IsOkAndHoldsRows({{1}, {2}, {4}, {7}}));
}

TEST_P(SearchTest, BasicSearchNot) {
  std::string query = R"sql(
          SELECT albumid
          FROM albums@{force_index=albumindex}
          WHERE SEARCH(summary_tokens, "-global")
            AND userid = 1
          ORDER BY albumid ASC)sql";
  EXPECT_THAT(Query(GetSqlQueryString(query)),
              IsOkAndHoldsRows(
                  {{0}, {4}, {6}, {7}, {9}, {10}, {12}, {13}, {16}, {17}}));
}

TEST_P(SearchTest, BasicSearchAround) {
  std::string query = R"sql(
          SELECT albumid
          FROM albums@{force_index=albumindex}
          WHERE SEARCH(summary_tokens, "global AROUND(2) song")
            AND userid = 1
          ORDER BY albumid ASC)sql";
  EXPECT_THAT(Query(GetSqlQueryString(query)), IsOkAndHoldsRows({{1}, {2}}));
}

TEST_P(SearchTest, BasicSearchConnectedPhrase) {
  std::string query = R"sql(
          SELECT albumid
          FROM albums@{force_index=albumindex}
          WHERE SEARCH(summary_tokens, "top-song")
            AND userid = 2
          ORDER BY albumid ASC)sql";
  EXPECT_THAT(Query(GetSqlQueryString(query)), IsOkAndHoldsRows({{5}}));
}

TEST_P(SearchTest, BasicSearchQuotedPhrase) {
  std::string query = R"sql(
          SELECT albumid
          FROM albums@{force_index=albumindex}
          WHERE SEARCH(summary_tokens, "\"global and US\"")
            AND userid = 2
          ORDER BY albumid ASC)sql";
  EXPECT_THAT(Query(GetSqlQueryString(query)), IsOkAndHoldsRows({{5}}));
}

TEST_P(SearchTest, CompositeSearch) {
  std::string query1 = R"sql(
          SELECT albumid
          FROM albums@{force_index=albumindex}
          WHERE SEARCH(summary_tokens, "top 1000 global | US")
            AND userid = 1
          ORDER BY albumid ASC)sql";
  EXPECT_THAT(Query(GetSqlQueryString(query1)), IsOkAndHoldsRows({{4}, {7}}));

  std::string query2 = R"sql(
          SELECT albumid
          FROM albums@{force_index=albumindex}
          WHERE SEARCH(summary_tokens, "global | US 50")
            AND userid = 1
          ORDER BY albumid ASC)sql";
  EXPECT_THAT(Query(GetSqlQueryString(query2)), IsOkAndHoldsRows({{1}}));

  std::string query3 = R"sql(
          SELECT albumid
          FROM albums@{force_index=albumindex}
          WHERE SEARCH(summary_tokens, "global top-song")
            AND userid = 2
          ORDER BY albumid ASC)sql";
  EXPECT_THAT(Query(GetSqlQueryString(query3)), IsOkAndHoldsRows({{5}}));

  std::string query4 = R"sql(
          SELECT albumid
          FROM albums@{force_index=albumindex}
          WHERE SEARCH(summary_tokens, "top-1000 -global")
            AND userid = 1
          ORDER BY albumid ASC)sql";
  EXPECT_THAT(Query(GetSqlQueryString(query4)), IsOkAndHoldsRows({{4}, {7}}));

  std::string query5 = R"sql(
          SELECT albumid
          FROM albums@{force_index=albumindex}
          WHERE SEARCH(summary_tokens, "\"top 1000\" -US")
            AND userid = 2
          ORDER BY albumid ASC)sql";
  EXPECT_THAT(Query(GetSqlQueryString(query5)), IsOkAndHoldsRows({{8}}));
}

// If there are multiple AROUND distances, use default 5
// In row 5 of the table, 'global' is 3 away from top and 4 away from song,
// matches neither of the AROUND condition but since there are two distances
// provided, the code falls back to default distance, and return row 5 as a
// match.
TEST_P(SearchTest, MultipleAroundDistanceFallBackToDefault) {
  std::string query = R"sql(
          SELECT albumid
          FROM albums@{force_index=albumindex}
          WHERE SEARCH(summary_tokens, "global AROUND(1) top AROUND(2) song")
            AND userid = 1
          ORDER BY albumid ASC)sql";
  EXPECT_THAT(Query(GetSqlQueryString(query)), IsOkAndHoldsRows({{1}, {2}}));
}

TEST_P(SearchTest, SearchIsCaseInsensitive) {
  std::string query = R"sql(
          SELECT albumid
          FROM albums@{force_index=albumindex}
          WHERE SEARCH(summary_tokens, "GLObal us song")
            AND userid = 2
          ORDER BY albumid ASC)sql";
  EXPECT_THAT(Query(GetSqlQueryString(query)), IsOkAndHoldsRows({{5}}));
}

TEST_P(SearchTest, SearchPhraseNotAcrossArrayBoundary) {
  std::string query = R"sql(
          SELECT albumid
          FROM albums@{force_index=albumindex}
          WHERE SEARCH(Tracks_Tokens, '"track1 track2"')
            AND userid = 2
          ORDER BY albumid ASC)sql";
  EXPECT_THAT(Query(GetSqlQueryString(query)), IsOkAndHoldsRows({{5}}));
}

TEST_P(SearchTest, SearchAroundNotAcrossArrayBoundary) {
  std::string query = R"sql(
          SELECT albumid
          FROM albums@{force_index=albumindex}
          WHERE SEARCH(Tracks_Tokens, "track1 AROUND(2) track2")
            AND userid = 2
          ORDER BY albumid ASC)sql";
  EXPECT_THAT(Query(GetSqlQueryString(query)), IsOkAndHoldsRows({{5}}));
}

TEST_P(SearchTest, EmptyQueryAlwaysReturnFalse) {
  std::string query = R"sql(
          SELECT albumid
          FROM albums@{force_index=albumindex}
          WHERE SEARCH(summary_tokens, "")
            AND userid = 1
          ORDER BY albumid ASC)sql";
  EXPECT_THAT(Query(GetSqlQueryString(query)),
              // Row 15 has NULL summary so it does not show up in the result
              IsOkAndHoldsRows({}));
}

TEST_P(SearchTest, NullQueryAlwaysReturnFalse) {
  std::string query = R"sql(
          SELECT albumid
          FROM albums@{force_index=albumindex}
          WHERE SEARCH(summary_tokens, CAST(NULL AS STRING))
            AND userid = 1
          ORDER BY albumid ASC)sql";
  EXPECT_THAT(Query(GetSqlQueryString(query)), IsOkAndHoldsRows({}));
}

TEST_P(SearchTest, NullTokenListAlwaysReturnFalse) {
  // Trying to perform search on row 16 (albumid = 15), where Summary is null.
  std::string query1 = R"sql(
          SELECT Name
          FROM albums@{force_index=albumindex}
          WHERE SEARCH(summary_tokens, "US")
            AND userid = 1
            AND albumid > 13
          ORDER BY albumid ASC)sql";
  EXPECT_THAT(Query(GetSqlQueryString(query1)), IsOkAndHoldsRows({}));

  std::string query2 = R"sql(
          SELECT Name
          FROM albums@{force_index=albumindex}
          WHERE SEARCH(summary_tokens, CAST(NULL AS STRING))
            AND userid = 1
            AND albumid > 13
          ORDER BY albumid ASC)sql";
  EXPECT_THAT(Query(GetSqlQueryString(query2)), IsOkAndHoldsRows({}));
}

TEST_P(SearchTest, BasicSearchSubstring) {
  std::string query = R"sql(
          SELECT albumid
          FROM albums@{force_index=albumindex}
          WHERE SEARCH_SUBSTRING(summary_substr_tokens, 'son')
            AND userid = 1
          ORDER BY albumid ASC)sql";
  EXPECT_THAT(Query(GetSqlQueryString(query)),
              IsOkAndHoldsRows({{1}, {2}, {4}, {6}, {7}}));
}

TEST_P(SearchTest, BasicSearchSubstringWithTokenlistConcat) {
  std::string query1 = R"sql(
          SELECT albumid
          FROM albums@{force_index=albumindex}
          WHERE SEARCH_SUBSTRING(summaries_substr_tokens, 'son')
            AND userid = 1
          ORDER BY albumid ASC)sql";
  EXPECT_THAT(Query(GetSqlQueryString(query1)),
              IsOkAndHoldsRows({{1}, {2}, {4}, {6}, {7}}));

  std::string query2 = R"sql(
          SELECT albumid
          FROM albums@{force_index=albumindex}
          WHERE SEARCH_SUBSTRING(summaries_substr_tokens, 'foo')
            AND userid = 1
          ORDER BY albumid ASC)sql";
  EXPECT_THAT(Query(GetSqlQueryString(query2)), IsOkAndHoldsRows({{1}, {4}}));
}

TEST_P(SearchTest, BasicSearchSubstringSubstringInMiddle) {
  std::string query = R"sql(
          SELECT albumid
          FROM albums@{force_index=albumindex}
          WHERE SEARCH_SUBSTRING(summary_substr_tokens, 'loba')
            AND userid = 1
          ORDER BY albumid ASC)sql";
  EXPECT_THAT(Query(GetSqlQueryString(query)), IsOkAndHoldsRows({{1}, {2}}));
}

TEST_P(SearchTest, BasicSearchSubstringMultiSubstrings) {
  std::string query = R"sql(
          SELECT albumid
          FROM albums@{force_index=albumindex}
          WHERE SEARCH_SUBSTRING(summary_substr_tokens, 'loba 100')
            AND userid = 1
          ORDER BY albumid ASC)sql";
  EXPECT_THAT(Query(GetSqlQueryString(query)), IsOkAndHoldsRows({{2}}));
}

TEST_P(SearchTest, SearchSubstringWithMatchingPartition) {
  std::string query = R"sql(
          SELECT albumid
          FROM albums@{force_index=albumindex}
          WHERE SEARCH_SUBSTRING(summary_substr_tokens, 'son')
            AND userid = 1
            AND albumid > 3
            AND albumid < 7
          ORDER BY albumid ASC)sql";
  EXPECT_THAT(Query(GetSqlQueryString(query)), IsOkAndHoldsRows({{4}, {6}}));
}

TEST_P(SearchTest, SearchSubstringWithUnMatchingPartition) {
  std::string query = R"sql(
          SELECT albumid
          FROM albums@{force_index=albumindex}
          WHERE SEARCH_SUBSTRING(summary_substr_tokens, 'loba')
            AND userid = 1
            AND albumid > 8
          ORDER BY albumid ASC)sql";
  EXPECT_THAT(Query(GetSqlQueryString(query)), IsOkAndHoldsRows({}));
}

TEST_P(SearchTest, SearchSubstringWithWordPrefix) {
  std::string query1 = R"sql(
          SELECT albumid
          FROM albums@{force_index=albumindex}
          WHERE SEARCH_SUBSTRING(Summary_SubStr_Relative_All_Tokens,
                                 'son',
                                 relative_search_type=>"word_prefix")
            AND userid = 1
            AND albumid < 7
          ORDER BY albumid ASC)sql";
  EXPECT_THAT(Query(GetSqlQueryString(query1)),
              IsOkAndHoldsRows({{1}, {2}, {4}, {6}}));

  std::string query2 = R"sql(
          SELECT albumid
          FROM albums@{force_index=albumindex}
          WHERE SEARCH_SUBSTRING(Summary_SubStr_Relative_All_Tokens,
                                 'ong',
                                 relative_search_type=>"word_prefix")
            AND userid = 1
            AND albumid < 7
          ORDER BY albumid ASC)sql";
  EXPECT_THAT(Query(GetSqlQueryString(query2)), IsOkAndHoldsRows({}));
}

TEST_P(SearchTest, SearchSubstringWithWordSuffix) {
  std::string query1 = R"sql(
          SELECT albumid
          FROM albums@{force_index=albumindex}
          WHERE SEARCH_SUBSTRING(Summary_SubStr_Relative_All_Tokens,
                                 'son',
                                 relative_search_type=>"word_suffix")
            AND userid = 1
            AND albumid < 7
          ORDER BY albumid ASC)sql";
  EXPECT_THAT(Query(GetSqlQueryString(query1)), IsOkAndHoldsRows({}));

  std::string query2 = R"sql(
          SELECT albumid
          FROM albums@{force_index=albumindex}
          WHERE SEARCH_SUBSTRING(Summary_SubStr_Relative_All_Tokens,
                                 'ong',
                                 relative_search_type=>"word_suffix")
            AND userid = 1
            AND albumid < 7
          ORDER BY albumid ASC)sql";
  EXPECT_THAT(Query(GetSqlQueryString(query2)),
              IsOkAndHoldsRows({{1}, {2}, {4}, {6}}));
}

TEST_P(SearchTest, SearchSubstringWithValuePrefix) {
  std::string query = R"sql(
          SELECT albumid
          FROM albums@{force_index=albumindex}
          WHERE SEARCH_SUBSTRING(Summary_SubStr_Relative_All_Tokens,
                                 'son',
                                 relative_search_type=>"value_prefix")
            AND userid = 1
            AND albumid < 7
          ORDER BY albumid ASC)sql";
  EXPECT_THAT(Query(GetSqlQueryString(query)), IsOkAndHoldsRows({{4}}));
}

TEST_P(SearchTest, SearchSubstringWithValueSuffix) {
  std::string query = R"sql(
          SELECT albumid
          FROM albums@{force_index=albumindex}
          WHERE SEARCH_SUBSTRING(Summary_SubStr_Relative_All_Tokens,
                                 'ong',
                                 relative_search_type=>"value_suffix")
            AND userid = 1
            AND albumid < 7
          ORDER BY albumid ASC)sql";
  EXPECT_THAT(Query(GetSqlQueryString(query)),
              IsOkAndHoldsRows({{1}, {2}, {6}}));
}

TEST_P(SearchTest, SearchSubstringWithPhrase) {
  std::string query1 = R"sql(
          SELECT albumid
          FROM albums@{force_index=albumindex}
          WHERE SEARCH_SUBSTRING(Summary_SubStr_Relative_All_Tokens,
                                 'US to',
                                 relative_search_type=>"phrase")
            AND userid = 1
          ORDER BY albumid ASC)sql";
  EXPECT_THAT(Query(GetSqlQueryString(query1)), IsOkAndHoldsRows({{7}}));

  // Even though the length of 'us' is less than ngrams_size_min, when
  // considering the leading and trailing space ' us ', it's still a match.
  std::string query2 = R"sql(
          SELECT albumid
          FROM albums@{force_index=albumindex}
          WHERE SEARCH_SUBSTRING(Summary_SubStr_Relative_All_Tokens,
                                 'us',
                                 relative_search_type=>"phrase")
            AND userid = 1
          ORDER BY albumid ASC)sql";
  EXPECT_THAT(Query(GetSqlQueryString(query2)), IsOkAndHoldsRows({{4}, {7}}));

  // There's no phrase/word matches "to".
  std::string query3 = R"sql(
          SELECT albumid
          FROM albums@{force_index=albumindex}
          WHERE SEARCH_SUBSTRING(Summary_SubStr_Relative_All_Tokens,
                                 'to',
                                 relative_search_type=>"phrase")
            AND userid = 1
          ORDER BY albumid ASC)sql";
  EXPECT_THAT(Query(GetSqlQueryString(query3)), IsOkAndHoldsRows({}));
}

TEST_P(SearchTest, SearchTooShortSubstring) {
  std::string query1 = R"sql(
          SELECT albumid
          FROM albums@{force_index=albumindex}
          WHERE SEARCH_SUBSTRING(summary_substr_tokens, 'ba')
            AND userid = 1
          ORDER BY albumid ASC)sql";
  EXPECT_THAT(Query(GetSqlQueryString(query1)), IsOkAndHoldsRows({}));

  std::string query2 = R"sql(
          SELECT albumid
          FROM albums@{force_index=albumindex}
          WHERE SEARCH_SUBSTRING(summary_substr_tokens, 'bal us')
            AND userid = 1
          ORDER BY albumid ASC)sql";
  EXPECT_THAT(Query(GetSqlQueryString(query2)), IsOkAndHoldsRows({}));
}

TEST_P(SearchTest, SearchEmptySubstringAlwaysReturnEmpty) {
  std::string query = R"sql(
          SELECT albumid
          FROM albums@{force_index=albumindex}
          WHERE SEARCH_SUBSTRING(summary_substr_tokens, '')
            AND userid = 1
          ORDER BY albumid ASC)sql";
  EXPECT_THAT(Query(GetSqlQueryString(query)), IsOkAndHoldsRows({}));
}

TEST_P(SearchTest, SearchNullSubstringAlwaysReturnEmpty) {
  std::string query = R"sql(
          SELECT albumid
          FROM albums@{force_index=albumindex}
          WHERE SEARCH_SUBSTRING(summary_substr_tokens, CAST(NULL AS STRING))
            AND userid = 1
          ORDER BY albumid ASC)sql";
  EXPECT_THAT(Query(GetSqlQueryString(query)), IsOkAndHoldsRows({}));
}

TEST_P(SearchTest, SearchNormalizedEmptySubstring) {
  std::string query = R"sql(
          SELECT albumid
          FROM albums@{force_index=albumindex}
          WHERE SEARCH_SUBSTRING(summary_substr_tokens, '*^& %- // ')
            AND userid = 1
          ORDER BY albumid ASC)sql";
  EXPECT_THAT(Query(GetSqlQueryString(query)), IsOkAndHoldsRows({}));
}

TEST_P(SearchTest, BasicSearchNgrams) {
  std::string query = R"sql(
          SELECT albumid
          FROM albums@{force_index=albumindex}
          WHERE SEARCH_NGRAMS(Name_Ngrams_Tokens, 'rock')
            AND userid = 1
          ORDER BY albumid ASC)sql";
  EXPECT_THAT(Query(GetSqlQueryString(query)), IsOkAndHoldsRows({{16}, {17}}));
}

TEST_P(SearchTest, FuzzySearchNgrams) {
  std::string query = R"sql(
          SELECT albumid
          FROM albums@{force_index=albumindex}
          WHERE SEARCH_NGRAMS(Name_Ngrams_Tokens, 'electronci')
            AND userid = 1
          ORDER BY albumid ASC)sql";
  EXPECT_THAT(Query(GetSqlQueryString(query)), IsOkAndHoldsRows({{17}}));
}

TEST_P(SearchTest, FuzzySearchNgramsOnDiffLargerThanMinNgramsReturnsEmpty) {
  std::string query = R"sql(
          SELECT albumid
          FROM albums@{force_index=albumindex}
          WHERE SEARCH_NGRAMS(Name_Ngrams_Tokens, 'elcetrnoci', min_ngrams=>5)
            AND userid = 1
          ORDER BY albumid ASC)sql";
  EXPECT_THAT(Query(GetSqlQueryString(query)), IsOkAndHoldsRows({}));
}

TEST_P(SearchTest, SearchNgramsWithTokenizeSubstringOnEmail) {
  std::string query = R"sql(
    SELECT AccountId
    FROM AccountSearchTerms@{force_index=AccountSearchFiltersIndex}
    WHERE SEARCH_NGRAMS(SearchPhraseSubstringTokens, 'email')
    ORDER BY AccountId ASC
  )sql";
  EXPECT_THAT(Query(GetSqlQueryString(query)), IsOkAndHoldsRows({{1}}));
}

TEST_P(SearchTest, FuzzySearchNgramsWithTokenizeSubstring) {
  std::string query = R"sql(
    SELECT Id FROM Test@{force_index=SearchTokenIndex}
    WHERE SEARCH_NGRAMS(Search_Tokens, "hello there")
    ORDER BY Id ASC)sql";
  EXPECT_THAT(Query(GetSqlQueryString(query)),
              IsOkAndHoldsRows({{"id1"}, {"id2"}, {"id3"}}));
}

TEST_P(SearchTest, SearchEmptyNgramsAlwaysReturnEmpty) {
  std::string query = R"sql(
          SELECT albumid
          FROM albums@{force_index=albumindex}
          WHERE SEARCH_NGRAMS(Name_Ngrams_Tokens, '')
            AND userid = 1
          ORDER BY albumid ASC)sql";
  EXPECT_THAT(Query(GetSqlQueryString(query)), IsOkAndHoldsRows({}));
}

TEST_P(SearchTest, SearchNullNgramsAlwaysReturnEmpty) {
  std::string query = R"sql(
          SELECT albumid
          FROM albums@{force_index=albumindex}
          WHERE SEARCH_NGRAMS(Name_Ngrams_Tokens, CAST(NULL AS STRING))
            AND userid = 1
          ORDER BY albumid ASC)sql";
  EXPECT_THAT(Query(GetSqlQueryString(query)), IsOkAndHoldsRows({}));
}

TEST_P(SearchTest, SearchNormalizedEmptyNgrams) {
  std::string query = R"sql(
          SELECT albumid
          FROM albums@{force_index=albumindex}
          WHERE SEARCH_NGRAMS(Name_Ngrams_Tokens, '*^& %- // ')
            AND userid = 1
          ORDER BY albumid ASC)sql";
  EXPECT_THAT(Query(GetSqlQueryString(query)), IsOkAndHoldsRows({}));
}

TEST_P(SearchTest, SearchSupportsEnhanceQueryHint) {
  std::string query = R"sql(
          @{require_enhance_query=true, enhance_query_timeout_ms=300}
          SELECT albumid
          FROM albums@{force_index=albumindex}
          WHERE SEARCH(summary_tokens, "GLObal us song")
            AND userid = 2
          ORDER BY albumid ASC)sql";
  EXPECT_THAT(Query(GetSqlQueryString(query)), IsOkAndHoldsRows({{5}}));
}

// The logic of SNIPPET, SCORE, and SCORE_NGRAMS functions are different between
// the Cloud Spanner and the Emulator. It is expected that the Cloud Spanner and
// the Emulator return different results for those functions. Thus we only check
// if the call is successful.
TEST_P(SearchTest, BasicScore) {
  std::string query = R"sql(
      SELECT SCORE(summary_tokens, 'top')
      FROM albums
      WHERE userid = 1 AND SEARCH(summary_tokens, 'top'))sql";
  GOOGLESQL_EXPECT_OK(Query(GetSqlQueryString(query)));
}

TEST_P(SearchTest, ScoreFunctionSupportOptionalArguments) {
  std::string query1 = R"sql(
          SELECT albumid
          FROM albums@{force_index=albumindex}
          WHERE SCORE(summary_tokens, "top", enhance_query=>true) >= 1
            AND userid = 1
            AND SEARCH(summary_tokens, "top")
          ORDER BY albumid ASC)sql";
  GOOGLESQL_EXPECT_OK(Query(GetSqlQueryString(query1)));

  std::string query2 = R"sql(
          SELECT albumid
          FROM albums@{force_index=albumindex}
          WHERE SCORE(summary_tokens,
                      "top",
                      enhance_query=>false,
                      language_tag=>"en-us") >= 1
            AND userid = 1
            AND SEARCH(summary_tokens, "top")
          ORDER BY albumid ASC)sql";
  GOOGLESQL_EXPECT_OK(Query(GetSqlQueryString(query2)));
}

TEST_P(SearchTest, ScoreFunctionWrongArguments) {
  std::string query = R"sql(
          SELECT albumid
          FROM albums@{force_index=albumindex}
          WHERE SCORE(summary_tokens, "top", "en-us") >= 1
            AND userid = 1
          ORDER BY albumid ASC)sql";
  EXPECT_THAT(Query(GetSqlQueryString(query)),
              StatusIs(absl::StatusCode::kInvalidArgument));
}

TEST_P(SearchTest, ScoreFunctionDefaultDialectArgument) {
  // The `dialect` argument is not specified.
  std::string query = R"sql(
      SELECT SCORE(summary_tokens, 'top')
      FROM albums
      WHERE userid = 1 AND SEARCH(summary_tokens, 'top'))sql";
  GOOGLESQL_EXPECT_OK(Query(GetSqlQueryString(query)));

  query = R"sql(
      SELECT SCORE(summary_tokens, 'top', dialect=>CAST(NULL AS STRING))
      FROM albums
      WHERE userid = 1 AND SEARCH(summary_tokens, 'top'))sql";
  GOOGLESQL_EXPECT_OK(Query(GetSqlQueryString(query)));
}

TEST_P(SearchTest, ScoreFunctionSupportsDialectArgument) {
  std::string query = R"sql(
      SELECT SCORE(summary_tokens, 'top', dialect=>'rquery')
      FROM albums
      WHERE userid = 1 AND SEARCH(summary_tokens, 'top'))sql";
  GOOGLESQL_EXPECT_OK(Query(GetSqlQueryString(query)));

  query = R"sql(
      SELECT SCORE(summary_tokens, 'top', dialect=>'WORDS')
      FROM albums
      WHERE userid = 1 AND SEARCH(summary_tokens, 'top'))sql";
  GOOGLESQL_EXPECT_OK(Query(GetSqlQueryString(query)));

  query = R"sql(
      SELECT SCORE(summary_tokens, 'top', dialect=>'Words_Phrase')
      FROM albums
      WHERE userid = 1 AND SEARCH(summary_tokens, 'top'))sql";
  GOOGLESQL_EXPECT_OK(Query(GetSqlQueryString(query)));
}

TEST_P(SearchTest, ScoreFunctionInvalidDialect) {
  std::string query = R"sql(
      SELECT SCORE(summary_tokens, 'top', dialect=>'invalid_dialect')
      FROM albums
      WHERE userid = 1 AND SEARCH(summary_tokens, 'top'))sql";
  EXPECT_THAT(Query(GetSqlQueryString(query)),
              StatusIs(absl::StatusCode::kInvalidArgument,
                       HasSubstr("Invalid dialect: invalid_dialect")));
}

TEST_P(SearchTest, ScoreFunctionSupportsOptionsWithLocalSemantics) {
  const std::string options_arg =
      IsGoogleStandardSql() ? "options=>JSON '$0'" : "options=>'$0'::jsonb";
  for (const std::string& options :
       {std::string(R"({"version": 2})"),
        std::string(R"({"token_category_weights": {"title": 2.0}})")}) {
    std::string query = absl::Substitute(
        R"sql(
      SELECT albumid
      FROM albums
      WHERE userid = 1 AND SEARCH(summary_tokens, 'top')
        AND SCORE(summary_tokens, 'top', $0) > 0
      ORDER BY albumid)sql",
        absl::Substitute(options_arg, options));
    EXPECT_THAT(Query(GetSqlQueryString(query)),
                IsOkAndHoldsRows({{1}, {2}, {4}, {7}}))
        << options;
  }
  if (in_prod_env()) return;

  // The local scorer has no bigram or term frequency statistics.
  std::string query = absl::Substitute(
      R"sql(
      SELECT SCORE(summary_tokens, 'top', $0)
      FROM albums
      WHERE userid = 1 AND SEARCH(summary_tokens, 'top'))sql",
      absl::Substitute(options_arg, R"({"bigram_weight": 3.0})"));
  EXPECT_THAT(Query(GetSqlQueryString(query)),
              StatusIs(absl::StatusCode::kUnimplemented));
}

TEST_P(SearchTest, ScoreNgramsArrayAggregator) {
  if (!IsGoogleStandardSql()) {
    GTEST_SKIP() << "array_aggregator is only documented for GoogleSQL";
  }
  // Album 2 has the tracks ["track1", "track2"]: the best element matches
  // "track1" better than the flattened array does.
  EXPECT_THAT(Query(R"sql(
      SELECT albumid
      FROM albums
      WHERE albumid = 2
        AND SCORE_NGRAMS(tracks_substring_tokens, "track1",
                         array_aggregator=>"max_element") >
            SCORE_NGRAMS(tracks_substring_tokens, "track1",
                         array_aggregator=>"flatten")
        AND SCORE_NGRAMS(tracks_substring_tokens, "track1") =
            SCORE_NGRAMS(tracks_substring_tokens, "track1",
                         array_aggregator=>"flatten"))sql"),
              IsOkAndHoldsRows({{2}}));

  EXPECT_THAT(Query(R"sql(
      SELECT SCORE_NGRAMS(summary_substr_tokens, "top",
                          array_aggregator=>"max_element")
      FROM albums
      WHERE albumid = 1)sql"),
              StatusIs(absl::StatusCode::kInvalidArgument));
  EXPECT_THAT(Query(R"sql(
      SELECT SCORE_NGRAMS(tracks_substring_tokens, "track1",
                          array_aggregator=>"sum")
      FROM albums
      WHERE albumid = 2)sql"),
              StatusIs(absl::StatusCode::kInvalidArgument));
}

TEST_P(SearchTest, BasicScoreNgrams) {
  std::string query = R"sql(
      SELECT SCORE_NGRAMS(Tracks_Substring_Tokens, "top")
      FROM albums
      WHERE userid = 1 AND SEARCH(summary_tokens, "top"))sql";
  GOOGLESQL_EXPECT_OK(Query(GetSqlQueryString(query)));
}

TEST_P(SearchTest, ScoreNgramsFunctionSupportOptionalArguments) {
  std::string query1 = R"sql(
          SELECT albumid
          FROM albums@{force_index=albumindex}
          WHERE SCORE_NGRAMS(Tracks_Substring_Tokens, "top") >= 0.25
            AND userid = 1
            AND SEARCH(summary_tokens, "top")
          ORDER BY albumid ASC)sql";
  GOOGLESQL_EXPECT_OK(Query(GetSqlQueryString(query1)));

  std::string query2 = R"sql(
          SELECT albumid
          FROM albums@{force_index=albumindex}
          WHERE SCORE_NGRAMS(Tracks_Substring_Tokens,
                      "top",
                      algorithm=>"trigrams",
                      language_tag=>"en-us") >= 0.25
            AND userid = 1
            AND SEARCH(summary_tokens, "top")
          ORDER BY albumid ASC)sql";
  GOOGLESQL_EXPECT_OK(Query(GetSqlQueryString(query2)));
}

TEST_P(SearchTest, ScoreNgramsFunctionWrongArguments) {
  std::string query = R"sql(
          SELECT albumid
          FROM albums@{force_index=albumindex}
          WHERE SCORE_NGRAMS(Tracks_Substring_Tokens, "trigrams", "en-us") >= 0.25
            AND userid = 1
          ORDER BY albumid ASC)sql";
  EXPECT_THAT(Query(GetSqlQueryString(query)),
              StatusIs(absl::StatusCode::kInvalidArgument));
}

TEST_P(SearchTest, ScoreNgramsFunctionWrongTokenListType) {
  std::string query = R"sql(
          SELECT albumid
          FROM albums@{force_index=albumindex}
          WHERE SCORE_NGRAMS(Tracks_Tokens, "top") >= 0.25
            AND userid = 1
            AND SEARCH(summary_tokens, "top")
          ORDER BY albumid ASC)sql";
  EXPECT_THAT(Query(GetSqlQueryString(query)),
              StatusIs(absl::StatusCode::kInvalidArgument));
}

TEST_P(SearchTest, BasicSnippet) {
  std::string query = R"sql(
      SELECT SNIPPET(Summary, 'top')
      FROM albums
      WHERE userid = 1)sql";
  GOOGLESQL_EXPECT_OK(Query(GetSqlQueryString(query)));
}

TEST_P(SearchTest, SnippetMatchesDocumentedLayout) {
  if (IsGoogleStandardSql()) {
    EXPECT_THAT(
        Query("SELECT TO_JSON_STRING(SNIPPET('Rock albums rock.', 'rock'))"),
        IsOkAndHoldsRows(
            {{R"({"snippets":[{"highlights":[{"begin":"1","end":"5"},)"
              R"({"begin":"13","end":"17"}],"snippet":"Rock albums rock.",)"
              R"("source_begin":1,"source_end":18}]})"}}));
  } else {
    // JSONB orders object keys by length.
    EXPECT_THAT(
        Query("SELECT spanner.snippet('Rock albums rock.', 'rock')::text"),
        IsOkAndHoldsRows(
            {{R"({"snippets": [{"snippet": "Rock albums rock.", )"
              R"("highlights": [{"end": "5", "begin": "1"}, )"
              R"({"end": "17", "begin": "13"}], "source_end": 18, )"
              R"("source_begin": 1}]})"}}));
  }
}

TEST_P(SearchTest, DebugTokenlist) {
  EXPECT_THAT(
      Query(GetSqlQueryString(
          "SELECT DEBUG_TOKENLIST(TOKENIZE_FULLTEXT('Hello DB #World'))")),
      IsOkAndHoldsRows({{"hello(boundary), db, [#world, world](boundary)"}}));
  EXPECT_THAT(Query(GetSqlQueryString(R"sql(
      SELECT DEBUG_TOKENLIST(summary_tokens)
      FROM albums
      WHERE albumid = 1)sql")),
              IsOkAndHoldsRows({{"global(boundary), top, 50, song(boundary)"}}));
}

TEST_P(SearchTest, RQueryUnicodeTerms) {
  EXPECT_THAT(
      Query(GetSqlQueryString(R"sql(
          SELECT albumid FROM albums WHERE albumid = 0
            AND SEARCH(TOKENIZE_FULLTEXT('Café Crème'), 'crème café'))sql")),
      IsOkAndHoldsRows({{0}}));
  EXPECT_THAT(
      Query(GetSqlQueryString(R"sql(
          SELECT albumid FROM albums WHERE albumid = 0
            AND SEARCH(TOKENIZE_FULLTEXT('Café Crème'), 'thé | crème'))sql")),
      IsOkAndHoldsRows({{0}}));
  EXPECT_THAT(
      Query(GetSqlQueryString(R"sql(
          SELECT albumid FROM albums WHERE albumid = 0
            AND SEARCH(TOKENIZE_FULLTEXT('Café Crème'), 'thé'))sql")),
      IsOkAndHoldsRows({}));
  // Text without spaces is split into words, in the value and in the query.
  EXPECT_THAT(
      Query(GetSqlQueryString(R"sql(
          SELECT albumid FROM albums WHERE albumid = 0
            AND SEARCH(TOKENIZE_FULLTEXT('東京タワーの夜景', language_tag => 'ja'),
                       '東京タワー'))sql")),
      IsOkAndHoldsRows({{0}}));
}

TEST_P(SearchTest, HtmlContentSkipsScriptsAndDecodesEntities) {
  const std::string tokens = R"sql(TOKENIZE_FULLTEXT(
      '<style>.secret { }</style><script>var secret;</script><p>caf&eacute; cr&egrave;me</p>',
      content_type => 'text/html'))sql";
  EXPECT_THAT(Query(GetSqlQueryString(absl::StrCat(
                  "SELECT DEBUG_TOKENLIST(", tokens, ")"))),
              IsOkAndHoldsRows({{"café(boundary), crème(boundary)"}}));
  EXPECT_THAT(Query(GetSqlQueryString(absl::StrCat(
                  "SELECT albumid FROM albums WHERE albumid = 0 AND SEARCH(",
                  tokens, ", 'secret')"))),
              IsOkAndHoldsRows({}));
}

TEST_P(SearchTest, ShortTokensOnlyForAnchors) {
  const std::string tokens =
      absl::StrCat(IsGoogleStandardSql() ? "TOKENIZE_SUBSTRING"
                                         : "spanner.tokenize_substring",
                   "('Heavy Metal', relative_search_types => ",
                   IsGoogleStandardSql() ? "['word_prefix']"
                                         : "ARRAY['word_prefix']",
                   ", short_tokens_only_for_anchors => TRUE)");
  const std::string search = IsGoogleStandardSql() ? "SEARCH_SUBSTRING"
                                                   : "spanner.search_substring";
  EXPECT_THAT(Query(absl::StrCat("SELECT ", search, "(", tokens, ", 'he'), ",
                                 search, "(", tokens, ", 'heav'), ", search,
                                 "(", tokens,
                                 ", 'he', relative_search_type => "
                                 "'word_prefix')")),
              IsOkAndHoldsRows({{false, true, true}}));
}

TEST_P(SearchTest, TokenlistConcatWithMixedRemoveDiacritics) {
  const std::string concat =
      IsGoogleStandardSql()
          ? R"sql(TOKENLIST_CONCAT([
                TOKENIZE_FULLTEXT('Café'),
                TOKENIZE_FULLTEXT('Crème', remove_diacritics => TRUE)]))sql"
          : R"sql(spanner.tokenlist_concat(ARRAY[
                spanner.tokenize_fulltext('Café'),
                spanner.tokenize_fulltext('Crème',
                                          remove_diacritics => TRUE)]))sql";
  // Queries on the concatenation ignore diacritics in both parts.
  EXPECT_THAT(Query(IsGoogleStandardSql()
                        ? absl::StrCat("SELECT SEARCH(", concat,
                                       ", 'café'), SEARCH(", concat,
                                       ", 'creme')")
                        : absl::StrCat("SELECT spanner.search(", concat,
                                       ", 'café'), spanner.search(", concat,
                                       ", 'creme')")),
              IsOkAndHoldsRows({{true, true}}));
}

TEST_P(SearchTest, InvalidMaxSnippets) {
  std::string query1 = R"sql(
      SELECT SNIPPET(Summary, 'top', max_snippets=>2147483648)
      FROM albums
      WHERE userid = 1)sql";
  EXPECT_THAT(Query(GetSqlQueryString(query1)),
              StatusIs(absl::StatusCode::kInvalidArgument));

  std::string query2 = R"sql(
      SELECT SNIPPET(Summary, 'top', max_snippets=>-1)
      FROM albums
      WHERE userid = 1)sql";
  EXPECT_THAT(Query(GetSqlQueryString(query2)),
              StatusIs(absl::StatusCode::kInvalidArgument));
}

TEST_P(SearchTest, InvalidMaxSnippetWidth) {
  std::string query1 = R"sql(
      SELECT SNIPPET(Summary, 'top', max_snippet_width=>2147483648)
      FROM albums
      WHERE userid = 1)sql";
  EXPECT_THAT(Query(GetSqlQueryString(query1)),
              StatusIs(absl::StatusCode::kInvalidArgument));

  std::string query2 = R"sql(
      SELECT SNIPPET(Summary, 'top', max_snippet_width=>-1)
      FROM albums
      WHERE userid = 1)sql";
  EXPECT_THAT(Query(GetSqlQueryString(query2)),
              StatusIs(absl::StatusCode::kInvalidArgument));
}

TEST_P(SearchTest, SnippetNamedArgumentShuffledOrder) {
  std::string query = R"sql(
      SELECT SNIPPET(language_tag=>'',
                     query=>'foo',
                     max_snippet_width=>10,
                     value=>Summary,
                     enhance_query=>false,
                     max_snippets=>4)
      FROM albums
      WHERE userid = 1)sql";
  GOOGLESQL_EXPECT_OK(Query(GetSqlQueryString(query)));
}

TEST_P(SearchTest, WrongSnippetContentType) {
  std::string query = R"sql(
      SELECT SNIPPET(Summary,
                     'foo',
                     enhance_query=>false,
                     language_tag=>'',
                     max_snippet_width=>10,
                     max_snippets=>4,
                     content_type=>'unsupported')
      FROM albums
      WHERE userid = 1)sql";
  EXPECT_THAT(
      Query(GetSqlQueryString(query)),
      StatusIs(
          absl::StatusCode::kInvalidArgument,
          HasSubstr(
              "Invalid use of SNIPPET: content_type. \'unsupported\' is not a "
              "supported content type.")));
}

TEST_P(SearchTest, SnippetContentTypeCaseInsensitive) {
  std::string query = R"sql(
      SELECT SNIPPET(Summary,
                     'foo',
                     enhance_query=>false,
                     language_tag=>'',
                     max_snippet_width=>10,
                     max_snippets=>4,
                     content_type=>'text/HTML')
      FROM albums
      WHERE userid = 1)sql";
  GOOGLESQL_EXPECT_OK(Query(GetSqlQueryString(query)));
}

// JSON search does not use the underlined tokenlist, instead it relies on the
// JSON_CONTAINS or JSONB_CONTAINS function to do the search.
// The required tokenize_json or tokenize_jsonb function will be added in the
// schema file to ensure JSON search schema can work.
TEST_P(SearchTest, JsonSearch) {
  std::string query = dialect_ == database_api::DatabaseDialect::POSTGRESQL
                          ?
                          R"sql(
      SELECT albumid
      FROM albums
      WHERE userid = 1 AND jsonb @> '1')sql"
                          :
                          R"sql(
      SELECT albumid
      FROM albums
      WHERE userid = 1 AND JSON_CONTAINS(json, JSON '1'))sql";

  GOOGLESQL_EXPECT_OK(Query(GetSqlQueryString(query)));
}

TEST_P(SearchTest, SearchWithExistingSearchIndexHint) {
  GOOGLESQL_EXPECT_OK(UpdateSchema({
      R"sql(
      CREATE SEARCH INDEX summary_idx
      ON albums(summary_tokens, summary_substr_tokens))sql"}));
  std::string query = R"sql(
      SELECT albumid
      FROM albums@{force_index=summary_idx}
      WHERE SEARCH_SUBSTRING(summary_substr_tokens, 'son')
        AND userid = 1
      ORDER BY albumid ASC)sql";
  EXPECT_THAT(Query(GetSqlQueryString(query)),
              IsOkAndHoldsRows({{1}, {2}, {4}, {6}, {7}}));
}

TEST_P(SearchTest, SearchWithComplicatedSearchIndexHint) {
  auto index_schema = dialect_ == database_api::DatabaseDialect::POSTGRESQL
                          ?
                          R"sql(
        CREATE SEARCH INDEX summary_idx
        ON albums(summary_tokens, summary_substr_tokens)
        INCLUDE(length)
        PARTITION BY userid
        ORDER BY releasetimestamp
        WITH (
          sort_order_sharding = true,
          disable_automatic_uid_column=true
        )
      )sql"
                          :
                          R"sql(
        CREATE SEARCH INDEX summary_idx
        ON albums(summary_tokens, summary_substr_tokens)
        STORING(length)
        PARTITION BY userid
        ORDER BY releasetimestamp
        OPTIONS (sort_order_sharding = true, disable_automatic_uid_column=true)
      )sql";
  GOOGLESQL_EXPECT_OK(UpdateSchema({index_schema}));
  std::string query = R"sql(
      SELECT albumid
      FROM albums@{force_index=summary_idx}
      WHERE SEARCH_SUBSTRING(summary_substr_tokens, 'loba')
        AND userid = 1
      ORDER BY albumid ASC)sql";
  EXPECT_THAT(Query(GetSqlQueryString(query)), IsOkAndHoldsRows({{1}, {2}}));
}

TEST_P(SearchTest, TokenlistConcatInSearch) {
  std::string query1 = R"sql(
          SELECT albumid
          FROM albums@{force_index=albumindex}
          WHERE SEARCH(TOKENLIST_CONCAT(ARRAY[summary_tokens, summary2_tokens]), "global foo")
            AND userid = 1
          ORDER BY albumid ASC)sql";
  EXPECT_THAT(Query(GetSqlQueryString(query1)), IsOkAndHoldsRows({{1}}));

  std::string query2 = R"sql(
          SELECT albumid
          FROM albums@{force_index=albumindex}
          WHERE SEARCH_SUBSTRING(TOKENLIST_CONCAT(ARRAY[summary_substr_tokens, summary2_substr_tokens]), 'son')
            AND userid = 1
          ORDER BY albumid ASC)sql";
  EXPECT_THAT(Query(GetSqlQueryString(query2)),
              IsOkAndHoldsRows({{1}, {2}, {4}, {6}, {7}}));
}

// The emulator's query_validator does not have the table name when fail the
// hint value check. Thus just try to match a portion of the error message.
TEST_P(SearchTest, SearchWithNonExistingSearchIndexFails) {
  std::string query = R"sql(
      SELECT albumid
      FROM albums@{force_index=summary_idx}
      WHERE SEARCH_SUBSTRING(summary_substr_tokens, 'son')
        AND userid = 1
      ORDER BY albumid ASC)sql";
  EXPECT_THAT(Query(GetSqlQueryString(query)),
              StatusIs(absl::StatusCode::kInvalidArgument,
                       HasSubstr("index called summary_idx")));
}

// The emulator's query_validator does not have the table name when fail the
// hint value check. Thus just try to match a portion of the error message.
TEST_P(SearchTest, SearchFailAfterDropSearchIndex) {
  std::string create_index_query = R"sql(
      CREATE SEARCH INDEX summary_idx
      ON albums(summary_tokens, summary_substr_tokens))sql";
  GOOGLESQL_EXPECT_OK(UpdateSchema({create_index_query}));
  std::string search_query = R"sql(
      SELECT albumid
      FROM albums@{force_index=summary_idx}
      WHERE SEARCH_SUBSTRING(summary_substr_tokens, 'son')
        AND userid = 1
      ORDER BY albumid ASC)sql";
  EXPECT_THAT(Query(GetSqlQueryString(search_query)),
              IsOkAndHoldsRows({{1}, {2}, {4}, {6}, {7}}));

  std::string drop_index_query = R"sql(
      DROP SEARCH INDEX summary_idx)sql";
  GOOGLESQL_EXPECT_OK(UpdateSchema({drop_index_query}));
  EXPECT_THAT(Query(GetSqlQueryString(search_query)),
              StatusIs(absl::StatusCode::kInvalidArgument,
                       HasSubstr("index called summary_idx")));
}

TEST_P(SearchTest, AlterSearchIndexColumns) {
  GOOGLESQL_ASSERT_OK(UpdateSchema({R"sql(
      CREATE SEARCH INDEX summary_idx ON albums(summary_tokens))sql"}));
  GOOGLESQL_ASSERT_OK(UpdateSchema(
      {"ALTER SEARCH INDEX summary_idx ADD COLUMN summary_substr_tokens",
       IsGoogleStandardSql()
           ? "ALTER SEARCH INDEX summary_idx ADD STORED COLUMN length"
           : "ALTER SEARCH INDEX summary_idx ADD INCLUDE COLUMN length"}));
  if (IsGoogleStandardSql()) {
    EXPECT_THAT(GetDatabaseDdl(),
                IsOkAndHolds(testing::Contains(
                    "CREATE SEARCH INDEX summary_idx ON albums(summary_tokens, "
                    "summary_substr_tokens) STORING (length)")));
  }
  std::string query = R"sql(
      SELECT albumid
      FROM albums@{force_index=summary_idx}
      WHERE SEARCH_SUBSTRING(summary_substr_tokens, 'son')
        AND userid = 1
      ORDER BY albumid ASC)sql";
  EXPECT_THAT(Query(GetSqlQueryString(query)),
              IsOkAndHoldsRows({{1}, {2}, {4}, {6}, {7}}));

  GOOGLESQL_ASSERT_OK(UpdateSchema(
      {"ALTER SEARCH INDEX summary_idx DROP COLUMN summary_tokens",
       IsGoogleStandardSql()
           ? "ALTER SEARCH INDEX summary_idx DROP STORED COLUMN length"
           : "ALTER SEARCH INDEX summary_idx DROP INCLUDE COLUMN length"}));
  if (IsGoogleStandardSql()) {
    EXPECT_THAT(GetDatabaseDdl(),
                IsOkAndHolds(testing::Contains(
                    "CREATE SEARCH INDEX summary_idx ON "
                    "albums(summary_substr_tokens)")));
  }
  EXPECT_THAT(Query(GetSqlQueryString(query)),
              IsOkAndHoldsRows({{1}, {2}, {4}, {6}, {7}}));

  EXPECT_THAT(
      UpdateSchema({"ALTER SEARCH INDEX summary_idx DROP COLUMN "
                    "summary_substr_tokens"}),
      StatusIs(absl::StatusCode::kFailedPrecondition,
               HasSubstr("Cannot drop the last TOKENLIST column")));
  EXPECT_THAT(UpdateSchema({"ALTER SEARCH INDEX missing_idx ADD COLUMN "
                            "summary_tokens"}),
              StatusIs(absl::StatusCode::kNotFound));
}

TEST_P(SearchTest, ProjectTokenlistFailColRef) {
  std::string query = R"sql(
      SELECT albumid, length_tokens
      FROM albums)sql";
  EXPECT_THAT(Query(GetSqlQueryString(query)),
              StatusIs(absl::StatusCode::kInvalidArgument));
}

TEST_P(SearchTest, ProjectTokenlistFailFunctionCall) {
  std::string query = R"sql(
      SELECT TOKEN("name1") AS col1
      FROM albums)sql";
  EXPECT_THAT(Query(GetSqlQueryString(query)),
              StatusIs(absl::StatusCode::kInvalidArgument));
}

TEST_P(SearchTest, TokenFunctionFailsWithNamedArgumentInDDL) {
  std::string ddl = IsGoogleStandardSql() ? R"sql(
              CREATE TABLE BadTokenTable (
                ID INT64 NOT NULL, Data STRING(MAX),
                Tokens TOKENLIST AS (TOKEN(value=>Data)) HIDDEN
              ) PRIMARY KEY(ID))sql"
                                          : R"sql(
              CREATE TABLE BadTokenTable (
                id bigint NOT NULL, data varchar,
                tokens spanner.tokenlist GENERATED ALWAYS AS (spanner.token(value=>data)) STORED HIDDEN
              ) PRIMARY KEY(id))sql";
  EXPECT_THAT(UpdateSchema({ddl}),
              StatusIs(absl::StatusCode::kInvalidArgument));
}

TEST_P(SearchTest, ProjectAll) {
  std::string query = R"sql(
      SELECT *
      FROM albums
      WHERE albumid = 1)sql";
  GOOGLESQL_EXPECT_OK(Query(GetSqlQueryString(query)));
}

TEST_P(SearchTest, ArrayIncludesSupported) {
  if (!IsGoogleStandardSql()) {
    GTEST_SKIP() << "ARRAY_INCLUDES functions are GoogleSQL only.";
  }
  std::string query1 = R"sql(
    SELECT a.albumid
    FROM albums@{force_index=albumindex} a
    WHERE ARRAY_INCLUDES(a.Tracks, "track2")
      AND a.userid = 1
    ORDER BY a.albumid
  )sql";
  EXPECT_THAT(Query(GetSqlQueryString(query1)),
              IsOkAndHoldsRows({{2}, {4}, {6}, {9}}));

  std::string query2 = R"sql(
    SELECT a.albumid
    FROM albums@{force_index=albumindex} a
    WHERE ARRAY_INCLUDES_ANY(a.Tracks, ["track1", "track2"])
      AND a.userid = 1
    ORDER BY a.albumid
  )sql";
  EXPECT_THAT(Query(GetSqlQueryString(query2)),
              IsOkAndHoldsRows({{2}, {4}, {6}, {9}, {12}}));

  std::string query3 = R"sql(
    SELECT a.albumid
    FROM albums@{force_index=albumindex} a
    WHERE ARRAY_INCLUDES_ALL(a.Tracks, ["track1", "track2"])
      AND a.userid = 1
    ORDER BY a.albumid
  )sql";
  EXPECT_THAT(Query(GetSqlQueryString(query3)),
              IsOkAndHoldsRows({{2}, {4}, {6}, {9}}));
}

TEST_P(SearchTest, ArrayIncludesNullOrEmpty) {
  if (!IsGoogleStandardSql()) {
    GTEST_SKIP() << "ARRAY_INCLUDES functions are GoogleSQL only.";
  }
  std::string query1 = R"sql(
    SELECT a.albumid
    FROM albums@{force_index=albumindex} a
    WHERE ARRAY_INCLUDES(a.Tracks, NULL)
      AND a.userid = 1
    ORDER BY a.albumid
  )sql";
  EXPECT_THAT(Query(GetSqlQueryString(query1)), IsOkAndHoldsRows({}));

  std::string query2 = R"sql(
    SELECT a.albumid
    FROM albums@{force_index=albumindex} a
    WHERE ARRAY_INCLUDES_ANY(a.Tracks, NULL)
      AND a.userid = 1
    ORDER BY a.albumid
  )sql";
  EXPECT_THAT(Query(GetSqlQueryString(query2)), IsOkAndHoldsRows({}));

  std::string query3 = R"sql(
    SELECT a.albumid
    FROM albums@{force_index=albumindex} a
    WHERE ARRAY_INCLUDES_ANY(a.Tracks, [])
      AND a.userid = 1
    ORDER BY a.albumid
  )sql";
  EXPECT_THAT(Query(GetSqlQueryString(query3)), IsOkAndHoldsRows({}));
}

static constexpr char kSearchQueryFormat[] = R"sql(
      SELECT a.Name
      FROM SqlSearchCountries$0 a
      WHERE $1)sql";

static constexpr char kSearchIndexHint[] =
    "@{force_index=SqlSearchCountriesIndex}";

static constexpr char kSearchPredicate[] = "SEARCH(a.name_tokens, 'foo')";
static constexpr char kNonSearchPrediate[] = "CountryId = 1";

static constexpr char kAllowSearchInTransactionHint[] =
    R"sql(@{allow_search_indexes_in_transaction=true})sql";

struct SearchInTransactionQuery {
  bool has_index_hint = false;
  bool has_search = false;
  bool read_only = false;
  bool with_allow_search_in_transaction_hint = false;
  bool expect_error = false;
};

class SearchInTransactionTest
    : public DatabaseTest,
      public testing::WithParamInterface<SearchInTransactionQuery> {
 public:
  static std::vector<SearchInTransactionQuery> GetTransactionTests() {
    return {
        {.has_index_hint = false, .has_search = true, .read_only = true},
        {.has_index_hint = true, .has_search = true, .read_only = true},
        {.has_index_hint = false, .has_search = true, .expect_error = true},
        {.has_index_hint = false,
         .has_search = true,
         .with_allow_search_in_transaction_hint = true},
        {.has_index_hint = true, .has_search = true, .expect_error = true},
        {.has_index_hint = true,
         .has_search = true,
         .with_allow_search_in_transaction_hint = true},
        {.has_index_hint = true, .has_search = false, .read_only = true},
        {.has_index_hint = true, .has_search = false, .expect_error = true},
        {.has_index_hint = true,
         .has_search = false,
         .with_allow_search_in_transaction_hint = true},
    };
  }

  absl::Status SetUpDatabase() override {
    GOOGLESQL_RETURN_IF_ERROR(SetSchema({
        R"sql(
        CREATE TABLE SqlSearchCountries (
          CountryId INT64 NOT NULL,
          Name STRING(MAX),
          Population INT64,
          name_tokens TOKENLIST AS (TOKENIZE_FULLTEXT(Name)) STORED HIDDEN,
          Name_Substring_Tokens TOKENLIST AS (TOKENIZE_SUBSTRING(Name)) STORED HIDDEN,
          Population_Tokens TOKENLIST AS (TOKENIZE_NUMBER(Population)) STORED HIDDEN,
          ) PRIMARY KEY(CountryId)
        )sql"}));
    return SetSchema({
        R"sql(
          CREATE SEARCH INDEX SqlSearchCountriesIndex ON
          SqlSearchCountries(name_tokens,
                             Name_Substring_Tokens,
                             Population_Tokens)
          OPTIONS (sort_order_sharding = true)
        )sql",
    });
  }
};

TEST_P(SearchInTransactionTest, SearchInTransactionalQuery) {
  const SearchInTransactionQuery& test = GetParam();
  auto txn = test.read_only ? Transaction(Transaction::ReadOnlyOptions())
                            : Transaction(Transaction::ReadWriteOptions());
  std::string query = absl::Substitute(
      kSearchQueryFormat, test.has_index_hint ? kSearchIndexHint : "",
      test.has_search ? kSearchPredicate : kNonSearchPrediate);
  query = test.with_allow_search_in_transaction_hint
              ? absl::StrCat(kAllowSearchInTransactionHint, query)
              : query;
  auto query_result = QueryTransaction(txn, query);
  if (test.expect_error) {
    EXPECT_THAT(query_result, StatusIs(absl::StatusCode::kInvalidArgument));
  } else {
    GOOGLESQL_EXPECT_OK(query_result);
  }
}

INSTANTIATE_TEST_SUITE_P(
    SearchInTransactionalQuery, SearchInTransactionTest,
    testing::ValuesIn(SearchInTransactionTest::GetTransactionTests()));

TEST_P(SearchInTransactionTest, SearchInBatchQuery) {
  const SearchInTransactionQuery& test = GetParam();
  Transaction txn{Transaction::ReadOnlyOptions{}};
  std::string query = absl::Substitute(
      kSearchQueryFormat, test.has_index_hint ? kSearchIndexHint : "",
      test.has_search ? kSearchPredicate : kNonSearchPrediate);
  query = test.with_allow_search_in_transaction_hint
              ? absl::StrCat(kAllowSearchInTransactionHint, query)
              : query;
  auto query_result = PartitionQuery(txn, query);
  EXPECT_THAT(query_result, StatusIs(absl::StatusCode::kInvalidArgument));
}

INSTANTIATE_TEST_SUITE_P(
    SearchInBatchQuery, SearchInTransactionTest,
    testing::ValuesIn(SearchInTransactionTest::GetTransactionTests()));

class SearchInTransactionWithForUpdateTest : public SearchInTransactionTest {
 public:
  static std::vector<SearchInTransactionQuery> GetTransactionTests() {
    return {
        {.has_index_hint = false,
         .has_search = true,
         .with_allow_search_in_transaction_hint = true},
        {.has_index_hint = true,
         .has_search = true,
         .with_allow_search_in_transaction_hint = true},
        {.has_index_hint = true,
         .has_search = false,
         .with_allow_search_in_transaction_hint = true},
    };
  }
};

TEST_P(SearchInTransactionWithForUpdateTest, SearchInTransactionalQuery) {
  Transaction txn{Transaction::ReadWriteOptions{}};

  std::string query =
      absl::Substitute(absl::StrCat(kAllowSearchInTransactionHint,
                                    kSearchQueryFormat, " FOR UPDATE"),
                       kSearchIndexHint, kSearchPredicate);
  auto query_result = QueryTransaction(txn, query);
  EXPECT_THAT(
      query_result,
      StatusIs(absl::StatusCode::kInvalidArgument,
               HasSubstr("FOR UPDATE is not supported in search queries")));
}

INSTANTIATE_TEST_SUITE_P(
    SearchInTransactionalQuery, SearchInTransactionWithForUpdateTest,
    testing::ValuesIn(
        SearchInTransactionWithForUpdateTest::GetTransactionTests()));

static constexpr char kQueryWithoutSearch[] = R"sql(
      SELECT a.Name
      FROM SqlSearchCountries a
      WHERE name = 'foo')sql";

TEST_P(SearchInTransactionTest, NonSearchQuerySucceedInTransaction) {
  auto txn = Transaction(Transaction::ReadWriteOptions());
  GOOGLESQL_EXPECT_OK(QueryTransaction(txn, kQueryWithoutSearch));

  txn = Transaction(Transaction::ReadOnlyOptions());
  GOOGLESQL_EXPECT_OK(PartitionQuery(txn, kQueryWithoutSearch));
}

TEST_P(SearchInTransactionTest, SearchNotSupportedInPartitionedDML) {
  EXPECT_THAT(ExecutePartitionedDml(
                  SqlStatement("UPDATE SqlSearchCountries SET Name = NULL "
                               "WHERE SEARCH_SUBSTRING(name_tokens, 'foo')")),
              StatusIs(absl::StatusCode::kInvalidArgument));
}

TEST_P(SearchInTransactionTest, SearchNotSupportedInBatchDML) {
  EXPECT_THAT(CommitBatchDml(
                  {SqlStatement("UPDATE SqlSearchCountries SET Name = NULL "
                                "WHERE SEARCH_SUBSTRING(name_tokens, 'foo')")}),
              StatusIs(absl::StatusCode::kInvalidArgument));
}

struct TokenizeNumberParametersTestCase {
  std::string query;
  Matcher<std::string> expected_error;
};

class TokenizeNumberParametersTest
    : public DatabaseTest,
      public testing::WithParamInterface<TokenizeNumberParametersTestCase> {
 public:
  absl::Status SetUpDatabase() override { return absl::OkStatus(); }

  static std::vector<TokenizeNumberParametersTestCase>
  GetTokenizeNumberErrorTests() {
    return {
        {.query = "SELECT TOKENIZE_NUMBER(999, comparison_type=>'compare')",
         .expected_error =
             AnyOf(HasSubstr("TOKENIZE_NUMBER: unsupported comparison type: "
                             "'compare'"),
                   HasSubstr("TOKENIZE_NUMBER: Unsupported comparison type: "
                             "'compare'"))},
        {.query = "SELECT TOKENIZE_NUMBER(999, algorithm=>'linear')",
         .expected_error = AnyOf(
             ContainsRegex(
                 "TOKENIZE_NUMBER: Algorithm is not supported: 'linear'; "
                 "supported algorithms are: 'auto', 'logtree', 'prefixtree', "
                 "'floatingpoint' \\(floatingpoint is supported only for "
                 "floating point types\\)"),
             ContainsRegex(
                 "TOKENIZE_NUMBER: unsupported algorithm 'linear'; "
                 "supported algorithms are 'auto', 'logtree', 'prefixtree', or "
                 "'floatingpoint' \\(floatingpoint is supported only for "
                 "floating point types\\)"))},
        {.query = "SELECT TOKENIZE_NUMBER(999, tree_base=>11)",
         .expected_error =
             HasSubstr("TOKENIZE_NUMBER: tree_base must be in the range "
                       "[2, 10], got: 11")},
        {.query = "SELECT TOKENIZE_NUMBER(2e255, algorithm=>'logtree', "
                  "min=>CAST('inf' AS FLOAT64))",
         .expected_error =
             HasSubstr("TOKENIZE_NUMBER: min must be finite, got: inf")},
        {.query = "SELECT TOKENIZE_NUMBER(2e255, algorithm=>'logtree', "
                  "max=>CAST('nan' AS FLOAT64))",
         .expected_error =
             HasSubstr("TOKENIZE_NUMBER: max must be finite, got: nan")},
        {.query = "SELECT TOKENIZE_NUMBER(2e255, algorithm=>'logtree', "
                  "granularity=>CAST('inf' AS FLOAT64))",
         .expected_error =
             HasSubstr("TOKENIZE_NUMBER: granularity must be finite and "
                       "positive, got: inf")},
        {.query = "SELECT TOKENIZE_NUMBER(2, min=>5, max=>-1)",
         .expected_error = HasSubstr(
             "TOKENIZE_NUMBER: min must be less than max, got: 5 and -1")},
        {.query = "SELECT TOKENIZE_NUMBER(5, min=>1, max=>10, granularity=>11)",
         .expected_error = AnyOf(
             HasSubstr(
                 "TOKENIZE_NUMBER: granularity (11) must be less than or equal "
                 "to the difference (9) between min and max (1, 10)"),
             HasSubstr(
                 "TOKENIZE_NUMBER: granularity (11) must be less than or equal "
                 "to the difference between min and max (1, 10)"))},
        {.query = "SELECT TOKENIZE_NUMBER(2.567, min=>-1.0, max=>5.0, "
                  "algorithm=>'floatingpoint', precision=>16)",
         .expected_error = ContainsRegex("TOKENIZE_NUMBER: precision must be "
                                         "in the range \\[1, 15\\], got: 16")},
        {.query = "SELECT TOKENIZE_NUMBER(2.567, min=>-1.0, max=>5.0, "
                  "algorithm=>'floatingpoint', ieee_precision=>16)",
         .expected_error = ContainsRegex("TOKENIZE_NUMBER: precision must be "
                                         "in the range \\[1, 15\\], got: 16")},
        {.query = "SELECT TOKENIZE_NUMBER(2, min=>-1, max=>5, "
                  "algorithm=>'floatingpoint');",
         .expected_error =
             ContainsRegex("TOKENIZE_NUMBER: algorithm 'floatingpoint' can "
                           "only be used to tokenize floating point values")},
        {.query = "SELECT TOKENIZE_NUMBER(0, algorithm=>'logtree', "
                  "precision=>-1)",
         .expected_error = ContainsRegex("TOKENIZE_NUMBER: precision must be "
                                         "in the range \\[1, 15\\], got: -1")}};
  }
};

TEST_P(TokenizeNumberParametersTest, TokenizeNumberErrorTest) {
  const TokenizeNumberParametersTestCase& test = GetParam();
  EXPECT_THAT(Query(test.query), StatusIs(absl::StatusCode::kInvalidArgument,
                                          test.expected_error));
}

INSTANTIATE_TEST_SUITE_P(
    TokenizeNumberErrorTest, TokenizeNumberParametersTest,
    testing::ValuesIn(
        TokenizeNumberParametersTest::GetTokenizeNumberErrorTests()));
}  // namespace

}  // namespace test
}  // namespace emulator
}  // namespace spanner
}  // namespace google

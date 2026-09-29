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

#ifndef THIRD_PARTY_CLOUD_SPANNER_EMULATOR_BACKEND_QUERY_SEARCH_TOKENIZER_H_
#define THIRD_PARTY_CLOUD_SPANNER_EMULATOR_BACKEND_QUERY_SEARCH_TOKENIZER_H_

#include <cstdint>
#include <string>
#include <vector>

#include "googlesql/public/value.h"
#include "absl/status/status.h"
#include "absl/status/statusor.h"
#include "absl/strings/string_view.h"
#include "absl/types/span.h"
#include "unicode/uscript.h"

namespace google {
namespace spanner {
namespace emulator {
namespace backend {
namespace query {
namespace search {

static constexpr absl::string_view kExactMatchTokenizer = "exact_match";
static constexpr absl::string_view kFullTextTokenizer = "fulltext";
static constexpr absl::string_view kSubstringTokenizer = "substring";
static constexpr absl::string_view kNumericTokenizer = "numeric";
static constexpr absl::string_view kBoolTokenizer = "bool";
static constexpr absl::string_view kNgramsTokenizer = "ngrams";
static constexpr absl::string_view kJsonTokenizer = "json";
static constexpr absl::string_view kJsonbTokenizer = "jsonb";

static constexpr int kSubstringTokenizerSignatureArgumentSize = 5;

static constexpr char kDelimiter[] =
    "~!@#$%^&*()_+-={}[]|\\/\"`:;'<>,.?\r\n\t\b\f ";

// Index attribute of the tokens at the start and at the end of a tokenized
// value. DEBUG_TOKENLIST shows it as "(boundary)".
static constexpr uint64_t kBoundaryIndexAttribute = 1;

// String indicate a gap between tokens. This is used to indicate tokens on both
// sides were created from different elements of an array.
static constexpr char kGapString[] = "\x01";

googlesql::Value TokenListFromStrings(std::vector<std::string> strings);

absl::StatusOr<std::vector<std::string>> StringsFromTokenList(
    const googlesql::Value& tokenList);

// Returns the tokens in `tokenlist` as a comma-separated list for
// DEBUG_TOKENLIST, in the documented format: tokens at the same position are
// listed in brackets, and boundary tokens are followed by "(boundary)", as in
// `hello(boundary), db, [#world, world](boundary)`. Tokenizer signatures and
// array gaps are emulator internals and are left out.
absl::StatusOr<std::string> DebugTokenList(const googlesql::Value& tokenlist);

googlesql::Value TokenListFromBytes(std::string& bytes);

// Relative search types supported by TOKENIZE_SUBSTRING and SEARCH_SUBSTRING.
enum RelativeSearchType {
  None = 0,
  Word_Prefix = 0x01,
  Word_Suffix = 0x02,
  Value_Prefix = 0x04,
  Value_Suffix = 0x08,
  Phrase = 0x10,

  All = Word_Prefix | Word_Suffix | Value_Prefix | Value_Suffix | Phrase,
};

absl::StatusOr<RelativeSearchType> ParseRelativeSearchType(
    absl::string_view relative_search_type);

// Returns true if `token` is a tokenizer signature rather than normal token.
bool IsTokenizerSignature(absl::string_view token);

int64_t GetIntParameterValue(absl::Span<const googlesql::Value> args,
                             int parameter_index, int default_value);

double GetDoubleParameterValue(absl::Span<const googlesql::Value> args,
                               int parameter_index, double default_value);

bool GetBoolParameterValue(absl::Span<const googlesql::Value> args,
                           int parameter_index, bool default_value);

absl::Status TokenizeSubstring(absl::string_view str,
                               std::vector<std::string>& token_list);

// Lowercases text using the requested language and optionally removes Unicode
// combining marks. Used for both indexed values and search queries.
absl::StatusOr<std::string> NormalizeSearchText(
    absl::string_view str, bool remove_diacritics = false,
    absl::string_view language_tag = "", bool lowercase = true);

// Returns whether text changes from the `previous` to the `current` script
// and one of them is an East Asian script (Han, Hiragana, Katakana or Hangul).
// Words are split there, as in "東京タワー", because the emulator's ICU data
// has no dictionaries for segmenting such text.
bool IsEastAsianScriptChange(UScriptCode previous, UScriptCode current);

// Splits `str` into words using the word boundaries of `language_tag`, and
// where East Asian text changes scripts. When
// `hashtags` is set, it receives for every word whether it directly follows a
// '#' that starts a hashtag.
absl::Status TokenizeWords(absl::string_view str,
                           absl::string_view language_tag,
                           std::vector<std::string>& token_list,
                           std::vector<bool>* hashtags = nullptr);

struct HtmlTextSegment {
  std::string text;
  int category;  // small=0, medium=1, large=2, title=3.
};

// Returns the text of `html` with its prominence. Tags and comments are
// removed, the contents of <script> and <style> elements are skipped, and
// character references (numeric, and the named references of HTML 4 plus
// &apos;) are decoded.
std::vector<HtmlTextSegment> ExtractHtmlText(absl::string_view html);

// Returns whether the tokenizer signature `signature` has the `flag` option,
// such as "d" for remove_diacritics.
bool SignatureHasFlag(absl::string_view signature, absl::string_view flag);

absl::StatusOr<bool> TokenListRemovesDiacritics(
    const googlesql::Value& tokenlist);

absl::Status TokenizeNgrams(absl::string_view str, int ngram_size_min,
                            int ngram_size_max,
                            std::vector<std::string>& token_list);
}  // namespace search
}  // namespace query
}  // namespace backend
}  // namespace emulator
}  // namespace spanner
}  // namespace google

#endif  // THIRD_PARTY_CLOUD_SPANNER_EMULATOR_BACKEND_QUERY_SEARCH_TOKENIZER_H_

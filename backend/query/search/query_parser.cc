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

#include "backend/query/search/query_parser.h"

#include <cstdint>
#include <memory>
#include <string>
#include <vector>

#include "googlesql/public/functions/string.h"
#include "absl/log/check.h"
#include "absl/memory/memory.h"
#include "absl/status/status.h"
#include "absl/strings/ascii.h"
#include "absl/strings/numbers.h"
#include "absl/strings/str_cat.h"
#include "absl/strings/str_join.h"
#include "absl/strings/string_view.h"
#include "unicode/brkiter.h"
#include "unicode/locid.h"
#include "unicode/uchar.h"
#include "unicode/unistr.h"
#include "unicode/uscript.h"
#include "backend/query/search/tokenizer.h"
#include "backend/query/search/ErrorHandler.h"
#include "backend/query/search/JavaCC.h"
#include "backend/query/search/SearchQueryParser.h"
#include "backend/query/search/SearchQueryParserTokenManager.h"
#include "backend/query/search/SearchQueryParserTreeConstants.h"
#include "backend/query/search/Token.h"
#include "backend/query/search/query_char_stream.h"
#include "common/errors.h"
#include "googlesql/base/status_macros.h"

namespace google {
namespace spanner {
namespace emulator {
namespace backend {
namespace query {
namespace search {

namespace {
constexpr absl::string_view kSeparators = "~`!@#$%^&_+{}[]<>,?* \r\n\t\b\f_;";

// The generated lexer only reads ASCII. Every other letter, digit or mark is
// spelled as kEncodedCharacter and six hex digits of its code point, which the
// grammar lexes as part of a term, and NormalizeParsedTree decodes it. Other
// non-ASCII characters separate terms, as do word boundaries next to non-ASCII
// characters, so that "東京タワー" searches for the words "東京" and "タワー".
constexpr char kEncodedCharacter = '\x01';
constexpr int kEncodedCharacterDigits = 6;

UScriptCode GetScript(UChar32 c) {
  UErrorCode error = U_ZERO_ERROR;
  const UScriptCode script = uscript_getScript(c, &error);
  return U_FAILURE(error) ? USCRIPT_COMMON : script;
}

std::string EncodeNonAsciiCharacters(absl::string_view query) {
  icu::UnicodeString text =
      icu::UnicodeString::fromUTF8(icu::StringPiece(query.data(), query.size()));
  UErrorCode error = U_ZERO_ERROR;
  std::unique_ptr<icu::BreakIterator> words(
      icu::BreakIterator::createWordInstance(icu::Locale::getRoot(), error));
  if (U_FAILURE(error)) words = nullptr;
  if (words != nullptr) words->setText(text);
  std::string encoded;
  bool previous_is_word = false;
  bool previous_is_ascii = true;
  // The script of the word being encoded, ignoring common characters.
  UScriptCode word_script = USCRIPT_COMMON;
  for (int32_t i = 0; i < text.length(); i = text.moveIndex32(i, 1)) {
    // A literal kEncodedCharacter separates terms.
    const UChar32 c = text.char32At(i) == kEncodedCharacter
                          ? static_cast<UChar32>(' ')
                          : text.char32At(i);
    const bool ascii = c < 0x80;
    const bool word = ascii ? absl::ascii_isalnum(c)
                            : u_isalnum(c) ||
                                  (U_GET_GC_MASK(c) & U_GC_M_MASK) != 0;
    const UScriptCode script = GetScript(c);
    if (word && previous_is_word && (!ascii || !previous_is_ascii) &&
        ((words != nullptr && words->isBoundary(i)) ||
         IsEastAsianScriptChange(word_script, script))) {
      encoded.push_back(' ');
      word_script = USCRIPT_COMMON;
    }
    if (!word) {
      word_script = USCRIPT_COMMON;
    } else if (script != USCRIPT_COMMON && script != USCRIPT_INHERITED) {
      word_script = script;
    }
    if (ascii) {
      encoded.push_back(static_cast<char>(c));
    } else if (word) {
      absl::StrAppend(&encoded, absl::string_view(&kEncodedCharacter, 1),
                      absl::Hex(c, absl::kZeroPad6));
    } else {
      encoded.push_back(' ');
    }
    previous_is_word = word;
    previous_is_ascii = ascii;
  }
  return encoded;
}

std::string DecodeNonAsciiCharacters(absl::string_view term) {
  std::string decoded;
  for (size_t i = 0; i < term.size(); ++i) {
    uint32_t code = 0;
    if (term[i] == kEncodedCharacter &&
        absl::SimpleHexAtoi(term.substr(i + 1, kEncodedCharacterDigits),
                            &code)) {
      icu::UnicodeString(static_cast<UChar32>(code)).toUTF8String(decoded);
      i += kEncodedCharacterDigits;
    } else {
      decoded.push_back(term[i]);
    }
  }
  return decoded;
}
}  // namespace

namespace {

// Borrowed from backend/schema/parser/query_parser.cc DDLErrorHandler.
// The class is used by query parser to record the parsing errors.
// The errors then can be populated to caller for future process.
class SearchQueryParserErrorHandler : public ErrorHandler {
 public:
  explicit SearchQueryParserErrorHandler(std::vector<std::string>* errors)
      : errors_(errors), ignore_further_errors_(false) {}
  ~SearchQueryParserErrorHandler() override = default;

  void handleUnexpectedToken(int expected_kind, const JJString& expected_token,
                             Token* actual,
                             SearchQueryParser* parser) override {
    if (ignore_further_errors_) {
      return;
    }
    // expected_kind is -1 when the next token is not expected, when choosing
    // the next rule based on next token. Every invocation of
    // handleUnexpectedToken with expeced_kind=-1 is followed by a call to
    // handleParserError. We process the error there.
    if (expected_kind == -1) {
      return;
    }

    // The parser would continue to throw unexpected token at us but only the
    // first error is the cause.
    ignore_further_errors_ = true;

    errors_->push_back(absl::StrCat("Syntax error on column ",
                                    actual->beginColumn, ": Expecting '",
                                    absl::AsciiStrToUpper(expected_token)));
  }

  void handleParseError(Token* last, Token* unexpected,
                        const JJSimpleString& production,
                        SearchQueryParser* parser) override {
    if (ignore_further_errors_) {
      return;
    }
    ignore_further_errors_ = true;

    std::string extra_info;
    if (unexpected->kind == OPEN_PARE || unexpected->kind == CLOSE_PARE) {
      extra_info =
          "Using parentheses to group query terms is not supported in rquery "
          "parser.";
    }

    errors_->push_back(
        absl::StrCat("Encountered error on column ", unexpected->beginColumn,
                     " while parsing: ", production, ". ", extra_info));
  }

  int getErrorCount() override { return errors_->size(); }

 private:
  // List of errors found during the parse.  Will be empty IFF
  // there were no problems parsing.
  std::vector<std::string>* errors_;
  bool ignore_further_errors_ = false;
};

}  // namespace

RQueryParser::RQueryParser(absl::string_view query)
    : query_(query), tree_(nullptr) {}

absl::Status RQueryParser::NormalizeParsedTree(SimpleNode* tree) {
  if (tree == nullptr) {
    return absl::OkStatus();
  }

  if (tree->getId() == JJTTERM) {
    std::string normalized_str;
    absl::Status status;

    googlesql::functions::LowerUtf8(DecodeNonAsciiCharacters(tree->image()),
                                    &normalized_str, &status);
    if (!status.ok()) {
      return error::FailToParseSearchQuery(
          query_, "Failed to normalize search query tree.");
    }

    tree->set_image(normalized_str);
  }

  for (int i = 0; i < tree->jjtGetNumChildren(); i++) {
    SimpleNode* child = dynamic_cast<SimpleNode*>(tree->jjtGetChild(i));
    GOOGLESQL_RETURN_IF_ERROR(NormalizeParsedTree(child));
  }

  return absl::OkStatus();
}

absl::Status RQueryParser::Parse() {
  std::string encoded_query = EncodeNonAsciiCharacters(query_);
  // Trim leading and trailing separators to avoid parsing errors.
  auto start = encoded_query.find_first_not_of(kSeparators.data(), 0,
                                               kSeparators.size());
  if (start == std::string::npos) {
    encoded_query.clear();
  } else {
    auto end = encoded_query.find_last_not_of(
        kSeparators.data(), std::string::npos, kSeparators.size());
    encoded_query.erase(end + 1);
    encoded_query.erase(0, start);
  }
  if (encoded_query.empty()) {
    return absl::OkStatus();
  }

  // Create the JavaCC generated parser.
  SearchQueryCharStream char_stream(encoded_query);
  SearchQueryParserTokenManager token_manager(&char_stream);
  SearchQueryParser parser(&token_manager);

  std::vector<std::string> errors;
  // The parser owns the error handler and deletes it.
  parser.setErrorHandler(new SearchQueryParserErrorHandler(&errors));

  tree_ = absl::WrapUnique<SimpleNode>(parser.ParseRQuery());

  if (tree_ == nullptr) {
    std::string errors_string;
    switch (errors.size()) {
      case 0:
        errors_string = "Unknown error while parsing search query.";
        break;
      case 1:
        errors_string = errors[0];
        break;
      default:
        errors_string = absl::StrJoin(errors, "\n-");
        break;
    }
    return error::FailToParseSearchQuery(query_, errors_string);
  }

  GOOGLESQL_RETURN_IF_ERROR(NormalizeParsedTree(tree_.get()));

  return absl::OkStatus();
}

}  // namespace search
}  // namespace query
}  // namespace backend
}  // namespace emulator
}  // namespace spanner
}  // namespace google

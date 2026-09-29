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

#include <cstdint>
#include <algorithm>
#include <iterator>
#include <memory>
#include <string>
#include <utility>
#include <vector>

#include "googlesql/public/functions/string.h"
#include "googlesql/public/simple_token_list.h"
#include "googlesql/public/token_list_util.h"
#include "googlesql/public/value.h"
#include "absl/status/status.h"
#include "absl/status/statusor.h"
#include "absl/strings/match.h"
#include "absl/strings/numbers.h"
#include "absl/strings/ascii.h"
#include "absl/strings/str_cat.h"
#include "absl/strings/str_join.h"
#include "absl/strings/str_split.h"
#include "absl/strings/string_view.h"
#include "absl/types/span.h"
#include "common/errors.h"
#include "googlesql/base/status_macros.h"
#include "unicode/brkiter.h"
#include "unicode/locid.h"
#include "unicode/normalizer2.h"
#include "unicode/uchar.h"
#include "unicode/unistr.h"
#include "unicode/uscript.h"

namespace google {
namespace spanner {
namespace emulator {
namespace backend {
namespace query {
namespace search {

using TextToken = googlesql::tokens::TextToken;
using TokenList = googlesql::tokens::TokenList;

namespace {
constexpr absl::string_view kRelativeSearchTypeWordPrefix = "word_prefix";
constexpr absl::string_view kRelativeSearchTypeWordSuffix = "word_suffix";
constexpr absl::string_view kRelativeSearchTypeValuePrefix = "value_prefix";
constexpr absl::string_view kRelativeSearchTypeValueSuffix = "value_suffix";
constexpr absl::string_view kRelateiveSearchTypeAdjacentAndInOrder =
    "adjacent_and_in_order";
constexpr absl::string_view kRelativeSearchTypePhrase = "phrase";
}  // namespace

googlesql::Value TokenListFromStrings(std::vector<std::string> strings) {
  return googlesql::TokenListFromStringArray(strings);
}

absl::StatusOr<std::vector<std::string>> StringsFromTokenList(
    const googlesql::Value& tokenList) {
  std::vector<std::string> strings;

  GOOGLESQL_ASSIGN_OR_RETURN(auto iter, tokenList.tokenlist_value().GetIterator());
  TextToken token;
  while (!iter.done() && iter.Next(token).ok()) {
    strings.push_back(std::string(token.text()));
  }

  return strings;
}

absl::StatusOr<std::string> DebugTokenList(const googlesql::Value& tokenlist) {
  GOOGLESQL_ASSIGN_OR_RETURN(auto iter, tokenlist.tokenlist_value().GetIterator());
  std::vector<std::string> shown_tokens;
  TextToken token;
  for (bool first = true; !iter.done(); first = false) {
    GOOGLESQL_RETURN_IF_ERROR(iter.Next(token));
    // Every tokenlist starts with the signature of its tokenizer, which is
    // repeated for each tokenlist merged by TOKENLIST_CONCAT.
    if (first || token.text() == kGapString ||
        IsTokenizerSignature(token.text())) {
      continue;
    }
    // Tokens at the same position are the index tokens of one text token.
    std::vector<absl::string_view> texts;
    bool boundary = false;
    for (const googlesql::tokens::Token& index_token : token.index_tokens()) {
      texts.push_back(index_token.text());
      boundary |= (index_token.attribute() & kBoundaryIndexAttribute) != 0;
    }
    if (texts.empty()) texts.push_back(token.text());
    std::string shown = texts.size() == 1
                            ? std::string(texts.front())
                            : absl::StrCat("[", absl::StrJoin(texts, ", "), "]");
    if (boundary) absl::StrAppend(&shown, "(boundary)");
    shown_tokens.push_back(std::move(shown));
  }
  return absl::StrJoin(shown_tokens, ", ");
}

googlesql::Value TokenListFromBytes(std::string& bytes) {
  return googlesql::Value::TokenList(TokenList::FromBytesUnvalidated(bytes));
}

bool IsTokenizerSignature(absl::string_view token) {
  return absl::StartsWith(token, absl::StrCat(kExactMatchTokenizer, "-")) ||
         absl::StartsWith(token, absl::StrCat(kFullTextTokenizer, "-")) ||
         absl::StartsWith(token, absl::StrCat(kSubstringTokenizer, "-")) ||
         absl::StartsWith(token, absl::StrCat(kNumericTokenizer, "-")) ||
         absl::StartsWith(token, absl::StrCat(kBoolTokenizer, "-")) ||
         absl::StartsWith(token, absl::StrCat(kNgramsTokenizer, "-")) ||
         absl::StartsWith(token, absl::StrCat(kJsonTokenizer, "-"));
}

int64_t GetIntParameterValue(absl::Span<const googlesql::Value> args,
                             int parameter_index, int default_value) {
  return args.size() > parameter_index && !args[parameter_index].is_null()
             ? args[parameter_index].int64_value()
             : default_value;
}

double GetDoubleParameterValue(absl::Span<const googlesql::Value> args,
                               int parameter_index, double default_value) {
  return args.size() > parameter_index && !args[parameter_index].is_null()
             ? args[parameter_index].double_value()
             : default_value;
}

bool GetBoolParameterValue(absl::Span<const googlesql::Value> args,
                           int parameter_index, bool default_value) {
  return args.size() > parameter_index && !args[parameter_index].is_null()
             ? args[parameter_index].bool_value()
             : default_value;
}

absl::StatusOr<RelativeSearchType> ParseRelativeSearchType(
    absl::string_view relative_search_type) {
  if (absl::EqualsIgnoreCase(relative_search_type,
                             kRelativeSearchTypeWordPrefix)) {
    return RelativeSearchType::Word_Prefix;
  }
  if (absl::EqualsIgnoreCase(relative_search_type,
                             kRelativeSearchTypeWordSuffix)) {
    return RelativeSearchType::Word_Suffix;
  }
  if (absl::EqualsIgnoreCase(relative_search_type,
                             kRelativeSearchTypeValuePrefix)) {
    return RelativeSearchType::Value_Prefix;
  }
  if (absl::EqualsIgnoreCase(relative_search_type,
                             kRelativeSearchTypeValueSuffix)) {
    return RelativeSearchType::Value_Suffix;
  }
  if (absl::EqualsIgnoreCase(relative_search_type, kRelativeSearchTypePhrase) ||
      absl::EqualsIgnoreCase(relative_search_type,
                             kRelateiveSearchTypeAdjacentAndInOrder)) {
    return RelativeSearchType::Phrase;
  }

  return error::InvalidRelativeSearchType(relative_search_type);
}

absl::Status TokenizeSubstring(absl::string_view str,
                               std::vector<std::string>& token_list) {
  return TokenizeWords(str, "", token_list);
}

absl::Status TokenizeNgrams(absl::string_view str, int ngram_size_min,
                            int ngram_size_max,
                            std::vector<std::string>& token_list) {
  icu::UnicodeString text = icu::UnicodeString::fromUTF8(
      icu::StringPiece(str.data(), str.size()));
  std::vector<int32_t> offsets{0};
  for (int32_t i = 0; i < text.length();) {
    i = text.moveIndex32(i, 1);
    offsets.push_back(i);
  }
  for (int n = ngram_size_min; n <= ngram_size_max; ++n) {
    if (n >= offsets.size()) {
      break;
    }
    for (int i = 0; i + n < offsets.size(); ++i) {
      std::string token;
      text.tempSubStringBetween(offsets[i], offsets[i + n])
          .toUTF8String(token);
      token_list.push_back(std::move(token));
    }
  }

  return absl::OkStatus();
}

absl::StatusOr<std::string> NormalizeSearchText(
    absl::string_view str, bool remove_diacritics,
    absl::string_view language_tag, bool lowercase) {
  std::string lower;
  absl::Status status;
  googlesql::functions::LowerUtf8(str, &lower, &status);
  GOOGLESQL_RETURN_IF_ERROR(status);

  icu::UnicodeString text = icu::UnicodeString::fromUTF8(icu::StringPiece(
      lowercase ? lower.data() : str.data(),
      lowercase ? lower.size() : str.size()));
  if (lowercase && !language_tag.empty()) {
    UErrorCode error = U_ZERO_ERROR;
    icu::Locale locale = icu::Locale::forLanguageTag(
        std::string(language_tag), error);
    if (U_FAILURE(error) || locale.isBogus()) {
      return absl::InvalidArgumentError("Invalid language_tag");
    }
    text = icu::UnicodeString::fromUTF8(
        icu::StringPiece(str.data(), str.size()));
    text.toLower(locale);
  }
  if (remove_diacritics) {
    UErrorCode error = U_ZERO_ERROR;
    const icu::Normalizer2* nfd = icu::Normalizer2::getNFDInstance(error);
    if (U_FAILURE(error)) return absl::InternalError("ICU NFD unavailable");
    icu::UnicodeString decomposed;
    nfd->normalize(text, decomposed, error);
    if (U_FAILURE(error)) return absl::InternalError("ICU NFD failed");
    text.remove();
    for (int32_t i = 0; i < decomposed.length();) {
      UChar32 c = decomposed.char32At(i);
      i = decomposed.moveIndex32(i, 1);
      int8_t type = u_charType(c);
      if (type != U_NON_SPACING_MARK && type != U_COMBINING_SPACING_MARK &&
          type != U_ENCLOSING_MARK) {
        text.append(c);
      }
    }
  }
  std::string result;
  text.toUTF8String(result);
  return result;
}

namespace {

bool IsEastAsianScript(UScriptCode script) {
  return script == USCRIPT_HAN || script == USCRIPT_HIRAGANA ||
         script == USCRIPT_KATAKANA || script == USCRIPT_HANGUL;
}

std::vector<std::string> SplitAtEastAsianScriptChanges(absl::string_view word) {
  icu::UnicodeString text =
      icu::UnicodeString::fromUTF8(icu::StringPiece(word.data(), word.size()));
  std::vector<std::string> parts;
  int32_t part_begin = 0;
  // The script of the current part, ignoring common characters such as the
  // Katakana prolonged sound mark.
  UScriptCode part_script = USCRIPT_COMMON;
  for (int32_t i = 0; i < text.length(); i = text.moveIndex32(i, 1)) {
    UErrorCode error = U_ZERO_ERROR;
    const UScriptCode script = uscript_getScript(text.char32At(i), &error);
    if (U_FAILURE(error) || script == USCRIPT_COMMON ||
        script == USCRIPT_INHERITED) {
      continue;
    }
    if (IsEastAsianScriptChange(part_script, script)) {
      text.tempSubStringBetween(part_begin, i).toUTF8String(
          parts.emplace_back());
      part_begin = i;
    }
    part_script = script;
  }
  text.tempSubStringBetween(part_begin, text.length())
      .toUTF8String(parts.emplace_back());
  return parts;
}

}  // namespace

bool IsEastAsianScriptChange(UScriptCode previous, UScriptCode current) {
  auto is_specific = [](UScriptCode script) {
    return script != USCRIPT_COMMON && script != USCRIPT_INHERITED;
  };
  return is_specific(previous) && is_specific(current) &&
         previous != current &&
         (IsEastAsianScript(previous) || IsEastAsianScript(current));
}

absl::Status TokenizeWords(absl::string_view str,
                           absl::string_view language_tag,
                           std::vector<std::string>& token_list,
                           std::vector<bool>* hashtags) {
  UErrorCode error = U_ZERO_ERROR;
  icu::Locale locale = language_tag.empty()
                           ? icu::Locale::getRoot()
                           : icu::Locale::forLanguageTag(
                                 std::string(language_tag), error);
  if (U_FAILURE(error) || locale.isBogus()) {
    return absl::InvalidArgumentError("Invalid language_tag");
  }
  std::unique_ptr<icu::BreakIterator> words(
      icu::BreakIterator::createWordInstance(locale, error));
  if (U_FAILURE(error) || words == nullptr) {
    return absl::InternalError("ICU word iterator unavailable");
  }
  icu::UnicodeString text = icu::UnicodeString::fromUTF8(
      icu::StringPiece(str.data(), str.size()));
  words->setText(text);
  for (int32_t begin = words->first(), end = words->next();
       end != icu::BreakIterator::DONE; begin = end, end = words->next()) {
    if (words->getRuleStatus() == UBRK_WORD_NONE) continue;
    std::string segment;
    text.tempSubStringBetween(begin, end).toUTF8String(segment);
    // A hashtag is a word right after a '#' that does not follow a word.
    const bool hashtag = begin > 0 && text.charAt(begin - 1) == '#' &&
                         (begin == 1 || !u_isalnum(text.char32At(begin - 2)));
    bool first_token = true;
    for (absl::string_view token : absl::StrSplit(
             segment, absl::ByAnyChar(kDelimiter), absl::SkipEmpty())) {
      for (std::string& word : SplitAtEastAsianScriptChanges(token)) {
        token_list.push_back(std::move(word));
        if (hashtags != nullptr) hashtags->push_back(hashtag && first_token);
        first_token = false;
      }
    }
  }
  return absl::OkStatus();
}

namespace {

// The named character references of HTML 4 and &apos;, sorted by name.
constexpr std::pair<absl::string_view, UChar32> kHtmlEntities[] = {
    {"AElig", 198}, {"Aacute", 193}, {"Acirc", 194}, {"Agrave", 192},
    {"Alpha", 913}, {"Aring", 197}, {"Atilde", 195}, {"Auml", 196},
    {"Beta", 914}, {"Ccedil", 199}, {"Chi", 935}, {"Dagger", 8225},
    {"Delta", 916}, {"ETH", 208}, {"Eacute", 201}, {"Ecirc", 202},
    {"Egrave", 200}, {"Epsilon", 917}, {"Eta", 919}, {"Euml", 203},
    {"Gamma", 915}, {"Iacute", 205}, {"Icirc", 206}, {"Igrave", 204},
    {"Iota", 921}, {"Iuml", 207}, {"Kappa", 922}, {"Lambda", 923}, {"Mu", 924},
    {"Ntilde", 209}, {"Nu", 925}, {"OElig", 338}, {"Oacute", 211},
    {"Ocirc", 212}, {"Ograve", 210}, {"Omega", 937}, {"Omicron", 927},
    {"Oslash", 216}, {"Otilde", 213}, {"Ouml", 214}, {"Phi", 934}, {"Pi", 928},
    {"Prime", 8243}, {"Psi", 936}, {"Rho", 929}, {"Scaron", 352},
    {"Sigma", 931}, {"THORN", 222}, {"Tau", 932}, {"Theta", 920},
    {"Uacute", 218}, {"Ucirc", 219}, {"Ugrave", 217}, {"Upsilon", 933},
    {"Uuml", 220}, {"Xi", 926}, {"Yacute", 221}, {"Yuml", 376}, {"Zeta", 918},
    {"aacute", 225}, {"acirc", 226}, {"acute", 180}, {"aelig", 230},
    {"agrave", 224}, {"alefsym", 8501}, {"alpha", 945}, {"amp", 38},
    {"and", 8743}, {"ang", 8736}, {"apos", 39}, {"aring", 229},
    {"asymp", 8776}, {"atilde", 227}, {"auml", 228}, {"bdquo", 8222},
    {"beta", 946}, {"brvbar", 166}, {"bull", 8226}, {"cap", 8745},
    {"ccedil", 231}, {"cedil", 184}, {"cent", 162}, {"chi", 967},
    {"circ", 710}, {"clubs", 9827}, {"cong", 8773}, {"copy", 169},
    {"crarr", 8629}, {"cup", 8746}, {"curren", 164}, {"dArr", 8659},
    {"dagger", 8224}, {"darr", 8595}, {"deg", 176}, {"delta", 948},
    {"diams", 9830}, {"divide", 247}, {"eacute", 233}, {"ecirc", 234},
    {"egrave", 232}, {"empty", 8709}, {"emsp", 8195}, {"ensp", 8194},
    {"epsilon", 949}, {"equiv", 8801}, {"eta", 951}, {"eth", 240},
    {"euml", 235}, {"euro", 8364}, {"exist", 8707}, {"fnof", 402},
    {"forall", 8704}, {"frac12", 189}, {"frac14", 188}, {"frac34", 190},
    {"frasl", 8260}, {"gamma", 947}, {"ge", 8805}, {"gt", 62}, {"hArr", 8660},
    {"harr", 8596}, {"hearts", 9829}, {"hellip", 8230}, {"iacute", 237},
    {"icirc", 238}, {"iexcl", 161}, {"igrave", 236}, {"image", 8465},
    {"infin", 8734}, {"int", 8747}, {"iota", 953}, {"iquest", 191},
    {"isin", 8712}, {"iuml", 239}, {"kappa", 954}, {"lArr", 8656},
    {"lambda", 955}, {"lang", 9001}, {"laquo", 171}, {"larr", 8592},
    {"lceil", 8968}, {"ldquo", 8220}, {"le", 8804}, {"lfloor", 8970},
    {"lowast", 8727}, {"loz", 9674}, {"lrm", 8206}, {"lsaquo", 8249},
    {"lsquo", 8216}, {"lt", 60}, {"macr", 175}, {"mdash", 8212},
    {"micro", 181}, {"middot", 183}, {"minus", 8722}, {"mu", 956},
    {"nabla", 8711}, {"nbsp", 160}, {"ndash", 8211}, {"ne", 8800},
    {"ni", 8715}, {"not", 172}, {"notin", 8713}, {"nsub", 8836},
    {"ntilde", 241}, {"nu", 957}, {"oacute", 243}, {"ocirc", 244},
    {"oelig", 339}, {"ograve", 242}, {"oline", 8254}, {"omega", 969},
    {"omicron", 959}, {"oplus", 8853}, {"or", 8744}, {"ordf", 170},
    {"ordm", 186}, {"oslash", 248}, {"otilde", 245}, {"otimes", 8855},
    {"ouml", 246}, {"para", 182}, {"part", 8706}, {"permil", 8240},
    {"perp", 8869}, {"phi", 966}, {"pi", 960}, {"piv", 982}, {"plusmn", 177},
    {"pound", 163}, {"prime", 8242}, {"prod", 8719}, {"prop", 8733},
    {"psi", 968}, {"quot", 34}, {"rArr", 8658}, {"radic", 8730},
    {"rang", 9002}, {"raquo", 187}, {"rarr", 8594}, {"rceil", 8969},
    {"rdquo", 8221}, {"real", 8476}, {"reg", 174}, {"rfloor", 8971},
    {"rho", 961}, {"rlm", 8207}, {"rsaquo", 8250}, {"rsquo", 8217},
    {"sbquo", 8218}, {"scaron", 353}, {"sdot", 8901}, {"sect", 167},
    {"shy", 173}, {"sigma", 963}, {"sigmaf", 962}, {"sim", 8764},
    {"spades", 9824}, {"sub", 8834}, {"sube", 8838}, {"sum", 8721},
    {"sup", 8835}, {"sup1", 185}, {"sup2", 178}, {"sup3", 179}, {"supe", 8839},
    {"szlig", 223}, {"tau", 964}, {"there4", 8756}, {"theta", 952},
    {"thetasym", 977}, {"thinsp", 8201}, {"thorn", 254}, {"tilde", 732},
    {"times", 215}, {"trade", 8482}, {"uArr", 8657}, {"uacute", 250},
    {"uarr", 8593}, {"ucirc", 251}, {"ugrave", 249}, {"uml", 168},
    {"upsih", 978}, {"upsilon", 965}, {"uuml", 252}, {"weierp", 8472},
    {"xi", 958}, {"yacute", 253}, {"yen", 165}, {"yuml", 255}, {"zeta", 950},
    {"zwj", 8205}, {"zwnj", 8204},
};

// Decodes the character reference that starts with the '&' at `html[pos]`.
// Returns the code point, or 0 if there is no valid reference, and sets
// `length` to the length of the reference. The trailing ';' is optional, as
// HTML parsers accept references without it.
UChar32 DecodeCharacterReference(absl::string_view html, size_t pos,
                                 size_t& length) {
  size_t end = pos + 1;
  UChar32 decoded = 0;
  if (end < html.size() && html[end] == '#') {
    ++end;
    const bool hex = end < html.size() && (html[end] == 'x' || html[end] == 'X');
    if (hex) ++end;
    const size_t digits_begin = end;
    uint32_t code = 0;
    while (end < html.size() && (hex ? absl::ascii_isxdigit(html[end])
                                     : absl::ascii_isdigit(html[end]))) {
      // Clamp to an invalid code point instead of overflowing.
      code = std::min<uint32_t>(
          code * (hex ? 16 : 10) +
              (absl::ascii_isdigit(html[end])
                   ? html[end] - '0'
                   : absl::ascii_tolower(html[end]) - 'a' + 10),
          0x110000);
      ++end;
    }
    if (end == digits_begin) return 0;
    // Invalid code points decode to the replacement character.
    decoded = code == 0 || code > 0x10ffff || (code >= 0xd800 && code <= 0xdfff)
                  ? 0xfffd
                  : code;
  } else {
    while (end < html.size() && absl::ascii_isalnum(html[end])) ++end;
    const absl::string_view name = html.substr(pos + 1, end - pos - 1);
    const auto* entity = std::lower_bound(
        std::begin(kHtmlEntities), std::end(kHtmlEntities), name,
        [](const auto& entry, absl::string_view key) {
          return entry.first < key;
        });
    if (entity == std::end(kHtmlEntities) || entity->first != name) return 0;
    decoded = entity->second;
  }
  if (end < html.size() && html[end] == ';') ++end;
  length = end - pos;
  return decoded;
}

// Returns the lowercase name of the tag in `tag`, the text between '<' and
// '>' without a leading '/'.
std::string TagName(absl::string_view tag) {
  size_t name_end = 0;
  while (name_end < tag.size() &&
         (absl::ascii_isalnum(tag[name_end]) || tag[name_end] == '-')) {
    ++name_end;
  }
  return absl::AsciiStrToLower(tag.substr(0, name_end));
}

// Returns the position of the '>' that ends the tag starting at `html[pos]`,
// skipping '>' inside quoted attribute values, or npos.
size_t TagEnd(absl::string_view html, size_t pos) {
  char quote = 0;
  for (size_t i = pos + 1; i < html.size(); ++i) {
    if (quote != 0) {
      if (html[i] == quote) quote = 0;
    } else if (html[i] == '"' || html[i] == '\'') {
      quote = html[i];
    } else if (html[i] == '>') {
      return i;
    }
  }
  return absl::string_view::npos;
}

}  // namespace

std::vector<HtmlTextSegment> ExtractHtmlText(absl::string_view html) {
  const std::string lower_html = absl::AsciiStrToLower(html);
  std::vector<HtmlTextSegment> result;
  std::vector<std::pair<std::string, int>> tags;
  std::string current;
  auto category = [&]() {
    int value = 0;
    for (const auto& tag : tags) value = std::max(value, tag.second);
    return value;
  };
  auto flush = [&]() {
    if (!current.empty()) {
      result.push_back({std::move(current), category()});
      current.clear();
    }
  };
  for (size_t i = 0; i < html.size();) {
    if (absl::StartsWith(html.substr(i), "<!--")) {
      flush();
      size_t end = html.find("-->", i + 4);
      i = end == absl::string_view::npos ? html.size() : end + 3;
      continue;
    }
    // Like HTML parsers, only treat '<' as markup when a tag name, '/', '!'
    // or '?' follows it.
    const bool markup =
        html[i] == '<' && i + 1 < html.size() &&
        (absl::ascii_isalpha(html[i + 1]) || html[i + 1] == '/' ||
         html[i + 1] == '!' || html[i + 1] == '?');
    const size_t end =
        markup ? TagEnd(html, i) : absl::string_view::npos;
    if (end != absl::string_view::npos) {
      flush();
      absl::string_view tag =
          absl::StripAsciiWhitespace(html.substr(i + 1, end - i - 1));
      const bool closing = absl::ConsumePrefix(&tag, "/");
      const std::string name = TagName(tag);
      i = end + 1;
      if (closing) {
        for (size_t j = tags.size(); j > 0; --j) {
          if (tags[j - 1].first == name) {
            tags.resize(j - 1);
            break;
          }
        }
      } else if ((name == "script" || name == "style") &&
                 !absl::EndsWith(tag, "/")) {
        // Scripts and style sheets are not text; skip to the end tag.
        size_t close = lower_html.find(absl::StrCat("</", name), i);
        size_t close_end = close == std::string::npos
                               ? std::string::npos
                               : TagEnd(html, close);
        i = close_end == std::string::npos ? html.size() : close_end + 1;
      } else if (!name.empty() && !absl::EndsWith(tag, "/") && name != "br" &&
                 name != "hr" && name != "img" && name != "meta" &&
                 name != "input" && name != "link" && name != "wbr") {
        int prominence = name == "title" || name == "h1"
                             ? 3
                             : name == "h2" ? 2
                             : name == "h3" || name == "b" ||
                                       name == "strong" ? 1
                                                        : 0;
        tags.emplace_back(name, prominence);
      }
      continue;
    }
    if (html[i] == '&') {
      size_t length = 0;
      if (UChar32 decoded = DecodeCharacterReference(html, i, length);
          decoded != 0) {
        // A no-break space separates words like a space.
        icu::UnicodeString(decoded == 0xa0 ? ' ' : decoded)
            .toUTF8String(current);
        i += length;
        continue;
      }
    }
    current.push_back(html[i++]);
  }
  flush();
  return result;
}

bool SignatureHasFlag(absl::string_view signature, absl::string_view flag) {
  std::vector<absl::string_view> parts = absl::StrSplit(signature, '-');
  return std::find(parts.begin() + 1, parts.end(), flag) != parts.end();
}

absl::StatusOr<bool> TokenListRemovesDiacritics(
    const googlesql::Value& tokenlist) {
  GOOGLESQL_ASSIGN_OR_RETURN(auto tokens, StringsFromTokenList(tokenlist));
  for (const std::string& token : tokens) {
    if (IsTokenizerSignature(token) && SignatureHasFlag(token, "d")) {
      return true;
    }
  }
  return false;
}

}  // namespace search
}  // namespace query
}  // namespace backend
}  // namespace emulator
}  // namespace spanner
}  // namespace google

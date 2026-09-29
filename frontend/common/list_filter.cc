//
// Copyright 2026 Google LLC
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

#include "frontend/common/list_filter.h"

#include <algorithm>
#include <cctype>
#include <cstdint>
#include <memory>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "google/longrunning/operations.pb.h"
#include "google/protobuf/descriptor.h"
#include "google/protobuf/message.h"
#include "absl/status/status.h"
#include "absl/status/statusor.h"
#include "absl/strings/ascii.h"
#include "absl/strings/escaping.h"
#include "absl/strings/match.h"
#include "absl/strings/numbers.h"
#include "absl/strings/str_cat.h"
#include "absl/strings/str_split.h"
#include "absl/time/time.h"
#include "absl/types/span.h"
#include "googlesql/base/status_macros.h"

namespace google {
namespace spanner {
namespace emulator {
namespace frontend {

namespace {

using ::google::protobuf::Descriptor;
using ::google::protobuf::DescriptorPool;
using ::google::protobuf::FieldDescriptor;
using ::google::protobuf::Message;
using ::google::protobuf::MessageFactory;
using ::google::protobuf::Reflection;

constexpr size_t kMaxFilterLength = 8192;
constexpr size_t kMaxFilterTokens = 512;
constexpr int kMaxFilterDepth = 32;
constexpr size_t kMaxPageTokenLength = 16 * 1024;

bool IsOperator(std::string_view text) {
  return text == "=" || text == "!=" || text == ":" || text == "<" ||
         text == "<=" || text == ">" || text == ">=";
}

}  // namespace

// Parses the tokens by recursive descent, resolving predicates as it goes.
class ListFilter::Evaluator {
 public:
  Evaluator(const std::vector<Token>& tokens, ListFilterResolver resolver)
      : tokens_(tokens), resolver_(resolver) {}

  absl::StatusOr<bool> Evaluate() {
    GOOGLESQL_ASSIGN_OR_RETURN(bool result, ParseAnd(0));
    if (position_ != tokens_.size()) {
      return absl::InvalidArgumentError("Invalid filter expression");
    }
    return result;
  }

 private:
  bool Is(std::string_view word) const {
    return position_ < tokens_.size() && !tokens_[position_].quoted &&
           absl::EqualsIgnoreCase(tokens_[position_].text, word);
  }

  // Explicit and implicit AND bind loosest.
  absl::StatusOr<bool> ParseAnd(int depth) {
    GOOGLESQL_ASSIGN_OR_RETURN(bool result, ParseOr(depth));
    while (position_ < tokens_.size() && !Is(")")) {
      if (Is("AND")) ++position_;
      GOOGLESQL_ASSIGN_OR_RETURN(bool right, ParseOr(depth));
      result = result && right;
    }
    return result;
  }

  absl::StatusOr<bool> ParseOr(int depth) {
    GOOGLESQL_ASSIGN_OR_RETURN(bool result, ParseUnary(depth));
    while (Is("OR")) {
      ++position_;
      GOOGLESQL_ASSIGN_OR_RETURN(bool right, ParseUnary(depth));
      result = result || right;
    }
    return result;
  }

  absl::StatusOr<bool> ParseUnary(int depth) {
    if (depth > kMaxFilterDepth) {
      return absl::InvalidArgumentError("Filter nesting is too deep");
    }
    if (Is("NOT")) {
      ++position_;
      GOOGLESQL_ASSIGN_OR_RETURN(bool result, ParseUnary(depth + 1));
      return !result;
    }
    if (Is("(")) {
      ++position_;
      GOOGLESQL_ASSIGN_OR_RETURN(bool result, ParseAnd(depth + 1));
      if (!Is(")")) {
        return absl::InvalidArgumentError("Unclosed filter group");
      }
      ++position_;
      return result;
    }
    return ParsePredicate();
  }

  absl::StatusOr<bool> ParsePredicate() {
    if (position_ + 2 >= tokens_.size() || tokens_[position_].quoted ||
        tokens_[position_ + 1].quoted || Is("AND") || Is("OR") || Is(")") ||
        IsOperator(tokens_[position_].text)) {
      return absl::InvalidArgumentError("Invalid filter predicate");
    }
    const std::string field = absl::AsciiStrToLower(tokens_[position_++].text);
    const std::string& op = tokens_[position_++].text;
    if (!IsOperator(op)) {
      return absl::InvalidArgumentError("Invalid filter operator");
    }
    const Token& value = tokens_[position_++];
    if (!value.quoted &&
        (value.text == "(" || value.text == ")" || IsOperator(value.text))) {
      return absl::InvalidArgumentError("Invalid filter value");
    }
    return resolver_(field, op, value.text);
  }

  const std::vector<Token>& tokens_;
  ListFilterResolver resolver_;
  size_t position_ = 0;
};

absl::StatusOr<ListFilter> ListFilter::Parse(std::string_view expression) {
  ListFilter filter;
  GOOGLESQL_RETURN_IF_ERROR(filter.Tokenize(expression));
  // Checks the syntax up front; Matches then only reports resolver errors.
  GOOGLESQL_RETURN_IF_ERROR(
      filter
          .Matches([](std::string_view, std::string_view,
                      std::string_view) -> absl::StatusOr<bool> {
            return false;
          })
          .status());
  return filter;
}

absl::StatusOr<bool> ListFilter::Matches(ListFilterResolver resolver) const {
  if (tokens_.empty()) return true;
  return Evaluator(tokens_, resolver).Evaluate();
}

absl::Status ListFilter::Tokenize(std::string_view expression) {
  if (expression.size() > kMaxFilterLength) {
    return absl::InvalidArgumentError("Filter is too long");
  }
  if (expression.find('\0') != std::string_view::npos) {
    return absl::InvalidArgumentError("Invalid filter token");
  }
  for (size_t i = 0; i < expression.size();) {
    const unsigned char ch = expression[i];
    if (std::isspace(ch)) {
      ++i;
      continue;
    }
    if (ch == '"' || ch == '\'') {
      const char quote = ch;
      ++i;
      std::string value;
      bool closed = false;
      while (i < expression.size()) {
        char next = expression[i++];
        if (next == quote) {
          closed = true;
          break;
        }
        if (next == '\\') {
          if (i == expression.size() ||
              (expression[i] != quote && expression[i] != '\\')) {
            return absl::InvalidArgumentError("Invalid filter escape");
          }
          next = expression[i++];
        }
        value.push_back(next);
      }
      if (!closed) {
        return absl::InvalidArgumentError("Unterminated filter string");
      }
      tokens_.push_back({std::move(value), true});
    } else if (ch == '(' || ch == ')' || ch == ':' || ch == '=' || ch == '<' ||
               ch == '>' || ch == '!') {
      std::string symbol(1, ch);
      ++i;
      if ((ch == '<' || ch == '>' || ch == '!') && i < expression.size() &&
          expression[i] == '=') {
        symbol.push_back(expression[i++]);
      }
      if (symbol == "!") {
        return absl::InvalidArgumentError("Invalid filter operator");
      }
      tokens_.push_back({std::move(symbol), false});
    } else {
      const size_t start = i;
      while (i < expression.size() &&
             !std::isspace(static_cast<unsigned char>(expression[i])) &&
             std::string_view("()<>!=:\"").find(expression[i]) ==
                 std::string_view::npos) {
        ++i;
      }
      tokens_.push_back({std::string(expression.substr(start, i - start)),
                         false});
    }
    if (tokens_.size() > kMaxFilterTokens) {
      return absl::InvalidArgumentError("Filter has too many terms");
    }
  }
  return absl::OkStatus();
}

bool CompareString(std::string_view left, std::string_view right,
                   std::string_view op) {
  const std::string lower_left = absl::AsciiStrToLower(left);
  const std::string lower_right = absl::AsciiStrToLower(right);
  if (op == ":") {
    return lower_right == "*" ? !lower_left.empty()
                              : absl::StrContains(lower_left, lower_right);
  }
  return CompareOrdered(lower_left, lower_right, op);
}

absl::StatusOr<bool> MatchLabel(
    const google::protobuf::Map<std::string, std::string>& labels,
    std::string_view key, std::string_view op, std::string_view value) {
  if (key.empty()) {
    return absl::InvalidArgumentError("Label filter field needs a key");
  }
  const auto label = labels.find(std::string(key));
  if (op == ":" && value == "*") return label != labels.end();
  if (label == labels.end()) return op == "!=";
  return CompareString(label->second, value, op);
}

namespace {

// A value reached by a filter field path: a field of `message`, or its element
// at `index` for a repeated field.
struct FieldValue {
  const Message* message;
  const FieldDescriptor* field;
  int index;  // -1 for a singular field.
};

std::string NormalizeFieldName(std::string_view name) {
  std::string normalized = absl::AsciiStrToLower(name);
  normalized.erase(std::remove(normalized.begin(), normalized.end(), '_'),
                   normalized.end());
  return normalized;
}

bool IsAny(const Descriptor* type) {
  return type->full_name() == "google.protobuf.Any";
}

const FieldDescriptor* FindField(const Descriptor* type,
                                 std::string_view segment) {
  if (IsAny(type) && segment == "@type") {
    return type->FindFieldByName("type_url");
  }
  const std::string normalized = NormalizeFieldName(segment);
  for (int i = 0; i < type->field_count(); ++i) {
    if (NormalizeFieldName(type->field(i)->name()) == normalized) {
      return type->field(i);
    }
  }
  return nullptr;
}

// Collects the values at a field path, unpacking Any messages on the way.
class FieldCollector {
 public:
  explicit FieldCollector(std::string_view field) : field_(field) {}

  // Collects the values at `path` below `message`, which is null when the
  // message is unset; the path is then only checked against `type`. `packed`
  // is true below an Any, where fields depend on the packed type.
  absl::Status Collect(const Descriptor* type, const Message* message,
                       absl::Span<const std::string> path, bool packed,
                       std::vector<FieldValue>* values) {
    if (IsAny(type) && path.front() != "@type") {
      if (message == nullptr) return absl::OkStatus();
      const Reflection* reflection = message->GetReflection();
      const std::string type_url =
          reflection->GetString(*message, type->FindFieldByName("type_url"));
      const Descriptor* packed_type =
          DescriptorPool::generated_pool()->FindMessageTypeByName(
              type_url.substr(type_url.rfind('/') + 1));
      if (packed_type == nullptr) return absl::OkStatus();
      const Message* prototype =
          MessageFactory::generated_factory()->GetPrototype(packed_type);
      if (prototype == nullptr) return absl::OkStatus();
      std::unique_ptr<Message> unpacked(prototype->New());
      if (!unpacked->ParseFromString(
              reflection->GetString(*message, type->FindFieldByName("value")))) {
        return absl::OkStatus();
      }
      unpacked_.push_back(std::move(unpacked));
      return Collect(packed_type, unpacked_.back().get(), path,
                     /*packed=*/true, values);
    }
    const FieldDescriptor* field = FindField(type, path.front());
    const bool leaf = path.size() == 1;
    if (field == nullptr ||
        (!leaf && field->cpp_type() != FieldDescriptor::CPPTYPE_MESSAGE)) {
      return packed ? absl::OkStatus()
                    : absl::InvalidArgumentError(absl::StrCat(
                          "Unsupported operation filter field: ", field_));
    }
    const Reflection* reflection =
        message == nullptr ? nullptr : message->GetReflection();
    if (leaf) {
      if (message == nullptr) return absl::OkStatus();
      if (field->is_repeated()) {
        for (int i = 0; i < reflection->FieldSize(*message, field); ++i) {
          values->push_back({message, field, i});
        }
      } else if (field->cpp_type() != FieldDescriptor::CPPTYPE_MESSAGE ||
                 reflection->HasField(*message, field)) {
        values->push_back({message, field, -1});
      }
      return absl::OkStatus();
    }
    const absl::Span<const std::string> rest = path.subspan(1);
    if (message == nullptr ||
        (field->is_repeated() ? reflection->FieldSize(*message, field) == 0
                              : !reflection->HasField(*message, field))) {
      return Collect(field->message_type(), nullptr, rest, packed, values);
    }
    if (!field->is_repeated()) {
      return Collect(field->message_type(),
                     &reflection->GetMessage(*message, field), rest, packed,
                     values);
    }
    for (int i = 0; i < reflection->FieldSize(*message, field); ++i) {
      GOOGLESQL_RETURN_IF_ERROR(
          Collect(field->message_type(),
                  &reflection->GetRepeatedMessage(*message, field, i), rest,
                  packed, values));
    }
    return absl::OkStatus();
  }

 private:
  std::string_view field_;
  std::vector<std::unique_ptr<Message>> unpacked_;
};

bool ParseNumber(std::string_view text, int64_t* number) {
  return absl::SimpleAtoi(text, number);
}
bool ParseNumber(std::string_view text, uint64_t* number) {
  return absl::SimpleAtoi(text, number);
}
bool ParseNumber(std::string_view text, double* number) {
  return absl::SimpleAtod(text, number);
}

// Like the ListBackups size_bytes filter, `:` matches a substring of the
// decimal form.
template <typename T>
absl::StatusOr<bool> CompareNumber(T actual, std::string_view value,
                                   std::string_view op) {
  T expected;
  if (!ParseNumber(value, &expected)) {
    return absl::InvalidArgumentError(
        absl::StrCat("Invalid numeric filter value: ", value));
  }
  if (op == ":") return absl::StrContains(absl::StrCat(actual), value);
  return CompareOrdered(actual, expected, op);
}

absl::StatusOr<bool> CompareTimestamp(const Message& timestamp,
                                      std::string_view value,
                                      std::string_view op) {
  absl::Time expected;
  std::string error;
  if (!absl::ParseTime(absl::RFC3339_full, value, &expected, &error)) {
    return absl::InvalidArgumentError(
        absl::StrCat("Invalid time filter value: ", value));
  }
  const Reflection* reflection = timestamp.GetReflection();
  const Descriptor* type = timestamp.GetDescriptor();
  const absl::Time actual =
      absl::FromUnixSeconds(reflection->GetInt64(
          timestamp, type->FindFieldByName("seconds"))) +
      absl::Nanoseconds(
          reflection->GetInt32(timestamp, type->FindFieldByName("nanos")));
  if (op == ":") {
    return absl::StrContains(
        absl::FormatTime(absl::RFC3339_full, actual, absl::UTCTimeZone()),
        value);
  }
  return CompareOrdered(actual, expected, op);
}

absl::StatusOr<bool> CompareFieldValue(const FieldValue& field_value,
                                       std::string_view op,
                                       std::string_view value) {
  const Message& message = *field_value.message;
  const FieldDescriptor* field = field_value.field;
  const Reflection* reflection = message.GetReflection();
  const int index = field_value.index;
  const bool repeated = index >= 0;
  switch (field->cpp_type()) {
    case FieldDescriptor::CPPTYPE_STRING:
      return CompareString(
          repeated ? reflection->GetRepeatedString(message, field, index)
                   : reflection->GetString(message, field),
          value, op);
    case FieldDescriptor::CPPTYPE_ENUM:
      return CompareString(
          (repeated ? reflection->GetRepeatedEnum(message, field, index)
                    : reflection->GetEnum(message, field))
              ->name(),
          value, op);
    case FieldDescriptor::CPPTYPE_BOOL: {
      bool expected;
      if (!absl::SimpleAtob(value, &expected)) {
        return absl::InvalidArgumentError(
            absl::StrCat("Invalid boolean filter value: ", value));
      }
      const bool actual = repeated
                              ? reflection->GetRepeatedBool(message, field, index)
                              : reflection->GetBool(message, field);
      return CompareOrdered(actual, expected,
                            op == ":" ? std::string_view("=") : op);
    }
    case FieldDescriptor::CPPTYPE_INT32:
      return CompareNumber<int64_t>(
          repeated ? reflection->GetRepeatedInt32(message, field, index)
                   : reflection->GetInt32(message, field),
          value, op);
    case FieldDescriptor::CPPTYPE_INT64:
      return CompareNumber<int64_t>(
          repeated ? reflection->GetRepeatedInt64(message, field, index)
                   : reflection->GetInt64(message, field),
          value, op);
    case FieldDescriptor::CPPTYPE_UINT32:
      return CompareNumber<uint64_t>(
          repeated ? reflection->GetRepeatedUInt32(message, field, index)
                   : reflection->GetUInt32(message, field),
          value, op);
    case FieldDescriptor::CPPTYPE_UINT64:
      return CompareNumber<uint64_t>(
          repeated ? reflection->GetRepeatedUInt64(message, field, index)
                   : reflection->GetUInt64(message, field),
          value, op);
    case FieldDescriptor::CPPTYPE_FLOAT:
      return CompareNumber<double>(
          repeated ? reflection->GetRepeatedFloat(message, field, index)
                   : reflection->GetFloat(message, field),
          value, op);
    case FieldDescriptor::CPPTYPE_DOUBLE:
      return CompareNumber<double>(
          repeated ? reflection->GetRepeatedDouble(message, field, index)
                   : reflection->GetDouble(message, field),
          value, op);
    case FieldDescriptor::CPPTYPE_MESSAGE: {
      const Message& child =
          repeated ? reflection->GetRepeatedMessage(message, field, index)
                   : reflection->GetMessage(message, field);
      if (field->message_type()->full_name() == "google.protobuf.Timestamp") {
        return CompareTimestamp(child, value, op);
      }
      return absl::InvalidArgumentError(
          absl::StrCat("Filter field ", field->name(),
                       " is a message and only supports :*"));
    }
  }
  return absl::InvalidArgumentError(
      absl::StrCat("Unsupported filter field type: ", field->name()));
}

absl::StatusOr<bool> MatchFieldValues(const std::vector<FieldValue>& values,
                                      std::string_view op,
                                      std::string_view value) {
  if (op == ":" && value == "*") {
    return std::any_of(values.begin(), values.end(), [](const FieldValue& v) {
      return v.index >= 0 || v.message->GetReflection()->HasField(*v.message,
                                                                  v.field);
    });
  }
  // `!=` holds when no value is equal, so it also holds for no values.
  const bool negate = op == "!=";
  for (const FieldValue& field_value : values) {
    GOOGLESQL_ASSIGN_OR_RETURN(
        const bool matches,
        CompareFieldValue(field_value, negate ? std::string_view("=") : op,
                          value));
    if (matches) return !negate;
  }
  return negate;
}

}  // namespace

absl::StatusOr<bool> FilterMatchesOperation(
    const ListFilter& filter, const google::longrunning::Operation& operation) {
  return filter.Matches([&operation](std::string_view field,
                                     std::string_view op,
                                     std::string_view value)
                            -> absl::StatusOr<bool> {
    if (field == "error" && !(op == ":" && value == "*")) {
      return CompareString(operation.error().message(), value, op);
    }
    const std::vector<std::string> path = absl::StrSplit(field, '.');
    if (std::any_of(path.begin(), path.end(),
                    [](const std::string& segment) { return segment.empty(); })) {
      return absl::InvalidArgumentError(
          absl::StrCat("Unsupported operation filter field: ", field));
    }
    FieldCollector collector(field);
    std::vector<FieldValue> values;
    GOOGLESQL_RETURN_IF_ERROR(collector.Collect(
        google::longrunning::Operation::descriptor(), &operation, path,
        /*packed=*/false, &values));
    return MatchFieldValues(values, op, value);
  });
}

std::string MakeListPageToken(std::string_view filter,
                              std::string_view cursor) {
  std::string payload(filter);
  payload.push_back('\0');
  payload.append(cursor);
  return absl::WebSafeBase64Escape(payload);
}

absl::StatusOr<std::string> ParseListPageToken(std::string_view token,
                                               std::string_view filter) {
  std::string payload;
  if (token.size() > kMaxPageTokenLength ||
      !absl::WebSafeBase64Unescape(token, &payload)) {
    return absl::InvalidArgumentError("Invalid page token");
  }
  const size_t separator = payload.find('\0');
  if (separator == std::string::npos ||
      std::string_view(payload).substr(0, separator) != filter) {
    return absl::InvalidArgumentError("Page token does not match the filter");
  }
  return payload.substr(separator + 1);
}

}  // namespace frontend
}  // namespace emulator
}  // namespace spanner
}  // namespace google

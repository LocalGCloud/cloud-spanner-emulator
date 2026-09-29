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

#include "backend/query/spanner_string_functions.h"

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <string>
#include <vector>

#include "googlesql/common/utf_util.h"
#include "googlesql/public/function.h"
#include "googlesql/public/function_signature.h"
#include "googlesql/public/type.h"
#include "googlesql/public/value.h"
#include "absl/status/status.h"
#include "absl/status/statusor.h"
#include "absl/strings/str_cat.h"
#include "absl/strings/string_view.h"
#include "absl/types/span.h"
#include "googlesql/base/ret_check.h"
#include "googlesql/base/status_macros.h"
#include "zstd.h"

namespace google {
namespace spanner {
namespace emulator {
namespace backend {

namespace {

constexpr absl::string_view kBase32Alphabet =
    "ABCDEFGHIJKLMNOPQRSTUVWXYZ234567";

// Zstd levels accepted by Spanner's ZSTD_COMPRESS.
constexpr int64_t kMinZstdLevel = -5;
constexpr int64_t kMaxZstdLevel = 22;

// RFC 4648 base32 with '=' padding, the encoding TO_BASE32 produces.
std::string Base32Encode(absl::string_view input) {
  // Significant output characters for a final group of 0-5 input bytes.
  static constexpr int kSignificantChars[] = {0, 2, 4, 5, 7, 8};
  std::string output;
  output.reserve((input.size() + 4) / 5 * 8);
  for (size_t i = 0; i < input.size(); i += 5) {
    const size_t group_size = std::min<size_t>(5, input.size() - i);
    uint64_t group = 0;
    for (size_t j = 0; j < 5; ++j) {
      group = (group << 8) |
              (j < group_size ? static_cast<uint8_t>(input[i + j]) : 0);
    }
    for (int c = 0; c < 8; ++c) {
      output.push_back(c < kSignificantChars[group_size]
                           ? kBase32Alphabet[(group >> (35 - 5 * c)) & 0x1f]
                           : '=');
    }
  }
  return output;
}

absl::StatusOr<std::string> Base32Decode(absl::string_view input) {
  const absl::Status invalid =
      absl::OutOfRangeError("Failed to decode invalid base32 string");
  while (!input.empty() && input.back() == '=') {
    input.remove_suffix(1);
  }
  // A final group of 1, 3 or 6 characters cannot encode whole bytes.
  const size_t remainder = input.size() % 8;
  if (remainder == 1 || remainder == 3 || remainder == 6) {
    return invalid;
  }
  std::string output;
  output.reserve(input.size() * 5 / 8);
  uint32_t buffer = 0;
  int bits = 0;
  for (char c : input) {
    int value;
    if (c >= 'A' && c <= 'Z') {
      value = c - 'A';
    } else if (c >= 'a' && c <= 'z') {
      value = c - 'a';
    } else if (c >= '2' && c <= '7') {
      value = c - '2' + 26;
    } else {
      return invalid;
    }
    buffer = (buffer << 5) | value;
    bits += 5;
    if (bits >= 8) {
      bits -= 8;
      output.push_back(static_cast<char>((buffer >> bits) & 0xff));
    }
  }
  return output;
}

absl::StatusOr<googlesql::Value> EvalToBase32(
    absl::Span<const googlesql::Value> args) {
  GOOGLESQL_RET_CHECK_EQ(args.size(), 1);
  if (args[0].is_null()) {
    return googlesql::Value::NullString();
  }
  return googlesql::Value::String(Base32Encode(args[0].bytes_value()));
}

absl::StatusOr<googlesql::Value> EvalFromBase32(
    absl::Span<const googlesql::Value> args) {
  GOOGLESQL_RET_CHECK_EQ(args.size(), 1);
  if (args[0].is_null()) {
    return googlesql::Value::NullBytes();
  }
  GOOGLESQL_ASSIGN_OR_RETURN(std::string bytes,
                             Base32Decode(args[0].string_value()));
  return googlesql::Value::Bytes(bytes);
}

absl::Status ZstdOutputTooLarge(uint64_t size, int64_t size_limit) {
  return absl::OutOfRangeError(absl::StrCat("ZSTD output is too large: (",
                                            size, " bytes) > limit (",
                                            size_limit, " bytes)"));
}

absl::StatusOr<std::string> ZstdDecompress(absl::string_view input,
                                           int64_t size_limit) {
  if (size_limit < 0) {
    return absl::OutOfRangeError(absl::StrCat(
        "ZSTD size_limit must not be negative: ", size_limit));
  }
  const absl::Status invalid =
      absl::OutOfRangeError("Failed to decompress invalid ZSTD input");
  // The first frame's declared size gives the exact size in the error; the
  // streaming loop below still enforces the limit for any later frames.
  const unsigned long long declared_size =  // NOLINT(runtime/int)
      ZSTD_getFrameContentSize(input.data(), input.size());
  if (declared_size == ZSTD_CONTENTSIZE_ERROR) {
    return invalid;
  }
  if (declared_size != ZSTD_CONTENTSIZE_UNKNOWN &&
      declared_size > static_cast<uint64_t>(size_limit)) {
    return ZstdOutputTooLarge(declared_size, size_limit);
  }

  std::unique_ptr<ZSTD_DCtx, decltype(&ZSTD_freeDCtx)> context(
      ZSTD_createDCtx(), &ZSTD_freeDCtx);
  std::string output;
  std::string chunk(ZSTD_DStreamOutSize(), '\0');
  ZSTD_inBuffer in{input.data(), input.size(), 0};
  size_t remaining = 0;
  do {
    ZSTD_outBuffer out{chunk.data(), chunk.size(), 0};
    remaining = ZSTD_decompressStream(context.get(), &out, &in);
    if (ZSTD_isError(remaining)) {
      return invalid;
    }
    output.append(chunk.data(), out.pos);
    if (output.size() > static_cast<uint64_t>(size_limit)) {
      return ZstdOutputTooLarge(output.size(), size_limit);
    }
    // All input is consumed and the decoder has no buffered output left.
    if (in.pos == in.size && out.pos < out.size) {
      break;
    }
  } while (true);
  if (remaining != 0) {
    // The last frame is truncated.
    return invalid;
  }
  return output;
}

absl::StatusOr<googlesql::Value> EvalZstdCompress(
    absl::Span<const googlesql::Value> args) {
  GOOGLESQL_RET_CHECK_EQ(args.size(), 2);
  if (args[0].is_null() || args[1].is_null()) {
    return googlesql::Value::NullBytes();
  }
  const int64_t level = args[1].int64_value();
  if (level < kMinZstdLevel || level > kMaxZstdLevel) {
    return absl::OutOfRangeError(
        absl::StrCat("ZSTD compression level must be between ", kMinZstdLevel,
                     " and ", kMaxZstdLevel, ": ", level));
  }
  const absl::string_view input = args[0].type()->IsString()
                                      ? args[0].string_value()
                                      : args[0].bytes_value();
  std::string output(ZSTD_compressBound(input.size()), '\0');
  const size_t size = ZSTD_compress(output.data(), output.size(), input.data(),
                                    input.size(), static_cast<int>(level));
  if (ZSTD_isError(size)) {
    return absl::InternalError(
        absl::StrCat("ZSTD compression failed: ", ZSTD_getErrorName(size)));
  }
  output.resize(size);
  return googlesql::Value::Bytes(output);
}

absl::StatusOr<googlesql::Value> EvalZstdDecompressToBytes(
    absl::Span<const googlesql::Value> args) {
  GOOGLESQL_RET_CHECK_EQ(args.size(), 2);
  if (args[0].is_null() || args[1].is_null()) {
    return googlesql::Value::NullBytes();
  }
  GOOGLESQL_ASSIGN_OR_RETURN(
      std::string output,
      ZstdDecompress(args[0].bytes_value(), args[1].int64_value()));
  return googlesql::Value::Bytes(output);
}

absl::StatusOr<googlesql::Value> EvalZstdDecompressToString(
    absl::Span<const googlesql::Value> args) {
  GOOGLESQL_RET_CHECK_EQ(args.size(), 2);
  if (args[0].is_null() || args[1].is_null()) {
    return googlesql::Value::NullString();
  }
  GOOGLESQL_ASSIGN_OR_RETURN(
      std::string output,
      ZstdDecompress(args[0].bytes_value(), args[1].int64_value()));
  if (!googlesql::IsWellFormedUTF8(output)) {
    return absl::OutOfRangeError(
        "ZSTD_DECOMPRESS_TO_STRING output is not valid UTF-8");
  }
  return googlesql::Value::String(output);
}

std::unique_ptr<googlesql::Function> MakeFunction(
    absl::string_view name, const std::string& catalog_name,
    googlesql::FunctionEvaluator evaluator,
    std::vector<googlesql::FunctionSignature> signatures) {
  googlesql::FunctionOptions options;
  options.set_evaluator(std::move(evaluator));
  return std::make_unique<googlesql::Function>(
      name, catalog_name, googlesql::Function::SCALAR, std::move(signatures),
      options);
}

}  // namespace

std::vector<std::unique_ptr<googlesql::Function>> SpannerStringFunctions(
    const std::string& catalog_name) {
  const googlesql::Type* bytes_type = googlesql::types::BytesType();
  const googlesql::Type* string_type = googlesql::types::StringType();
  const googlesql::Type* int64_type = googlesql::types::Int64Type();

  const googlesql::FunctionArgumentType level_arg(
      int64_type,
      googlesql::FunctionArgumentTypeOptions(
          googlesql::FunctionArgumentType::OPTIONAL)
          .set_argument_name("level", googlesql::kPositionalOrNamed)
          .set_default(googlesql::Value::Int64(3)));
  const googlesql::FunctionArgumentType size_limit_arg(
      int64_type,
      googlesql::FunctionArgumentTypeOptions(
          googlesql::FunctionArgumentType::OPTIONAL)
          .set_argument_name("size_limit", googlesql::kNamedOnly)
          .set_default(googlesql::Value::Int64(int64_t{1} << 30)));

  std::vector<std::unique_ptr<googlesql::Function>> functions;
  functions.push_back(MakeFunction(
      "to_base32", catalog_name, EvalToBase32,
      {googlesql::FunctionSignature(string_type, {bytes_type}, nullptr)}));
  functions.push_back(MakeFunction(
      "from_base32", catalog_name, EvalFromBase32,
      {googlesql::FunctionSignature(bytes_type, {string_type}, nullptr)}));
  functions.push_back(MakeFunction(
      "zstd_compress", catalog_name, EvalZstdCompress,
      {googlesql::FunctionSignature(bytes_type, {bytes_type, level_arg},
                                    nullptr),
       googlesql::FunctionSignature(bytes_type, {string_type, level_arg},
                                    nullptr)}));
  functions.push_back(MakeFunction(
      "zstd_decompress_to_bytes", catalog_name, EvalZstdDecompressToBytes,
      {googlesql::FunctionSignature(bytes_type, {bytes_type, size_limit_arg},
                                    nullptr)}));
  functions.push_back(MakeFunction(
      "zstd_decompress_to_string", catalog_name, EvalZstdDecompressToString,
      {googlesql::FunctionSignature(string_type, {bytes_type, size_limit_arg},
                                    nullptr)}));
  return functions;
}

}  // namespace backend
}  // namespace emulator
}  // namespace spanner
}  // namespace google

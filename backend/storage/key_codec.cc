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

#include "backend/storage/key_codec.h"

#include <cstdint>
#include <cstring>
#include <string>

#include "googlesql/public/numeric_value.h"
#include "googlesql/public/value.h"
#include "absl/time/time.h"

namespace google {
namespace spanner {
namespace emulator {
namespace backend {

namespace {

// Type prefix bytes. These are chosen so that NULLs sort before all other
// values (nulls-first) and Infinity sorts after everything.
constexpr uint8_t kNullFirst = 0x00;
constexpr uint8_t kBoolFalse = 0x01;
constexpr uint8_t kBoolTrue = 0x02;
constexpr uint8_t kInt64 = 0x03;
constexpr uint8_t kDouble = 0x04;
constexpr uint8_t kString = 0x05;
constexpr uint8_t kBytes = 0x06;
constexpr uint8_t kTimestamp = 0x07;
constexpr uint8_t kDate = 0x08;
constexpr uint8_t kNumeric = 0x09;
constexpr uint8_t kNullLast = 0xFE;
constexpr uint8_t kInfinity = 0xFF;

void AppendBigEndian64(std::string* out, uint64_t val) {
  char buf[8];
  buf[0] = static_cast<char>((val >> 56) & 0xFF);
  buf[1] = static_cast<char>((val >> 48) & 0xFF);
  buf[2] = static_cast<char>((val >> 40) & 0xFF);
  buf[3] = static_cast<char>((val >> 32) & 0xFF);
  buf[4] = static_cast<char>((val >> 24) & 0xFF);
  buf[5] = static_cast<char>((val >> 16) & 0xFF);
  buf[6] = static_cast<char>((val >> 8) & 0xFF);
  buf[7] = static_cast<char>(val & 0xFF);
  out->append(buf, 8);
}

void AppendBigEndian32(std::string* out, uint32_t val) {
  char buf[4];
  buf[0] = static_cast<char>((val >> 24) & 0xFF);
  buf[1] = static_cast<char>((val >> 16) & 0xFF);
  buf[2] = static_cast<char>((val >> 8) & 0xFF);
  buf[3] = static_cast<char>(val & 0xFF);
  out->append(buf, 4);
}

// Byte-stuffed encoding: each 0x00 in the input is replaced with 0x00 0x01,
// and the encoded sequence is terminated with 0x00 0x00.
void AppendByteStuffed(std::string* out, const std::string& data) {
  for (char c : data) {
    if (c == '\x00') {
      out->push_back('\x00');
      out->push_back('\x01');
    } else {
      out->push_back(c);
    }
  }
  out->push_back('\x00');
  out->push_back('\x00');
}

// Inverts all bytes in the string (for descending order).
void InvertBytes(std::string* s, size_t start, size_t len) {
  for (size_t i = start; i < start + len; ++i) {
    (*s)[i] = ~(*s)[i];
  }
}

void EncodeColumn(const googlesql::Value& value, bool is_descending,
                  bool is_nulls_last, std::string* out) {
  size_t start_pos = out->size();

  if (!value.is_valid() || value.is_null()) {
    out->push_back(is_nulls_last ? kNullLast : kNullFirst);
    if (is_descending) {
      InvertBytes(out, start_pos, out->size() - start_pos);
    }
    return;
  }

  switch (value.type_kind()) {
    case googlesql::TYPE_BOOL: {
      out->push_back(value.bool_value() ? kBoolTrue : kBoolFalse);
      break;
    }
    case googlesql::TYPE_INT64: {
      out->push_back(kInt64);
      // Flip the sign bit so that negative numbers sort before positive.
      uint64_t encoded =
          static_cast<uint64_t>(value.int64_value()) ^ (1ULL << 63);
      AppendBigEndian64(out, encoded);
      break;
    }
    case googlesql::TYPE_DOUBLE: {
      out->push_back(kDouble);
      double d = value.double_value();
      uint64_t bits;
      std::memcpy(&bits, &d, sizeof(bits));
      // If the sign bit is set (negative), flip all bits.
      // Otherwise, flip only the sign bit.
      if (bits & (1ULL << 63)) {
        bits = ~bits;
      } else {
        bits ^= (1ULL << 63);
      }
      AppendBigEndian64(out, bits);
      break;
    }
    case googlesql::TYPE_STRING: {
      out->push_back(kString);
      AppendByteStuffed(out, value.string_value());
      break;
    }
    case googlesql::TYPE_BYTES: {
      out->push_back(kBytes);
      AppendByteStuffed(out, value.bytes_value());
      break;
    }
    case googlesql::TYPE_TIMESTAMP: {
      out->push_back(kTimestamp);
      absl::Time t = value.ToTime();
      int64_t seconds = absl::ToUnixSeconds(t);
      int32_t nanos = static_cast<int32_t>(
          absl::ToInt64Nanoseconds(t - absl::FromUnixSeconds(seconds)));
      // Flip sign bit on seconds for proper ordering.
      uint64_t encoded_seconds =
          static_cast<uint64_t>(seconds) ^ (1ULL << 63);
      AppendBigEndian64(out, encoded_seconds);
      AppendBigEndian32(out, static_cast<uint32_t>(nanos));
      break;
    }
    case googlesql::TYPE_DATE: {
      out->push_back(kDate);
      int32_t date_val = value.date_value();
      // Flip sign bit for proper ordering.
      uint32_t encoded = static_cast<uint32_t>(date_val) ^ (1U << 31);
      AppendBigEndian32(out, encoded);
      break;
    }
    case googlesql::TYPE_NUMERIC: {
      out->push_back(kNumeric);
      // Use a fixed-width string encoding for order preservation.
      // NumericValue: 29 integer digits, 9 fractional digits.
      googlesql::NumericValue num = value.numeric_value();
      std::string num_str = num.ToString();
      bool is_negative = !num_str.empty() && num_str[0] == '-';
      if (is_negative) num_str = num_str.substr(1);

      size_t dot_pos = num_str.find('.');
      std::string int_part = (dot_pos != std::string::npos)
                                 ? num_str.substr(0, dot_pos)
                                 : num_str;
      std::string frac_part = (dot_pos != std::string::npos)
                                  ? num_str.substr(dot_pos + 1)
                                  : "";
      // Pad to fixed width: 29 integer digits + 9 fractional digits.
      while (int_part.size() < 29) int_part = "0" + int_part;
      while (frac_part.size() < 9) frac_part += "0";
      std::string normalized = int_part + frac_part;

      if (is_negative) {
        // Complement digits so larger negatives sort smaller.
        for (char& c : normalized) c = '0' + ('9' - (c - '0'));
        out->push_back(0x00);  // Negative prefix sorts before positive.
      } else {
        out->push_back(0x01);  // Positive prefix.
      }
      AppendByteStuffed(out, normalized);
      break;
    }
    default: {
      // For unsupported types, encode as bytes using the debug string.
      out->push_back(kBytes);
      AppendByteStuffed(out, value.DebugString());
      break;
    }
  }

  if (is_descending) {
    InvertBytes(out, start_pos, out->size() - start_pos);
  }
}

}  // namespace

std::string EncodeKey(const Key& key) {
  // Keys with 0 columns are either empty or infinity.
  // Both encode to empty string. Callers must handle infinity separately
  // (e.g., using empty limit_encoded = no upper bound in range scans).
  if (key.NumColumns() == 0) {
    return std::string();
  }

  std::string result;
  result.reserve(key.NumColumns() * 9);  // Rough estimate.

  for (int i = 0; i < key.NumColumns(); ++i) {
    EncodeColumn(key.ColumnValue(i), key.IsColumnDescending(i),
                 key.IsColumnNullsLast(i), &result);
  }

  return result;
}

std::string EncodeKeyForPrefixLimit(const Key& key) {
  std::string result = EncodeKey(key);
  // Append 0xFF bytes to ensure this sorts after any key with the same prefix.
  result.append(8, '\xFF');
  return result;
}

}  // namespace backend
}  // namespace emulator
}  // namespace spanner
}  // namespace google

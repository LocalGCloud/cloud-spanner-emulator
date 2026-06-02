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

#include "backend/storage/value_codec.h"

#include <cstdint>
#include <cstring>

#include "zetasql/base/logging.h"
#include <string>
#include <utility>
#include <vector>

#include "zetasql/public/json_value.h"
#include "zetasql/public/numeric_value.h"
#include "zetasql/public/types/type.h"
#include "zetasql/public/value.h"
#include "absl/time/time.h"

namespace google {
namespace spanner {
namespace emulator {
namespace backend {

namespace {

// Type tags for value encoding.
constexpr uint8_t kTagInvalid = 0x00;
constexpr uint8_t kTagNull = 0x01;
constexpr uint8_t kTagBool = 0x02;
constexpr uint8_t kTagInt64 = 0x03;
constexpr uint8_t kTagDouble = 0x04;
constexpr uint8_t kTagString = 0x05;
constexpr uint8_t kTagBytes = 0x06;
constexpr uint8_t kTagTimestamp = 0x07;
constexpr uint8_t kTagDate = 0x08;
constexpr uint8_t kTagNumeric = 0x09;
constexpr uint8_t kTagJson = 0x0A;
constexpr uint8_t kTagArray = 0x0B;
constexpr uint8_t kTagFloat = 0x0C;

// Little-endian encoding helpers for cross-architecture portability.
void AppendInt64LE(std::string* out, int64_t val) {
  uint64_t u;
  std::memcpy(&u, &val, sizeof(u));
  char buf[8];
  buf[0] = static_cast<char>(u & 0xFF);
  buf[1] = static_cast<char>((u >> 8) & 0xFF);
  buf[2] = static_cast<char>((u >> 16) & 0xFF);
  buf[3] = static_cast<char>((u >> 24) & 0xFF);
  buf[4] = static_cast<char>((u >> 32) & 0xFF);
  buf[5] = static_cast<char>((u >> 40) & 0xFF);
  buf[6] = static_cast<char>((u >> 48) & 0xFF);
  buf[7] = static_cast<char>((u >> 56) & 0xFF);
  out->append(buf, 8);
}

void AppendDoubleLE(std::string* out, double val) {
  uint64_t u;
  std::memcpy(&u, &val, sizeof(u));
  char buf[8];
  buf[0] = static_cast<char>(u & 0xFF);
  buf[1] = static_cast<char>((u >> 8) & 0xFF);
  buf[2] = static_cast<char>((u >> 16) & 0xFF);
  buf[3] = static_cast<char>((u >> 24) & 0xFF);
  buf[4] = static_cast<char>((u >> 32) & 0xFF);
  buf[5] = static_cast<char>((u >> 40) & 0xFF);
  buf[6] = static_cast<char>((u >> 48) & 0xFF);
  buf[7] = static_cast<char>((u >> 56) & 0xFF);
  out->append(buf, 8);
}

void AppendInt32LE(std::string* out, int32_t val) {
  uint32_t u;
  std::memcpy(&u, &val, sizeof(u));
  char buf[4];
  buf[0] = static_cast<char>(u & 0xFF);
  buf[1] = static_cast<char>((u >> 8) & 0xFF);
  buf[2] = static_cast<char>((u >> 16) & 0xFF);
  buf[3] = static_cast<char>((u >> 24) & 0xFF);
  out->append(buf, 4);
}

void AppendLengthPrefixedString(std::string* out, const std::string& s) {
  int32_t len = static_cast<int32_t>(s.size());
  AppendInt32LE(out, len);
  out->append(s);
}

int64_t ReadInt64LE(const char* data) {
  const auto* p = reinterpret_cast<const unsigned char*>(data);
  uint64_t u = static_cast<uint64_t>(p[0]) |
               (static_cast<uint64_t>(p[1]) << 8) |
               (static_cast<uint64_t>(p[2]) << 16) |
               (static_cast<uint64_t>(p[3]) << 24) |
               (static_cast<uint64_t>(p[4]) << 32) |
               (static_cast<uint64_t>(p[5]) << 40) |
               (static_cast<uint64_t>(p[6]) << 48) |
               (static_cast<uint64_t>(p[7]) << 56);
  int64_t val;
  std::memcpy(&val, &u, sizeof(val));
  return val;
}

double ReadDoubleLE(const char* data) {
  const auto* p = reinterpret_cast<const unsigned char*>(data);
  uint64_t u = static_cast<uint64_t>(p[0]) |
               (static_cast<uint64_t>(p[1]) << 8) |
               (static_cast<uint64_t>(p[2]) << 16) |
               (static_cast<uint64_t>(p[3]) << 24) |
               (static_cast<uint64_t>(p[4]) << 32) |
               (static_cast<uint64_t>(p[5]) << 40) |
               (static_cast<uint64_t>(p[6]) << 48) |
               (static_cast<uint64_t>(p[7]) << 56);
  double val;
  std::memcpy(&val, &u, sizeof(val));
  return val;
}

int32_t ReadInt32LE(const char* data) {
  const auto* p = reinterpret_cast<const unsigned char*>(data);
  uint32_t u = static_cast<uint32_t>(p[0]) |
               (static_cast<uint32_t>(p[1]) << 8) |
               (static_cast<uint32_t>(p[2]) << 16) |
               (static_cast<uint32_t>(p[3]) << 24);
  int32_t val;
  std::memcpy(&val, &u, sizeof(val));
  return val;
}

const zetasql::Type* TypeForKind(zetasql::TypeKind type_kind) {
  switch (type_kind) {
    case zetasql::TYPE_BOOL:
      return zetasql::types::BoolType();
    case zetasql::TYPE_INT64:
      return zetasql::types::Int64Type();
    case zetasql::TYPE_FLOAT:
      return zetasql::types::FloatType();
    case zetasql::TYPE_DOUBLE:
      return zetasql::types::DoubleType();
    case zetasql::TYPE_STRING:
      return zetasql::types::StringType();
    case zetasql::TYPE_BYTES:
      return zetasql::types::BytesType();
    case zetasql::TYPE_TIMESTAMP:
      return zetasql::types::TimestampType();
    case zetasql::TYPE_DATE:
      return zetasql::types::DateType();
    case zetasql::TYPE_NUMERIC:
      return zetasql::types::NumericType();
    case zetasql::TYPE_JSON:
      return zetasql::types::JsonType();
    default:
      return nullptr;
  }
}

const zetasql::ArrayType* ArrayTypeForElementKind(
    zetasql::TypeKind element_kind) {
  switch (element_kind) {
    case zetasql::TYPE_BOOL:
      return zetasql::types::BoolArrayType();
    case zetasql::TYPE_INT64:
      return zetasql::types::Int64ArrayType();
    case zetasql::TYPE_FLOAT:
      return zetasql::types::FloatArrayType();
    case zetasql::TYPE_DOUBLE:
      return zetasql::types::DoubleArrayType();
    case zetasql::TYPE_STRING:
      return zetasql::types::StringArrayType();
    case zetasql::TYPE_BYTES:
      return zetasql::types::BytesArrayType();
    case zetasql::TYPE_TIMESTAMP:
      return zetasql::types::TimestampArrayType();
    case zetasql::TYPE_DATE:
      return zetasql::types::DateArrayType();
    case zetasql::TYPE_NUMERIC:
      return zetasql::types::NumericArrayType();
    case zetasql::TYPE_JSON:
      return zetasql::types::JsonArrayType();
    default:
      return nullptr;
  }
}

}  // namespace

std::string EncodeValue(const zetasql::Value& value) {
  std::string result;

  if (!value.is_valid()) {
    result.push_back(static_cast<char>(kTagInvalid));
    return result;
  }

  if (value.is_null()) {
    result.push_back(static_cast<char>(kTagNull));
    // Also store the type kind so we can reconstruct the typed null.
    int32_t type_kind = static_cast<int32_t>(value.type_kind());
    AppendInt32LE(&result, type_kind);
    if (value.type_kind() == zetasql::TYPE_ARRAY) {
      int32_t element_kind =
          static_cast<int32_t>(value.type()->AsArray()->element_type()->kind());
      AppendInt32LE(&result, element_kind);
    }
    return result;
  }

  switch (value.type_kind()) {
    case zetasql::TYPE_BOOL: {
      result.push_back(static_cast<char>(kTagBool));
      result.push_back(value.bool_value() ? 1 : 0);
      break;
    }
    case zetasql::TYPE_INT64: {
      result.push_back(static_cast<char>(kTagInt64));
      AppendInt64LE(&result, value.int64_value());
      break;
    }
    case zetasql::TYPE_FLOAT: {
      result.push_back(static_cast<char>(kTagFloat));
      AppendDoubleLE(&result, static_cast<double>(value.float_value()));
      break;
    }
    case zetasql::TYPE_DOUBLE: {
      result.push_back(static_cast<char>(kTagDouble));
      AppendDoubleLE(&result, value.double_value());
      break;
    }
    case zetasql::TYPE_STRING: {
      result.push_back(static_cast<char>(kTagString));
      AppendLengthPrefixedString(&result, value.string_value());
      break;
    }
    case zetasql::TYPE_BYTES: {
      result.push_back(static_cast<char>(kTagBytes));
      AppendLengthPrefixedString(&result, value.bytes_value());
      break;
    }
    case zetasql::TYPE_TIMESTAMP: {
      result.push_back(static_cast<char>(kTagTimestamp));
      absl::Time t = value.ToTime();
      int64_t seconds = absl::ToUnixSeconds(t);
      int32_t nanos = static_cast<int32_t>(
          absl::ToInt64Nanoseconds(t - absl::FromUnixSeconds(seconds)));
      AppendInt64LE(&result, seconds);
      AppendInt32LE(&result, nanos);
      break;
    }
    case zetasql::TYPE_DATE: {
      result.push_back(static_cast<char>(kTagDate));
      AppendInt32LE(&result, value.date_value());
      break;
    }
    case zetasql::TYPE_NUMERIC: {
      result.push_back(static_cast<char>(kTagNumeric));
      std::string serialized = value.numeric_value().SerializeAsProtoBytes();
      AppendLengthPrefixedString(&result, serialized);
      break;
    }
    case zetasql::TYPE_JSON: {
      result.push_back(static_cast<char>(kTagJson));
      std::string json_str = value.json_string();
      AppendLengthPrefixedString(&result, json_str);
      break;
    }
    case zetasql::TYPE_ARRAY: {
      result.push_back(static_cast<char>(kTagArray));
      int32_t element_kind =
          static_cast<int32_t>(value.type()->AsArray()->element_type()->kind());
      AppendInt32LE(&result, element_kind);
      AppendInt32LE(&result, value.num_elements());
      for (int i = 0; i < value.num_elements(); ++i) {
        AppendLengthPrefixedString(&result, EncodeValue(value.element(i)));
      }
      break;
    }
    default: {
      // Unsupported type — fail explicitly rather than silently losing type
      // information via DebugString(). Add support for new types here.
      ABSL_LOG(FATAL) << "Unsupported type in value codec: "
                      << value.type()->DebugString();
      break;
    }
  }

  return result;
}

zetasql::Value DecodeValue(const std::string& encoded) {
  if (encoded.empty()) {
    return zetasql::Value();
  }

  const char* data = encoded.data();
  const size_t size = encoded.size();
  uint8_t tag = static_cast<uint8_t>(data[0]);
  data++;
  const size_t remaining = size - 1;

  switch (tag) {
    case kTagInvalid:
      return zetasql::Value();

    case kTagNull: {
      if (remaining < 4) return zetasql::Value();
      int32_t type_kind = ReadInt32LE(data);
      switch (static_cast<zetasql::TypeKind>(type_kind)) {
        case zetasql::TYPE_BOOL:
          return zetasql::values::NullBool();
        case zetasql::TYPE_INT64:
          return zetasql::values::NullInt64();
        case zetasql::TYPE_FLOAT:
          return zetasql::values::NullFloat();
        case zetasql::TYPE_DOUBLE:
          return zetasql::values::NullDouble();
        case zetasql::TYPE_STRING:
          return zetasql::values::NullString();
        case zetasql::TYPE_BYTES:
          return zetasql::values::NullBytes();
        case zetasql::TYPE_TIMESTAMP:
          return zetasql::values::NullTimestamp();
        case zetasql::TYPE_DATE:
          return zetasql::values::NullDate();
        case zetasql::TYPE_NUMERIC:
          return zetasql::values::NullNumeric();
        case zetasql::TYPE_JSON:
          return zetasql::values::NullJson();
        case zetasql::TYPE_ARRAY: {
          if (remaining < 8) return zetasql::Value();
          zetasql::TypeKind element_kind =
              static_cast<zetasql::TypeKind>(ReadInt32LE(data + 4));
          const zetasql::ArrayType* array_type =
              ArrayTypeForElementKind(element_kind);
          if (array_type == nullptr) return zetasql::Value();
          return zetasql::values::Null(array_type);
        }
        default:
          return zetasql::Value();
      }
    }

    case kTagBool:
      if (remaining < 1) return zetasql::Value();
      return zetasql::values::Bool(data[0] != 0);

    case kTagInt64:
      if (remaining < 8) return zetasql::Value();
      return zetasql::values::Int64(ReadInt64LE(data));

    case kTagFloat:
      if (remaining < 8) return zetasql::Value();
      return zetasql::values::Float(static_cast<float>(ReadDoubleLE(data)));

    case kTagDouble:
      if (remaining < 8) return zetasql::Value();
      return zetasql::values::Double(ReadDoubleLE(data));

    case kTagString: {
      if (remaining < 4) return zetasql::Value();
      int32_t len = ReadInt32LE(data);
      if (len < 0) return zetasql::Value();
      data += sizeof(int32_t);
      if (remaining < 4 + static_cast<size_t>(len)) return zetasql::Value();
      return zetasql::values::String(std::string(data, len));
    }

    case kTagBytes: {
      if (remaining < 4) return zetasql::Value();
      int32_t len = ReadInt32LE(data);
      if (len < 0) return zetasql::Value();
      data += sizeof(int32_t);
      if (remaining < 4 + static_cast<size_t>(len)) return zetasql::Value();
      return zetasql::values::Bytes(std::string(data, len));
    }

    case kTagTimestamp: {
      if (remaining < 12) return zetasql::Value();
      int64_t seconds = ReadInt64LE(data);
      data += sizeof(int64_t);
      int32_t nanos = ReadInt32LE(data);
      absl::Time t =
          absl::FromUnixSeconds(seconds) + absl::Nanoseconds(nanos);
      return zetasql::values::Timestamp(t);
    }

    case kTagDate:
      if (remaining < 4) return zetasql::Value();
      return zetasql::values::Date(ReadInt32LE(data));

    case kTagNumeric: {
      if (remaining < 4) return zetasql::Value();
      int32_t len = ReadInt32LE(data);
      if (len < 0) return zetasql::Value();
      data += sizeof(int32_t);
      if (remaining < 4 + static_cast<size_t>(len)) return zetasql::Value();
      auto status_or =
          zetasql::NumericValue::DeserializeFromProtoBytes(
              absl::string_view(data, len));
      if (status_or.ok()) {
        return zetasql::values::Numeric(status_or.value());
      }
      return zetasql::Value();
    }

    case kTagJson: {
      if (remaining < 4) return zetasql::Value();
      int32_t len = ReadInt32LE(data);
      if (len < 0) return zetasql::Value();
      data += sizeof(int32_t);
      if (remaining < 4 + static_cast<size_t>(len)) return zetasql::Value();
      auto json_value = zetasql::JSONValue::ParseJSONString(
          std::string(data, len));
      if (!json_value.ok()) return zetasql::Value();
      return zetasql::values::Json(std::move(json_value).value());
    }

    case kTagArray: {
      if (remaining < 8) return zetasql::Value();
      zetasql::TypeKind element_kind =
          static_cast<zetasql::TypeKind>(ReadInt32LE(data));
      const zetasql::ArrayType* array_type =
          ArrayTypeForElementKind(element_kind);
      const zetasql::Type* element_type = TypeForKind(element_kind);
      if (array_type == nullptr || element_type == nullptr) {
        return zetasql::Value();
      }
      data += sizeof(int32_t);
      size_t bytes_left = remaining - sizeof(int32_t);

      int32_t num_elements = ReadInt32LE(data);
      if (num_elements < 0) return zetasql::Value();
      data += sizeof(int32_t);
      bytes_left -= sizeof(int32_t);

      std::vector<zetasql::Value> elements;
      elements.reserve(num_elements);
      for (int i = 0; i < num_elements; ++i) {
        if (bytes_left < 4) return zetasql::Value();
        int32_t len = ReadInt32LE(data);
        if (len < 0) return zetasql::Value();
        data += sizeof(int32_t);
        bytes_left -= sizeof(int32_t);
        if (bytes_left < static_cast<size_t>(len)) return zetasql::Value();

        zetasql::Value element = DecodeValue(std::string(data, len));
        if (!element.is_valid()) return zetasql::Value();
        if (!element.is_null() && !element.type()->Equals(element_type)) {
          return zetasql::Value();
        }
        elements.push_back(std::move(element));
        data += len;
        bytes_left -= len;
      }

      auto array_or = zetasql::Value::MakeArray(array_type, elements);
      if (!array_or.ok()) return zetasql::Value();
      return std::move(array_or).value();
    }

    default:
      return zetasql::Value();
  }
}

}  // namespace backend
}  // namespace emulator
}  // namespace spanner
}  // namespace google

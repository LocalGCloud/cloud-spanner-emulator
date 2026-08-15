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

#ifndef THIRD_PARTY_CLOUD_SPANNER_EMULATOR_BACKEND_STORAGE_VALUE_CODEC_H_
#define THIRD_PARTY_CLOUD_SPANNER_EMULATOR_BACKEND_STORAGE_VALUE_CODEC_H_

#include <string>

#include "googlesql/public/value.h"

namespace google {
namespace spanner {
namespace emulator {
namespace backend {

// Serializes a googlesql::Value to a byte string for storage in LevelDB.
// The encoding uses a type prefix byte followed by the value bytes.
// An invalid (unset) value is encoded as a single 0x00 byte.
// A NULL value is encoded as 0x01 followed by a 4-byte LE type kind,
// so that the correct typed null can be reconstructed on decode.
std::string EncodeValue(const googlesql::Value& value);

// Deserializes a googlesql::Value from a byte string.
// The type parameter is used only as a hint to ensure correct decoding.
// If the encoded data indicates an invalid or NULL value, those are returned
// regardless of the type hint.
googlesql::Value DecodeValue(const std::string& encoded);

}  // namespace backend
}  // namespace emulator
}  // namespace spanner
}  // namespace google

#endif  // THIRD_PARTY_CLOUD_SPANNER_EMULATOR_BACKEND_STORAGE_VALUE_CODEC_H_

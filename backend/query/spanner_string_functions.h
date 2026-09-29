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

#ifndef THIRD_PARTY_CLOUD_SPANNER_EMULATOR_BACKEND_QUERY_SPANNER_STRING_FUNCTIONS_H_
#define THIRD_PARTY_CLOUD_SPANNER_EMULATOR_BACKEND_QUERY_SPANNER_STRING_FUNCTIONS_H_

#include <memory>
#include <string>
#include <vector>

#include "googlesql/public/function.h"

namespace google {
namespace spanner {
namespace emulator {
namespace backend {

// Returns Spanner-documented functions whose open-source GoogleSQL reference
// implementations are missing (TO_BASE32, FROM_BASE32) or placeholders
// (ZSTD_COMPRESS, ZSTD_DECOMPRESS_TO_BYTES, ZSTD_DECOMPRESS_TO_STRING). They
// replace the GoogleSQL built-ins of the same names in the function catalog.
std::vector<std::unique_ptr<googlesql::Function>> SpannerStringFunctions(
    const std::string& catalog_name);

}  // namespace backend
}  // namespace emulator
}  // namespace spanner
}  // namespace google

#endif  // THIRD_PARTY_CLOUD_SPANNER_EMULATOR_BACKEND_QUERY_SPANNER_STRING_FUNCTIONS_H_

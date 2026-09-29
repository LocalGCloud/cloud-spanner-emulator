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

#include "backend/stats/spanner_sys_types.h"

#include <vector>

#include "googlesql/public/types/array_type.h"
#include "googlesql/public/types/struct_type.h"
#include "googlesql/public/types/type.h"
#include "googlesql/public/types/type_factory.h"
#include "absl/base/no_destructor.h"
#include "absl/log/check.h"

namespace google {
namespace spanner {
namespace emulator {
namespace backend {

namespace {

// Owns the SPANNER_SYS struct types for the lifetime of the process, so that
// values built by the statistics collector share types with the catalog.
const googlesql::ArrayType* MakeStructArrayType(
    const std::vector<googlesql::StructType::StructField>& fields) {
  static absl::NoDestructor<googlesql::TypeFactory> type_factory;
  const googlesql::StructType* struct_type = nullptr;
  ABSL_CHECK_OK(type_factory->MakeStructType(fields, &struct_type));  // Crash OK
  absl::StatusOr<const googlesql::ArrayType*> array_type =
      type_factory->MakeArrayType(struct_type);
  ABSL_CHECK_OK(array_type.status());  // Crash OK
  return *array_type;
}

}  // namespace

const googlesql::ArrayType* LatencyDistributionType() {
  static const googlesql::ArrayType* type = MakeStructArrayType({
      {"COUNT", googlesql::types::Int64Type()},
      {"MEAN", googlesql::types::DoubleType()},
      {"SUM_OF_SQUARED_DEVIATION", googlesql::types::DoubleType()},
      {"NUM_FINITE_BUCKETS", googlesql::types::Int64Type()},
      {"GROWTH_FACTOR", googlesql::types::DoubleType()},
      {"SCALE", googlesql::types::DoubleType()},
      {"BUCKET_COUNTS", googlesql::types::Int64ArrayType()},
  });
  return type;
}

const googlesql::ArrayType* OperationsByTableType() {
  static const googlesql::ArrayType* type = MakeStructArrayType({
      {"TABLE_NAME", googlesql::types::StringType()},
      {"INSERT_OR_UPDATE_COUNT", googlesql::types::Int64Type()},
      {"INSERT_OR_UPDATE_BYTES", googlesql::types::Int64Type()},
  });
  return type;
}

const googlesql::ArrayType* SampleLockRequestsType() {
  static const googlesql::ArrayType* type = MakeStructArrayType({
      {"COLUMN", googlesql::types::StringType()},
      {"LOCK_MODE", googlesql::types::StringType()},
      {"TRANSACTION_TAG", googlesql::types::StringType()},
  });
  return type;
}

}  // namespace backend
}  // namespace emulator
}  // namespace spanner
}  // namespace google

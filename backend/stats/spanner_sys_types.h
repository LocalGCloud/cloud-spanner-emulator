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

#ifndef THIRD_PARTY_CLOUD_SPANNER_EMULATOR_BACKEND_STATS_SPANNER_SYS_TYPES_H_
#define THIRD_PARTY_CLOUD_SPANNER_EMULATOR_BACKEND_STATS_SPANNER_SYS_TYPES_H_

#include "googlesql/public/types/array_type.h"

namespace google {
namespace spanner {
namespace emulator {
namespace backend {

// The ARRAY<STRUCT<...>> column types of the GoogleSQL SPANNER_SYS tables.
// PostgreSQL databases expose the *_JSON_STRING columns instead.

// LATENCY_DISTRIBUTION and TOTAL_LATENCY_DISTRIBUTION: an exponential-bucket
// histogram in the shape of a Cloud Monitoring Distribution.
const googlesql::ArrayType* LatencyDistributionType();

// OPERATIONS_BY_TABLE of the TXN_STATS tables.
const googlesql::ArrayType* OperationsByTableType();

// SAMPLE_LOCK_REQUESTS of the LOCK_STATS_TOP tables.
const googlesql::ArrayType* SampleLockRequestsType();

}  // namespace backend
}  // namespace emulator
}  // namespace spanner
}  // namespace google

#endif  // THIRD_PARTY_CLOUD_SPANNER_EMULATOR_BACKEND_STATS_SPANNER_SYS_TYPES_H_

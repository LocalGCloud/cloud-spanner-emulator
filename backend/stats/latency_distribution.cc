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

#include "backend/stats/latency_distribution.h"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <string>
#include <utility>
#include <vector>

#include "googlesql/public/value.h"
#include "absl/time/time.h"
#include "backend/stats/spanner_sys_types.h"
#include "nlohmann/json.hpp"

namespace google {
namespace spanner {
namespace emulator {
namespace backend {

LatencyDistribution::LatencyDistribution(int64_t count, double mean,
                                         double sum_of_squared_deviation,
                                         std::vector<int64_t> bucket_counts)
    : count_(count),
      mean_(mean),
      sum_of_squared_deviation_(sum_of_squared_deviation),
      bucket_counts_(std::move(bucket_counts)) {}

void LatencyDistribution::Add(absl::Duration latency) {
  const double seconds = absl::ToDoubleSeconds(latency);

  // Welford's online update of the mean and the sum of squared deviations.
  ++count_;
  const double delta = seconds - mean_;
  mean_ += delta / count_;
  sum_of_squared_deviation_ += delta * (seconds - mean_);

  int64_t bucket = 0;
  if (seconds >= kScale) {
    bucket = std::min<int64_t>(
        kNumFiniteBuckets + 1,
        1 + static_cast<int64_t>(std::floor(std::log(seconds / kScale) /
                                            std::log(kGrowthFactor))));
  }
  if (bucket_counts_.size() <= bucket) {
    bucket_counts_.resize(bucket + 1);
  }
  ++bucket_counts_[bucket];
}

googlesql::Value LatencyDistribution::ToValue() const {
  const googlesql::ArrayType* type = LatencyDistributionType();
  return googlesql::values::Array(
      type,
      {googlesql::Value::Struct(
          type->element_type()->AsStruct(),
          {googlesql::values::Int64(count_), googlesql::values::Double(mean_),
           googlesql::values::Double(sum_of_squared_deviation_),
           googlesql::values::Int64(kNumFiniteBuckets),
           googlesql::values::Double(kGrowthFactor),
           googlesql::values::Double(kScale),
           googlesql::values::Int64Array(bucket_counts_)})});
}

std::string LatencyDistribution::ToJson() const {
  nlohmann::json distribution = {
      {"COUNT", count_},
      {"MEAN", mean_},
      {"SUM_OF_SQUARED_DEVIATION", sum_of_squared_deviation_},
      {"NUM_FINITE_BUCKETS", kNumFiniteBuckets},
      {"GROWTH_FACTOR", kGrowthFactor},
      {"SCALE", kScale},
      {"BUCKET_COUNTS", bucket_counts_},
  };
  return nlohmann::json::array({distribution}).dump();
}

}  // namespace backend
}  // namespace emulator
}  // namespace spanner
}  // namespace google

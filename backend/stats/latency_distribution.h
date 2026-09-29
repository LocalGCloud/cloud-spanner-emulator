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

#ifndef THIRD_PARTY_CLOUD_SPANNER_EMULATOR_BACKEND_STATS_LATENCY_DISTRIBUTION_H_
#define THIRD_PARTY_CLOUD_SPANNER_EMULATOR_BACKEND_STATS_LATENCY_DISTRIBUTION_H_

#include <cstdint>
#include <string>
#include <vector>

#include "googlesql/public/value.h"
#include "absl/time/time.h"

namespace google {
namespace spanner {
namespace emulator {
namespace backend {

// A histogram of latencies with exponential buckets, reported like a Cloud
// Monitoring Distribution: bucket 0 counts latencies below kScale seconds,
// bucket i in [1, kNumFiniteBuckets] counts latencies in
// [kScale * kGrowthFactor^(i-1), kScale * kGrowthFactor^i), and the last
// bucket counts everything larger. Trailing empty buckets are omitted from
// BUCKET_COUNTS, as Cloud Monitoring allows.
class LatencyDistribution {
 public:
  // Bucket boundaries chosen for emulator latencies: 10 microseconds up to
  // about 47 minutes.
  static constexpr double kScale = 1e-5;
  static constexpr double kGrowthFactor = 1.5;
  static constexpr int64_t kNumFiniteBuckets = 48;

  LatencyDistribution() = default;

  // Returns a distribution with the state that the accessors below returned
  // for another distribution, such as one that was saved.
  LatencyDistribution(int64_t count, double mean,
                      double sum_of_squared_deviation,
                      std::vector<int64_t> bucket_counts);

  void Add(absl::Duration latency);

  int64_t count() const { return count_; }
  double mean() const { return mean_; }
  double sum_of_squared_deviation() const { return sum_of_squared_deviation_; }
  const std::vector<int64_t>& bucket_counts() const { return bucket_counts_; }

  // Returns the value of a LATENCY_DISTRIBUTION column: an array holding one
  // struct of type LatencyDistributionType().
  googlesql::Value ToValue() const;

  // Returns the value of a LATENCY_DISTRIBUTION_JSON_STRING column: the JSON
  // form of ToValue(), using the struct field names as keys.
  std::string ToJson() const;

 private:
  int64_t count_ = 0;
  double mean_ = 0;
  double sum_of_squared_deviation_ = 0;
  std::vector<int64_t> bucket_counts_;
};

}  // namespace backend
}  // namespace emulator
}  // namespace spanner
}  // namespace google

#endif  // THIRD_PARTY_CLOUD_SPANNER_EMULATOR_BACKEND_STATS_LATENCY_DISTRIBUTION_H_

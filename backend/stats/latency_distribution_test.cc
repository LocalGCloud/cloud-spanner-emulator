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

#include "gmock/gmock.h"
#include "gtest/gtest.h"
#include "googlesql/public/value.h"
#include "absl/time/time.h"
#include "backend/stats/spanner_sys_types.h"
#include "nlohmann/json.hpp"

namespace google {
namespace spanner {
namespace emulator {
namespace backend {

namespace {

using ::testing::ElementsAre;

TEST(LatencyDistributionTest, CountsLatenciesInExponentialBuckets) {
  LatencyDistribution distribution;
  // Below the scale, in the first finite bucket, and in the third one.
  distribution.Add(absl::Microseconds(5));
  distribution.Add(absl::Microseconds(12));
  distribution.Add(absl::Microseconds(30));

  EXPECT_EQ(distribution.count(), 3);
  EXPECT_THAT(distribution.bucket_counts(), ElementsAre(1, 1, 0, 1));
  EXPECT_NEAR(distribution.mean(), 47e-6 / 3, 1e-12);
}

TEST(LatencyDistributionTest, CountsHugeLatenciesInTheOverflowBucket) {
  LatencyDistribution distribution;
  distribution.Add(absl::Hours(10));
  EXPECT_EQ(distribution.bucket_counts().size(),
            LatencyDistribution::kNumFiniteBuckets + 2);
  EXPECT_EQ(distribution.bucket_counts().back(), 1);
}

TEST(LatencyDistributionTest, TracksTheSumOfSquaredDeviations) {
  LatencyDistribution distribution;
  distribution.Add(absl::Seconds(1));
  distribution.Add(absl::Seconds(3));
  EXPECT_DOUBLE_EQ(distribution.mean(), 2);
  EXPECT_DOUBLE_EQ(distribution.sum_of_squared_deviation(), 2);
}

TEST(LatencyDistributionTest, ConvertsToTheSpannerSysValueAndJson) {
  LatencyDistribution distribution;
  distribution.Add(absl::Microseconds(5));

  const googlesql::Value value = distribution.ToValue();
  EXPECT_EQ(value.type(), LatencyDistributionType());
  ASSERT_EQ(value.num_elements(), 1);
  const googlesql::Value& element = value.element(0);
  EXPECT_EQ(element.field(0).int64_value(), 1);
  EXPECT_DOUBLE_EQ(element.field(1).double_value(), 5e-6);
  EXPECT_EQ(element.field(3).int64_value(),
            LatencyDistribution::kNumFiniteBuckets);
  EXPECT_EQ(element.field(6), googlesql::values::Int64Array({1}));

  const nlohmann::json json = nlohmann::json::parse(distribution.ToJson());
  ASSERT_TRUE(json.is_array());
  EXPECT_EQ(json[0]["COUNT"], 1);
  EXPECT_EQ(json[0]["NUM_FINITE_BUCKETS"],
            LatencyDistribution::kNumFiniteBuckets);
  EXPECT_EQ(json[0]["BUCKET_COUNTS"], nlohmann::json::array({1}));
}

}  // namespace

}  // namespace backend
}  // namespace emulator
}  // namespace spanner
}  // namespace google

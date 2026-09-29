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

#include "backend/stats/operation_stats.h"

#include <string>
#include <vector>

#include "gmock/gmock.h"
#include "gtest/gtest.h"
#include "googlesql/public/value.h"
#include "absl/time/time.h"
#include "backend/access/write.h"
#include "backend/datamodel/key.h"
#include "backend/datamodel/key_set.h"

namespace google {
namespace spanner {
namespace emulator {
namespace backend {

namespace {

using ::googlesql::values::Int64;
using ::googlesql::values::String;
using ::testing::ElementsAre;
using ::testing::Pair;

TEST(LogicalByteSizeTest, CountsTheSizeOfValues) {
  EXPECT_EQ(LogicalByteSize(Int64(1)), 8);
  EXPECT_EQ(LogicalByteSize(String("abc")), 3);
  EXPECT_EQ(LogicalByteSize(googlesql::values::Bool(true)), 1);
  EXPECT_EQ(LogicalByteSize(googlesql::values::NullString()), 0);
  EXPECT_EQ(LogicalByteSize(googlesql::values::Int64Array({1, 2})), 16);
}

TEST(ThreadCpuTimeTest, Advances) {
  const absl::Duration start = ThreadCpuTime();
  volatile int64_t sum = 0;
  for (int i = 0; i < 1000000; ++i) {
    sum = sum + i;
  }
  EXPECT_GT(ThreadCpuTime(), start);
}

TEST(AccessFootprintTest, RecordsMutations) {
  Mutation mutation;
  mutation.AddWriteOp(MutationOpType::kInsert, "Singers", {"Id", "Name"},
                      {{Int64(1), String("ab")}, {Int64(2), String("c")}});
  mutation.AddWriteOp(MutationOpType::kReplace, "Albums", {"Id"},
                      {{Int64(1)}});
  KeySet key_set;
  key_set.AddKey(Key({Int64(3)}));
  mutation.AddDeleteOp("Songs", key_set);

  AccessFootprint footprint;
  footprint.AddMutation(mutation);
  EXPECT_THAT(footprint.written_columns,
              ElementsAre(Pair("Albums", ElementsAre("Id")),
                          Pair("Singers", ElementsAre("Id", "Name"))));
  EXPECT_THAT(footprint.deleted_tables, ElementsAre("Albums", "Songs"));
  EXPECT_EQ(footprint.writes["Singers"].rows, 2);
  EXPECT_EQ(footprint.writes["Singers"].bytes, 19);
  // Written values and deleted keys.
  EXPECT_EQ(footprint.bytes_written, 19 + 8 + 8);
}

TEST(AccessFootprintTest, MergesFootprints) {
  AccessFootprint footprint;
  footprint.AddRead("Singers", {"Name"});
  AccessFootprint other;
  other.AddRead("Singers", {"Id"});
  other.AddRead("Albums", {"Id"});
  footprint.Merge(other);
  EXPECT_THAT(footprint.read_columns,
              ElementsAre(Pair("Albums", ElementsAre("Id")),
                          Pair("Singers", ElementsAre("Id", "Name"))));
  EXPECT_FALSE(footprint.empty());
  EXPECT_TRUE(AccessFootprint().empty());
}

}  // namespace

}  // namespace backend
}  // namespace emulator
}  // namespace spanner
}  // namespace google

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

#include "backend/storage/user_split_point_store.h"

#include <vector>

#include "gmock/gmock.h"
#include "gtest/gtest.h"
#include "googlesql/base/testing/status_matchers.h"
#include "absl/time/time.h"
#include "backend/stats/system_stats_collector.h"
#include "backend/storage/in_memory_storage.h"

namespace google {
namespace spanner {
namespace emulator {
namespace backend {
namespace {

using ::testing::FieldsAre;
using ::testing::UnorderedElementsAre;

TEST(UserSplitPointStoreTest, SavesAndReplacesSplitPoints) {
  InMemoryStorage storage;
  UserSplitPointStore store(&storage);
  EXPECT_THAT(store.LoadAll(), ::googlesql_base::testing::IsOkAndHolds(
                                   ::testing::IsEmpty()));

  const absl::Time expire_time = absl::FromUnixSeconds(1000);
  GOOGLESQL_ASSERT_OK(store.Save({.table_name = "T",
                        .initiator = "load",
                        .split_key = "T(1)",
                        .expire_time = expire_time}));
  GOOGLESQL_ASSERT_OK(store.Save({.table_name = "T",
                        .index_name = "I",
                        .initiator = "load",
                        .split_key = "Index: I on T, Index Key: (a)",
                        .expire_time = expire_time}));
  // Saving the same split point again replaces it.
  GOOGLESQL_ASSERT_OK(store.Save({.table_name = "T",
                        .initiator = "again",
                        .split_key = "T(1)",
                        .expire_time = expire_time + absl::Hours(1)}));

  EXPECT_THAT(
      store.LoadAll(),
      ::googlesql_base::testing::IsOkAndHolds(UnorderedElementsAre(
          FieldsAre("T", "", "again", "T(1)", expire_time + absl::Hours(1)),
          FieldsAre("T", "I", "load", "Index: I on T, Index Key: (a)",
                    expire_time))));
}

}  // namespace
}  // namespace backend
}  // namespace emulator
}  // namespace spanner
}  // namespace google

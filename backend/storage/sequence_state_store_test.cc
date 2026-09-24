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

#include "backend/storage/sequence_state_store.h"

#include <optional>

#include "gmock/gmock.h"
#include "gtest/gtest.h"
#include "googlesql/base/testing/status_matchers.h"
#include "backend/storage/in_memory_storage.h"

namespace google {
namespace spanner {
namespace emulator {
namespace backend {
namespace {

using ::googlesql_base::testing::IsOkAndHolds;

TEST(SequenceStateStoreTest, LoadReturnsNothingWhenUnsaved) {
  InMemoryStorage storage;
  SequenceStateStore store(&storage);
  EXPECT_THAT(store.Load("seq"), IsOkAndHolds(std::nullopt));
}

TEST(SequenceStateStoreTest, LoadReturnsLatestSave) {
  InMemoryStorage storage;
  SequenceStateStore store(&storage);
  GOOGLESQL_ASSERT_OK(store.Save("seq", 1001));
  GOOGLESQL_ASSERT_OK(store.Save("seq", 2001));
  EXPECT_THAT(store.Load("seq"), IsOkAndHolds(2001));
}

TEST(SequenceStateStoreTest, SequencesAreKeptApart) {
  InMemoryStorage storage;
  SequenceStateStore store(&storage);
  GOOGLESQL_ASSERT_OK(store.Save("a", 10));
  GOOGLESQL_ASSERT_OK(store.Save("b", 20));
  EXPECT_THAT(store.Load("a"), IsOkAndHolds(10));
  EXPECT_THAT(store.Load("b"), IsOkAndHolds(20));
}

TEST(SequenceStateStoreTest, RemoveForgetsTheCounter) {
  InMemoryStorage storage;
  SequenceStateStore store(&storage);
  GOOGLESQL_ASSERT_OK(store.Save("seq", 1001));
  GOOGLESQL_ASSERT_OK(store.Remove("seq"));
  EXPECT_THAT(store.Load("seq"), IsOkAndHolds(std::nullopt));

  GOOGLESQL_ASSERT_OK(store.Save("seq", 5));
  EXPECT_THAT(store.Load("seq"), IsOkAndHolds(5));
}

TEST(SequenceStateStoreTest, StateIsSharedThroughStorage) {
  InMemoryStorage storage;
  GOOGLESQL_ASSERT_OK(SequenceStateStore(&storage).Save("seq", 42));
  EXPECT_THAT(SequenceStateStore(&storage).Load("seq"), IsOkAndHolds(42));
}

}  // namespace
}  // namespace backend
}  // namespace emulator
}  // namespace spanner
}  // namespace google

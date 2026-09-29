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

#include "backend/locking/manager.h"

#include <atomic>
#include <cstdint>
#include <functional>
#include <map>
#include <memory>
#include <thread>  // NOLINT
#include <vector>

#include "gmock/gmock.h"
#include "gtest/gtest.h"
#include "googlesql/base/testing/status_matchers.h"
#include "tests/common/proto_matchers.h"
#include "absl/synchronization/notification.h"
#include "absl/time/clock.h"
#include "backend/common/ids.h"
#include "backend/datamodel/key.h"
#include "backend/datamodel/key_range.h"
#include "backend/datamodel/value.h"
#include "common/config.h"

namespace google {
namespace spanner {
namespace emulator {
namespace backend {

namespace {

KeyRange Point(int64_t value) {
  return KeyRange::Point(Key({googlesql::values::Int64(value)}));
}

class LockManagerTest : public testing::Test {
 public:
  LockManagerTest()
      : request_(LockMode::kExclusive, "table", KeyRange::All(), {}) {}

  Clock* clock() { return &clock_; }
  LockManager* manager() { return &manager_; }
  const LockRequest& request() { return request_; }

 private:
  Clock clock_;
  LockManager manager_ = LockManager(&clock_);
  LockRequest request_;
};

TEST_F(LockManagerTest, SingleTransactionAcquiresLock) {
  std::unique_ptr<LockHandle> lh =
      manager()->CreateHandle(TransactionID(1),
                              /*try_abort_fn=*/nullptr, TransactionPriority(1));
  lh->EnqueueLock(request());
  EXPECT_FALSE(lh->IsBlocked());
  GOOGLESQL_EXPECT_OK(lh->Wait());
}

TEST_F(LockManagerTest, ConcurrentTransactionIsAborted) {
  std::unique_ptr<LockHandle> lh1 =
      manager()->CreateHandle(TransactionID(1),
                              /*try_abort_fn=*/nullptr, TransactionPriority(1));
  std::unique_ptr<LockHandle> lh2 =
      manager()->CreateHandle(TransactionID(2),
                              /*try_abort_fn=*/nullptr, TransactionPriority(1));

  // First transaction gets the lock.
  lh1->EnqueueLock(request());
  EXPECT_FALSE(lh1->IsBlocked());
  GOOGLESQL_EXPECT_OK(lh1->Wait());
  EXPECT_FALSE(lh1->IsBlocked());
  EXPECT_FALSE(lh1->IsAborted());

  // Second transaction does not get the lock.
  lh2->EnqueueLock(request());
  EXPECT_FALSE(lh2->IsBlocked());
  EXPECT_TRUE(lh2->IsAborted());
  EXPECT_THAT(lh2->Wait(),
              googlesql_base::testing::StatusIs(absl::StatusCode::kAborted));
  EXPECT_TRUE(lh2->IsAborted());
}

TEST_F(LockManagerTest, ObservesConflicts) {
  struct Conflict {
    TransactionID requester;
    LockMode requested_mode;
    TransactionID holder;
    LockMode held_mode;
    std::vector<ColumnID> held_columns;
  };
  std::vector<Conflict> conflicts;
  manager()->SetConflictObserver(
      [&conflicts](TransactionID requester, const LockRequest& request,
                   TransactionID holder, const LockRequest& held,
                   absl::Duration lock_wait) {
        EXPECT_EQ(lock_wait, absl::ZeroDuration());
        conflicts.push_back({requester, request.mode(), holder, held.mode(),
                             held.column_ids()});
      });
  const int abort_probability = config::abort_current_transaction_probability();
  config::set_abort_current_transaction_probability(0);
  auto reader = manager()->CreateHandle(TransactionID(1), nullptr,
                                        TransactionPriority(1));
  auto other_reader = manager()->CreateHandle(TransactionID(2), nullptr,
                                              TransactionPriority(1));
  auto writer = manager()->CreateHandle(TransactionID(3), nullptr,
                                        TransactionPriority(1));

  // Shared locks don't conflict.
  reader->EnqueueLock({LockMode::kShared, "table", Point(1), {"c1"}});
  writer->EnqueueLock({LockMode::kExclusive, "table", Point(2), {"c1"}});
  EXPECT_TRUE(conflicts.empty());

  // A conflict is observed when it is resolved by aborting the requester, as
  // handles without an abort function never wait.
  writer->EnqueueLock({LockMode::kExclusive, "table", Point(1), {"c2"}});
  EXPECT_TRUE(writer->IsAborted());
  ASSERT_EQ(conflicts.size(), 1);
  EXPECT_EQ(conflicts[0].requester, 3);
  EXPECT_EQ(conflicts[0].requested_mode, LockMode::kExclusive);
  EXPECT_EQ(conflicts[0].holder, 1);
  EXPECT_EQ(conflicts[0].held_mode, LockMode::kShared);
  EXPECT_THAT(conflicts[0].held_columns, testing::ElementsAre("c1"));

  // The aborted writer released its locks, so its former key is free.
  other_reader->EnqueueLock({LockMode::kShared, "table", Point(2), {}});
  EXPECT_FALSE(other_reader->IsAborted());
  EXPECT_EQ(conflicts.size(), 1);

  // An exclusive lock conflicts with a shared request.
  auto second_writer = manager()->CreateHandle(TransactionID(4), nullptr,
                                               TransactionPriority(1));
  second_writer->EnqueueLock({LockMode::kExclusive, "table", Point(3), {}});
  other_reader->EnqueueLock({LockMode::kShared, "table", Point(3), {}});
  ASSERT_EQ(conflicts.size(), 2);
  EXPECT_EQ(conflicts[1].requester, 2);
  EXPECT_EQ(conflicts[1].holder, 4);
  EXPECT_EQ(conflicts[1].held_mode, LockMode::kExclusive);
  config::set_abort_current_transaction_probability(abort_probability);
}

TEST_F(LockManagerTest, DisjointRangesAndTablesCanCommitIndependently) {
  auto first = manager()->CreateHandle(TransactionID(1), nullptr,
                                       TransactionPriority(1));
  auto second = manager()->CreateHandle(TransactionID(2), nullptr,
                                        TransactionPriority(1));
  auto other_table = manager()->CreateHandle(TransactionID(3), nullptr,
                                             TransactionPriority(1));

  first->EnqueueLock({LockMode::kExclusive, "table", Point(1), {}});
  second->EnqueueLock({LockMode::kExclusive, "table", Point(2), {}});
  other_table->EnqueueLock({LockMode::kExclusive, "other", Point(1), {}});
  GOOGLESQL_EXPECT_OK(first->Wait());
  GOOGLESQL_EXPECT_OK(second->Wait());
  GOOGLESQL_EXPECT_OK(other_table->Wait());

  GOOGLESQL_ASSERT_OK_AND_ASSIGN(absl::Time first_commit,
                                 first->ReserveCommitTimestamp());
  std::atomic<bool> second_finished = false;
  absl::StatusOr<absl::Time> second_commit;
  std::thread second_committer([&] {
    second_commit = second->ReserveCommitTimestamp();
    second_finished = true;
  });
  absl::SleepFor(absl::Milliseconds(10));
  EXPECT_FALSE(second_finished);
  absl::Status first_result = first->MarkCommitted();
  first->UnlockAll();
  second_committer.join();
  GOOGLESQL_ASSERT_OK(first_result);
  GOOGLESQL_ASSERT_OK(second_commit.status());
  EXPECT_LT(first_commit, *second_commit);
  GOOGLESQL_EXPECT_OK(second->MarkCommitted());
  second->UnlockAll();
  other_table->UnlockAll();
}

TEST_F(LockManagerTest, OverlappingReadLocksShareButWriteConflicts) {
  auto first = manager()->CreateHandle(TransactionID(1), nullptr,
                                       TransactionPriority(1));
  auto second = manager()->CreateHandle(TransactionID(2), nullptr,
                                        TransactionPriority(1));
  auto writer = manager()->CreateHandle(TransactionID(3), nullptr,
                                        TransactionPriority(1));

  first->EnqueueLock({LockMode::kShared, "table", Point(1), {}});
  second->EnqueueLock({LockMode::kShared, "table", Point(1), {}});
  GOOGLESQL_EXPECT_OK(first->Wait());
  GOOGLESQL_EXPECT_OK(second->Wait());
  writer->EnqueueLock({LockMode::kExclusive, "table", Point(1), {}});
  EXPECT_THAT(writer->Wait(),
              googlesql_base::testing::StatusIs(absl::StatusCode::kAborted));

  first->UnlockAll();
  second->UnlockAll();
  writer->UnlockAll();
  auto later = manager()->CreateHandle(TransactionID(4), nullptr,
                                       TransactionPriority(1));
  later->EnqueueLock({LockMode::kExclusive, "table", Point(1), {}});
  GOOGLESQL_EXPECT_OK(later->Wait());
  later->UnlockAll();
}

TEST_F(LockManagerTest, ReadUpgradeAbortsBusyReaderToMakeProgress) {
  const int original_probability =
      config::abort_current_transaction_probability();
  config::set_abort_current_transaction_probability(0);
  // The older upgrader wounds the reader even while the reader is busy.
  auto upgrader = manager()->CreateHandle(
      TransactionID(1), [] { return absl::OkStatus(); },
      TransactionPriority(1));
  auto reader = manager()->CreateHandle(
      TransactionID(2), [] { return absl::UnavailableError("busy"); },
      TransactionPriority(1));
  upgrader->EnqueueLock({LockMode::kShared, "table", Point(1), {}});
  reader->EnqueueLock({LockMode::kShared, "table", Point(1), {}});
  GOOGLESQL_EXPECT_OK(upgrader->Wait());
  GOOGLESQL_EXPECT_OK(reader->Wait());

  upgrader->EnqueueLock({LockMode::kExclusive, "table", Point(1), {}});
  GOOGLESQL_EXPECT_OK(upgrader->Wait());
  EXPECT_THAT(reader->Wait(),
              googlesql_base::testing::StatusIs(absl::StatusCode::kAborted));
  upgrader->UnlockAll();
  reader->UnlockAll();
  config::set_abort_current_transaction_probability(original_probability);
}

TEST_F(LockManagerTest, AdjacentRangesDoNotConflictButIntersectingRangesDo) {
  auto first = manager()->CreateHandle(TransactionID(1), nullptr,
                                       TransactionPriority(1));
  auto adjacent = manager()->CreateHandle(TransactionID(2), nullptr,
                                          TransactionPriority(1));
  auto overlapping = manager()->CreateHandle(TransactionID(3), nullptr,
                                             TransactionPriority(1));
  first->EnqueueLock({LockMode::kExclusive, "table",
                      KeyRange::ClosedOpen(Key({googlesql::values::Int64(1)}),
                                           Key({googlesql::values::Int64(3)})),
                      {}});
  adjacent->EnqueueLock({LockMode::kExclusive, "table",
                         KeyRange::ClosedOpen(Key({googlesql::values::Int64(3)}),
                                              Key({googlesql::values::Int64(5)})),
                         {}});
  overlapping->EnqueueLock({LockMode::kExclusive, "table", Point(2), {}});
  GOOGLESQL_EXPECT_OK(first->Wait());
  GOOGLESQL_EXPECT_OK(adjacent->Wait());
  EXPECT_THAT(overlapping->Wait(),
              googlesql_base::testing::StatusIs(absl::StatusCode::kAborted));
  first->UnlockAll();
  adjacent->UnlockAll();
  overlapping->UnlockAll();
}

TEST_F(LockManagerTest, PrefixAndUnboundedRangesUseCanonicalEndpoints) {
  auto prefix = manager()->CreateHandle(TransactionID(1), nullptr,
                                        TransactionPriority(1));
  auto same_prefix = manager()->CreateHandle(TransactionID(2), nullptr,
                                             TransactionPriority(1));
  auto other_prefix = manager()->CreateHandle(TransactionID(3), nullptr,
                                              TransactionPriority(1));
  prefix->EnqueueLock({LockMode::kExclusive, "table",
                       KeyRange::Prefix(Key({googlesql::values::Int64(1)})),
                       {}});
  same_prefix->EnqueueLock(
      {LockMode::kExclusive, "table",
       KeyRange::Point(Key({googlesql::values::Int64(1),
                            googlesql::values::Int64(9)})),
       {}});
  other_prefix->EnqueueLock(
      {LockMode::kExclusive, "table",
       KeyRange::Point(Key({googlesql::values::Int64(2),
                            googlesql::values::Int64(9)})),
       {}});
  GOOGLESQL_EXPECT_OK(prefix->Wait());
  EXPECT_THAT(same_prefix->Wait(),
              googlesql_base::testing::StatusIs(absl::StatusCode::kAborted));
  GOOGLESQL_EXPECT_OK(other_prefix->Wait());
  prefix->UnlockAll();
  same_prefix->UnlockAll();
  other_prefix->UnlockAll();

  auto all = manager()->CreateHandle(TransactionID(4), nullptr,
                                     TransactionPriority(1));
  auto point = manager()->CreateHandle(TransactionID(5), nullptr,
                                       TransactionPriority(1));
  all->EnqueueLock({LockMode::kExclusive, "table", KeyRange::All(), {}});
  point->EnqueueLock({LockMode::kExclusive, "table", Point(999), {}});
  EXPECT_THAT(point->Wait(),
              googlesql_base::testing::StatusIs(absl::StatusCode::kAborted));
}

TEST_F(LockManagerTest, SchemaLockConflictsAcrossTables) {
  auto writer = manager()->CreateHandle(TransactionID(1), nullptr,
                                        TransactionPriority(1));
  auto schema = manager()->CreateHandle(TransactionID(2), nullptr,
                                        TransactionPriority(1));
  writer->EnqueueLock({LockMode::kExclusive, "table", Point(1), {}});
  schema->EnqueueLock({LockMode::kExclusive, "", KeyRange::All(), {}});
  EXPECT_THAT(schema->Wait(),
              googlesql_base::testing::StatusIs(absl::StatusCode::kAborted));
  writer->UnlockAll();
  schema->UnlockAll();

  auto schema_again = manager()->CreateHandle(TransactionID(3), nullptr,
                                              TransactionPriority(1));
  auto other_table = manager()->CreateHandle(TransactionID(4), nullptr,
                                             TransactionPriority(1));
  schema_again->EnqueueLock({LockMode::kExclusive, "", KeyRange::All(), {}});
  other_table->EnqueueLock({LockMode::kExclusive, "other", Point(1), {}});
  GOOGLESQL_EXPECT_OK(schema_again->Wait());
  EXPECT_THAT(other_table->Wait(),
              googlesql_base::testing::StatusIs(absl::StatusCode::kAborted));
  schema_again->UnlockAll();
  other_table->UnlockAll();
}

TEST_F(LockManagerTest, WoundingReleasesAllRangesOfPreviousHolder) {
  const int original_probability = config::abort_current_transaction_probability();
  config::set_abort_current_transaction_probability(100);
  auto first = manager()->CreateHandle(TransactionID(1),
                                       [] { return absl::OkStatus(); },
                                       TransactionPriority(1));
  auto second = manager()->CreateHandle(TransactionID(2), nullptr,
                                        TransactionPriority(1));
  auto third = manager()->CreateHandle(TransactionID(3), nullptr,
                                       TransactionPriority(1));
  first->EnqueueLock({LockMode::kExclusive, "table", Point(1), {}});
  first->EnqueueLock({LockMode::kExclusive, "table", Point(2), {}});
  second->EnqueueLock({LockMode::kExclusive, "table", Point(1), {}});
  GOOGLESQL_EXPECT_OK(second->Wait());
  EXPECT_THAT(first->Wait(),
              googlesql_base::testing::StatusIs(absl::StatusCode::kAborted));
  third->EnqueueLock({LockMode::kExclusive, "table", Point(2), {}});
  GOOGLESQL_EXPECT_OK(third->Wait());
  first->UnlockAll();
  second->UnlockAll();
  third->UnlockAll();
  config::set_abort_current_transaction_probability(original_probability);
}

TEST_F(LockManagerTest, DeadlockAbortsOnlyTheRequester) {
  auto first = manager()->CreateHandle(TransactionID(1), nullptr,
                                       TransactionPriority(1));
  auto second = manager()->CreateHandle(TransactionID(2), nullptr,
                                        TransactionPriority(1));
  first->EnqueueLock({LockMode::kExclusive, "table", Point(1), {}});
  second->EnqueueLock({LockMode::kExclusive, "table", Point(2), {}});
  GOOGLESQL_EXPECT_OK(first->Wait());
  GOOGLESQL_EXPECT_OK(second->Wait());

  // Each transaction now wants the key the other holds. The first requester
  // aborts, and the locks it held no longer block the other transaction even
  // before it releases them itself.
  first->EnqueueLock({LockMode::kExclusive, "table", Point(2), {}});
  EXPECT_THAT(first->Wait(),
              googlesql_base::testing::StatusIs(absl::StatusCode::kAborted));
  second->EnqueueLock({LockMode::kExclusive, "table", Point(1), {}});
  GOOGLESQL_EXPECT_OK(second->Wait());

  // The aborted transaction can take locks again once it has reset.
  second->UnlockAll();
  first->UnlockAll();
  first->EnqueueLock({LockMode::kExclusive, "table", Point(1), {}});
  first->EnqueueLock({LockMode::kExclusive, "table", Point(2), {}});
  GOOGLESQL_EXPECT_OK(first->Wait());
  first->UnlockAll();
}

// Configures lock conflicts for a test and restores the flags afterwards.
class ScopedLockConflictConfig {
 public:
  ScopedLockConflictConfig(int abort_probability, absl::Duration timeout)
      : abort_probability_(config::abort_current_transaction_probability()),
        timeout_(config::lock_wait_timeout()) {
    config::set_abort_current_transaction_probability(abort_probability);
    config::set_lock_wait_timeout_ms(absl::ToInt64Milliseconds(timeout));
  }
  ~ScopedLockConflictConfig() {
    config::set_abort_current_transaction_probability(abort_probability_);
    config::set_lock_wait_timeout_ms(absl::ToInt64Milliseconds(timeout_));
  }

 private:
  int abort_probability_;
  absl::Duration timeout_;
};

// Returns an abort function of a transaction that is idle, or of one that is
// busy executing a request.
std::function<absl::Status()> IdleTransaction() {
  return [] { return absl::OkStatus(); };
}
std::function<absl::Status()> BusyTransaction() {
  return [] { return absl::UnavailableError("busy"); };
}

TEST_F(LockManagerTest, YoungerTransactionWaitsUntilOlderReleasesLock) {
  ScopedLockConflictConfig lock_config(0, absl::Seconds(60));
  absl::Duration observed_wait;
  manager()->SetConflictObserver(
      [&observed_wait](TransactionID requester, const LockRequest& request,
                       TransactionID holder, const LockRequest& held,
                       absl::Duration lock_wait) {
        if (requester == 2 && holder == 1) {
          observed_wait = lock_wait;
        }
      });
  auto older = manager()->CreateHandle(TransactionID(1), IdleTransaction(),
                                       TransactionPriority(1));
  auto younger = manager()->CreateHandle(TransactionID(2), IdleTransaction(),
                                         TransactionPriority(2));
  older->EnqueueLock({LockMode::kExclusive, "table", Point(1), {}});
  GOOGLESQL_ASSERT_OK(older->Wait());

  younger->EnqueueLock({LockMode::kExclusive, "table", Point(1), {}});
  // Later requests queue behind the waiting one, even if they don't conflict.
  younger->EnqueueLock({LockMode::kExclusive, "table", Point(2), {}});
  EXPECT_TRUE(younger->IsBlocked());
  EXPECT_FALSE(younger->IsAborted());
  absl::Status younger_status;
  std::thread waiter([&] { younger_status = younger->Wait(); });
  absl::SleepFor(absl::Milliseconds(20));
  EXPECT_TRUE(younger->IsBlocked());
  EXPECT_FALSE(older->IsAborted());

  older->UnlockAll();
  waiter.join();
  GOOGLESQL_EXPECT_OK(younger_status);
  EXPECT_FALSE(younger->IsBlocked());
  EXPECT_GE(observed_wait, absl::Milliseconds(20));

  // The younger transaction holds both locks now.
  auto later = manager()->CreateHandle(TransactionID(3), nullptr,
                                       TransactionPriority(3));
  later->EnqueueLock({LockMode::kShared, "table", Point(2), {}});
  EXPECT_THAT(later->Wait(),
              googlesql_base::testing::StatusIs(absl::StatusCode::kAborted));
}

TEST_F(LockManagerTest, OlderTransactionWoundsYoungerHolder) {
  ScopedLockConflictConfig lock_config(0, absl::Seconds(60));
  auto younger = manager()->CreateHandle(TransactionID(1), IdleTransaction(),
                                         TransactionPriority(2));
  auto older = manager()->CreateHandle(TransactionID(2), IdleTransaction(),
                                       TransactionPriority(1));
  younger->EnqueueLock({LockMode::kExclusive, "table", Point(1), {}});
  GOOGLESQL_ASSERT_OK(younger->Wait());

  older->EnqueueLock({LockMode::kExclusive, "table", Point(1), {}});
  EXPECT_FALSE(older->IsBlocked());
  GOOGLESQL_EXPECT_OK(older->Wait());
  EXPECT_THAT(younger->Wait(),
              googlesql_base::testing::StatusIs(absl::StatusCode::kAborted));
}

TEST_F(LockManagerTest, OlderTransactionWoundsWaitingYoungerTransaction) {
  ScopedLockConflictConfig lock_config(0, absl::Seconds(60));
  auto older = manager()->CreateHandle(TransactionID(1), BusyTransaction(),
                                       TransactionPriority(1));
  auto younger = manager()->CreateHandle(TransactionID(2), BusyTransaction(),
                                         TransactionPriority(2));
  older->EnqueueLock({LockMode::kExclusive, "table", Point(1), {}});
  younger->EnqueueLock({LockMode::kExclusive, "table", Point(2), {}});
  GOOGLESQL_ASSERT_OK(older->Wait());
  GOOGLESQL_ASSERT_OK(younger->Wait());

  // The younger transaction waits for the older one's key while the older one
  // requests the younger one's key. Instead of deadlocking, the older
  // transaction wounds the younger one although it is busy waiting.
  younger->EnqueueLock({LockMode::kExclusive, "table", Point(1), {}});
  absl::Status younger_status;
  std::thread waiter([&] { younger_status = younger->Wait(); });
  older->EnqueueLock({LockMode::kExclusive, "table", Point(2), {}});
  GOOGLESQL_EXPECT_OK(older->Wait());
  waiter.join();
  EXPECT_THAT(younger_status,
              googlesql_base::testing::StatusIs(absl::StatusCode::kAborted));
  EXPECT_FALSE(younger->IsBlocked());
}

TEST_F(LockManagerTest, WaitingTransactionAbortsAfterTimeout) {
  ScopedLockConflictConfig lock_config(0, absl::Milliseconds(50));
  auto older = manager()->CreateHandle(TransactionID(1), IdleTransaction(),
                                       TransactionPriority(1));
  auto younger = manager()->CreateHandle(TransactionID(2), IdleTransaction(),
                                         TransactionPriority(2));
  older->EnqueueLock({LockMode::kExclusive, "table", Point(1), {}});
  younger->EnqueueLock({LockMode::kExclusive, "table", Point(2), {}});
  GOOGLESQL_ASSERT_OK(older->Wait());
  GOOGLESQL_ASSERT_OK(younger->Wait());

  younger->EnqueueLock({LockMode::kExclusive, "table", Point(1), {}});
  const absl::Time start = absl::Now();
  EXPECT_THAT(younger->Wait(),
              googlesql_base::testing::StatusIs(
                  absl::StatusCode::kAborted,
                  testing::HasSubstr("aborted after waiting 50ms")));
  EXPECT_GE(absl::Now() - start, absl::Milliseconds(50));
  EXPECT_FALSE(older->IsAborted());

  // The aborted transaction released its locks.
  auto other = manager()->CreateHandle(TransactionID(3), nullptr,
                                       TransactionPriority(3));
  other->EnqueueLock({LockMode::kExclusive, "table", Point(2), {}});
  GOOGLESQL_EXPECT_OK(other->Wait());
}

TEST_F(LockManagerTest, ZeroLockWaitTimeoutAbortsYoungerRequester) {
  ScopedLockConflictConfig lock_config(0, absl::ZeroDuration());
  auto older = manager()->CreateHandle(TransactionID(1), IdleTransaction(),
                                       TransactionPriority(1));
  auto younger = manager()->CreateHandle(TransactionID(2), IdleTransaction(),
                                         TransactionPriority(2));
  older->EnqueueLock({LockMode::kExclusive, "table", Point(1), {}});
  younger->EnqueueLock({LockMode::kExclusive, "table", Point(1), {}});
  EXPECT_FALSE(younger->IsBlocked());
  EXPECT_THAT(younger->Wait(),
              googlesql_base::testing::StatusIs(absl::StatusCode::kAborted));
  GOOGLESQL_EXPECT_OK(older->Wait());
}

TEST_F(LockManagerTest, SequentialTransactionAcquiresLock) {
  std::unique_ptr<LockHandle> lh1 =
      manager()->CreateHandle(TransactionID(1),
                              /*try_abort_fn=*/nullptr, TransactionPriority(1));
  std::unique_ptr<LockHandle> lh2 =
      manager()->CreateHandle(TransactionID(2),
                              /*try_abort_fn=*/nullptr, TransactionPriority(1));
  std::unique_ptr<LockHandle> lh3 =
      manager()->CreateHandle(TransactionID(3),
                              /*try_abort_fn=*/nullptr, TransactionPriority(1));

  // First transaction gets the lock.
  lh1->EnqueueLock(request());
  GOOGLESQL_EXPECT_OK(lh1->Wait());

  // Second transaction does not get the lock yet.
  lh2->EnqueueLock(request());
  EXPECT_THAT(lh2->Wait(),
              googlesql_base::testing::StatusIs(absl::StatusCode::kAborted));

  // First transaction unlocks.
  lh1->UnlockAll();

  // Second transaction is still in a final aborted state.
  EXPECT_THAT(lh2->Wait(),
              googlesql_base::testing::StatusIs(absl::StatusCode::kAborted));

  // Now another transaction can get the lock.
  lh3->EnqueueLock(request());
  GOOGLESQL_EXPECT_OK(lh3->Wait());
}

TEST_F(LockManagerTest, TransactionsThatDidNotAcquireLockCanReleaseIt) {
  std::unique_ptr<LockHandle> lh1 =
      manager()->CreateHandle(TransactionID(1),
                              /*try_abort_fn=*/nullptr, TransactionPriority(1));
  lh1->UnlockAll();

  std::unique_ptr<LockHandle> lh2 =
      manager()->CreateHandle(TransactionID(1),
                              /*try_abort_fn=*/nullptr, TransactionPriority(1));
  lh2->EnqueueLock(request());
  EXPECT_FALSE(lh2->IsBlocked());
  GOOGLESQL_EXPECT_OK(lh2->Wait());
}

TEST_F(LockManagerTest, DestroyingHandleReleasesItsLocks) {
  auto first = manager()->CreateHandle(TransactionID(1), nullptr,
                                       TransactionPriority(1));
  first->EnqueueLock({LockMode::kExclusive, "table", Point(1), {}});
  GOOGLESQL_EXPECT_OK(first->Wait());
  first.reset();

  auto second = manager()->CreateHandle(TransactionID(2), nullptr,
                                        TransactionPriority(1));
  second->EnqueueLock({LockMode::kExclusive, "table", Point(1), {}});
  GOOGLESQL_EXPECT_OK(second->Wait());
}

TEST_F(LockManagerTest, UnlockReleasesAbandonedCommitReservation) {
  auto first = manager()->CreateHandle(TransactionID(1), nullptr,
                                       TransactionPriority(1));
  auto second = manager()->CreateHandle(TransactionID(2), nullptr,
                                        TransactionPriority(1));
  GOOGLESQL_ASSERT_OK(first->ReserveCommitTimestamp().status());

  std::atomic<bool> second_finished = false;
  absl::StatusOr<absl::Time> second_timestamp;
  std::thread waiter([&] {
    second_timestamp = second->ReserveCommitTimestamp();
    second_finished = true;
  });
  absl::SleepFor(absl::Milliseconds(10));
  EXPECT_FALSE(second_finished);
  first->UnlockAll();
  waiter.join();

  GOOGLESQL_ASSERT_OK(second_timestamp.status());
  GOOGLESQL_EXPECT_OK(second->MarkCommitted());
  second->UnlockAll();
}

TEST_F(LockManagerTest, EnsuresSerializationWithParallelTransactions) {
  // Simulate a thread-safe mvcc store with a single key. Even though multiple
  // threads access this store, they are synchronized by the lock manager.
  // Concurrent access issues are expected to be caught by tsan.
  std::map<absl::Time, int> value{{absl::InfinitePast(), 0}};
  auto SetValue = [&value](absl::Time t, int i) { value[t] = i; };
  auto GetValue = [&value](absl::Time t) {
    auto itr = value.upper_bound(t);
    --itr;
    return itr->second;
  };

  // Start n threads each doing a transactional increment k times.
  int n = 20;
  int k = 10;
  std::vector<std::thread> threads;
  std::atomic<int> id_counter(0);
  for (int i = 0; i < n; ++i) {
    threads.emplace_back(
        [&](int i) {
          for (int j = 0; j < k; ++j) {
            while (true) {
              // Get a lock.
              std::unique_ptr<LockHandle> lh = manager()->CreateHandle(
                  TransactionID(++id_counter),
                  /*try_abort_fn=*/nullptr, TransactionPriority(1));
              lh->EnqueueLock(request());
              absl::Status status = lh->Wait();

              // Retry on aborts.
              if (status.code() == absl::StatusCode::kAborted) {
                continue;
              } else {
                GOOGLESQL_ASSERT_OK(status);
              }

              // We got the lock, increment the counter.
              int cur_value = GetValue(absl::InfiniteFuture());
              int new_value = cur_value + 1;
              SetValue(clock()->Now(), new_value);

              // Unlock the lock.
              lh->UnlockAll();
              break;
            }
          }
        },
        i);
  }

  // Wait for all threads to complete.
  for (std::thread& thread : threads) {
    thread.join();
  }

  // Expect that the counter was incremented n*k times.
  EXPECT_EQ(n * k, GetValue(absl::InfiniteFuture()));
}

TEST_F(LockManagerTest, SerializesActionsBetweenCommitTimestamps) {
  std::unique_ptr<LockHandle> first =
      manager()->CreateHandle(TransactionID(1),
                              /*try_abort_fn=*/nullptr, TransactionPriority(1));
  GOOGLESQL_ASSERT_OK_AND_ASSIGN(const absl::Time first_timestamp,
                                 first->ReserveCommitTimestamp());

  std::atomic<bool> first_action_started = false;
  absl::StatusOr<absl::Time> first_action_timestamp;
  std::thread first_action([&] {
    first_action_timestamp = manager()->RunWithCommitSerialization([&] {
      first_action_started = true;
      return absl::OkStatus();
    });
  });
  absl::SleepFor(absl::Milliseconds(10));
  EXPECT_FALSE(first_action_started);
  GOOGLESQL_EXPECT_OK(first->MarkCommitted());
  first_action.join();
  GOOGLESQL_ASSERT_OK(first_action_timestamp.status());
  EXPECT_LT(first_timestamp, *first_action_timestamp);
  first->UnlockAll();

  absl::Notification action_started;
  absl::Notification release_action;
  absl::StatusOr<absl::Time> serialized_timestamp;
  std::thread serialized_action([&] {
    serialized_timestamp = manager()->RunWithCommitSerialization([&] {
      action_started.Notify();
      release_action.WaitForNotification();
      return absl::OkStatus();
    });
  });
  action_started.WaitForNotification();

  std::atomic<bool> later_commit_finished = false;
  absl::StatusOr<absl::Time> later_commit_timestamp;
  std::unique_ptr<LockHandle> later =
      manager()->CreateHandle(TransactionID(2),
                              /*try_abort_fn=*/nullptr, TransactionPriority(1));
  std::thread later_commit([&] {
    later_commit_timestamp = later->ReserveCommitTimestamp();
    later_commit_finished = true;
  });
  absl::SleepFor(absl::Milliseconds(10));
  EXPECT_FALSE(later_commit_finished);
  release_action.Notify();
  serialized_action.join();
  later_commit.join();

  GOOGLESQL_ASSERT_OK(serialized_timestamp.status());
  GOOGLESQL_ASSERT_OK(later_commit_timestamp.status());
  EXPECT_LT(*serialized_timestamp, *later_commit_timestamp);
  GOOGLESQL_EXPECT_OK(later->MarkCommitted());
  later->UnlockAll();
}

}  // namespace

}  // namespace backend
}  // namespace emulator
}  // namespace spanner
}  // namespace google

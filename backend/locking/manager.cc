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

#include <algorithm>
#include <functional>
#include <memory>

#include "absl/memory/memory.h"
#include "absl/random/random.h"
#include "absl/random/uniform_int_distribution.h"
#include "absl/status/status.h"
#include "absl/status/statusor.h"
#include "absl/strings/substitute.h"
#include "absl/synchronization/mutex.h"
#include "absl/time/time.h"
#include "backend/common/ids.h"
#include "common/config.h"
#include "common/errors.h"
#include "googlesql/base/ret_check.h"

namespace google {
namespace spanner {
namespace emulator {
namespace backend {

std::unique_ptr<LockHandle> LockManager::CreateHandle(
    TransactionID tid, const std::function<absl::Status()>& abort_fn,
    TransactionPriority priority) {
  return absl::WrapUnique(new LockHandle(this, tid, abort_fn, priority));
}

namespace {

// Returns a lock in `held` that conflicts with `request`, or nullptr. Shared
// locks conflict only with exclusive requests.
const LockRequest* FindConflict(const std::vector<LockRequest>& shared,
                                const std::vector<LockRequest>& exclusive,
                                const LockRequest& request) {
  auto conflicts = [&request](const LockRequest& held) {
    return held.ConflictsWith(request);
  };
  auto it = std::find_if(exclusive.begin(), exclusive.end(), conflicts);
  if (it != exclusive.end()) {
    return &*it;
  }
  if (request.mode() == LockMode::kExclusive) {
    it = std::find_if(shared.begin(), shared.end(), conflicts);
    if (it != shared.end()) {
      return &*it;
    }
  }
  return nullptr;
}

// Returns true if the transaction of `requester` is older than that of
// `holder`, so that it takes priority under wound-wait.
bool IsOlder(LockHandle* requester, LockHandle* holder) {
  return std::make_pair(requester->priority(), requester->tid()) <
         std::make_pair(holder->priority(), holder->tid());
}

// Draws whether a request tries to abort the holder of a conflicting lock
// although it has no priority over it.
bool ShouldWoundWithoutPriority() {
  absl::BitGen gen;
  return absl::uniform_int_distribution<int>(1, 100)(gen) <=
         config::abort_current_transaction_probability();
}

}  // namespace

void LockManager::EnqueueLock(LockHandle* handle, const LockRequest& request) {
  absl::MutexLock lock(mu_);

  // Don't hand out locks to aborted handles.
  if (handle->IsAborted()) {
    return;
  }

  // Requests are granted in order, so a request queues behind one that waits.
  auto waiter = waiters_.find(handle);
  if (waiter != waiters_.end()) {
    waiter->second.requests.push_back(request);
    return;
  }
  Waiter new_waiter;
  if (Acquire(handle, request, new_waiter) == Acquisition::kWaiting) {
    new_waiter.requests.push_back(request);
    waiters_.emplace(handle, std::move(new_waiter));
  }
}

LockManager::Acquisition LockManager::Acquire(LockHandle* handle,
                                              const LockRequest& request,
                                              Waiter& waiter) {
  for (auto it = held_locks_.begin(); it != held_locks_.end();) {
    LockHandle* holder = it->first;
    const LockRequest* held =
        holder == handle
            ? nullptr
            : FindConflict(it->second.shared, it->second.exclusive, request);
    if (held == nullptr) {
      ++it;
      continue;
    }
    // A request keeps waiting for the holder it waits for.
    if (waiter.held.has_value() && waiter.holder == holder->tid()) {
      return Acquisition::kWaiting;
    }
    EndWait(handle, request, waiter);

    const TransactionID holder_id = holder->tid();
    switch (ResolveConflict(handle, holder)) {
      case Resolution::kWound:
        if (conflict_observer_ != nullptr) {
          conflict_observer_(handle->tid(), request, holder_id, *held,
                             absl::ZeroDuration());
        }
        held_locks_.erase(holder);
        locks_released_cvar_.SignalAll();
        it = held_locks_.begin();
        continue;
      case Resolution::kWait: {
        const absl::Time now = absl::Now();
        if (!waiter.deadline.has_value()) {
          waiter.deadline = now + config::lock_wait_timeout();
        }
        waiter.holder = holder_id;
        waiter.wait_start = now;
        waiter.held = *held;
        return Acquisition::kWaiting;
      }
      case Resolution::kAbortRequester:
        if (conflict_observer_ != nullptr) {
          conflict_observer_(handle->tid(), request, holder_id, *held,
                             absl::ZeroDuration());
        }
        AbortRequester(handle, error::AbortConcurrentTransaction(
                                   handle->tid(), holder_id));
        return Acquisition::kAborted;
    }
  }
  EndWait(handle, request, waiter);
  HeldLocks& own = held_locks_[handle];
  if (request.mode() == LockMode::kShared) {
    own.shared.push_back(request);
  } else {
    own.exclusive.push_back(request);
  }
  return Acquisition::kGranted;
}

LockManager::Resolution LockManager::ResolveConflict(LockHandle* requester,
                                                     LockHandle* holder) {
  // A handle that cannot be wounded must not be waited for either, or an
  // older transaction could wait for it while it waits for that transaction.
  if (!holder->IsAbortable()) {
    return Resolution::kAbortRequester;
  }
  const absl::Status wound_status =
      error::AbortCurrentTransaction(holder->tid(), requester->tid());
  const bool can_wound = holder != committing_handle_;
  if (!requester->IsAbortable()) {
    return can_wound && ShouldWoundWithoutPriority() &&
                   holder->TryAbortTransaction(wound_status).ok()
               ? Resolution::kWound
               : Resolution::kAbortRequester;
  }
  if (can_wound) {
    // An older transaction wounds the holder even while the holder executes a
    // request, which may itself wait for a lock.
    if (IsOlder(requester, holder)) {
      if (holder->TryAbortTransaction(wound_status).ok() ||
          holder->ForceAbortTransaction(wound_status)) {
        return Resolution::kWound;
      }
    } else if (ShouldWoundWithoutPriority() &&
               holder->TryAbortTransaction(wound_status).ok()) {
      return Resolution::kWound;
    }
  }
  return config::lock_wait_timeout() > absl::ZeroDuration()
             ? Resolution::kWait
             : Resolution::kAbortRequester;
}

bool LockManager::GrantWaitingRequests(LockHandle* handle) {
  auto it = waiters_.find(handle);
  if (it == waiters_.end()) {
    return true;
  }
  Waiter& waiter = it->second;
  while (!waiter.requests.empty()) {
    switch (Acquire(handle, waiter.requests.front(), waiter)) {
      case Acquisition::kGranted:
        waiter.requests.pop_front();
        waiter.deadline.reset();
        continue;
      case Acquisition::kWaiting:
        return false;
      case Acquisition::kAborted:
        waiters_.erase(it);
        return true;
    }
  }
  waiters_.erase(it);
  return true;
}

void LockManager::EndWait(LockHandle* handle, const LockRequest& request,
                          Waiter& waiter) {
  if (!waiter.held.has_value()) {
    return;
  }
  if (conflict_observer_ != nullptr) {
    conflict_observer_(handle->tid(), request, waiter.holder, *waiter.held,
                       absl::Now() - waiter.wait_start);
  }
  waiter.held.reset();
}

void LockManager::AbortRequester(LockHandle* handle,
                                 const absl::Status& status) {
  // An aborted transaction can no longer commit. Release its locks now
  // instead of when it resets, or a transaction that deadlocked with it can
  // still conflict with them and abort as well.
  held_locks_.erase(handle);
  locks_released_cvar_.SignalAll();
  handle->Abort(status);
}

bool LockManager::IsWaiting(LockHandle* handle) {
  absl::MutexLock lock(mu_);
  return !GrantWaitingRequests(handle);
}

void LockManager::WaitForLocks(LockHandle* handle) {
  absl::MutexLock lock(mu_);
  while (true) {
    auto it = waiters_.find(handle);
    if (it == waiters_.end()) {
      return;
    }
    Waiter& waiter = it->second;
    // Another transaction wounded this one while it waited.
    if (handle->IsAborted()) {
      EndWait(handle, waiter.requests.front(), waiter);
      waiters_.erase(it);
      return;
    }
    if (GrantWaitingRequests(handle)) {
      return;
    }
    if (absl::Now() >= *waiter.deadline) {
      const TransactionID holder = waiter.holder;
      EndWait(handle, waiter.requests.front(), waiter);
      waiters_.erase(it);
      AbortRequester(handle,
                     error::LockWaitTimeout(handle->tid(), holder,
                                            config::lock_wait_timeout()));
      return;
    }
    locks_released_cvar_.WaitWithDeadline(&mu_, *waiter.deadline);
  }
}

void LockManager::UnlockAll(LockHandle* handle) {
  absl::MutexLock lock(mu_);

  auto waiter = waiters_.find(handle);
  if (waiter != waiters_.end()) {
    EndWait(handle, waiter->second.requests.front(), waiter->second);
    waiters_.erase(waiter);
  }
  if (held_locks_.erase(handle) > 0) {
    locks_released_cvar_.SignalAll();
  }
  if (snapshots_.erase(handle) > 0) {
    PruneCommittedWrites();
  }
  if (committing_handle_ == handle) {
    committing_handle_ = nullptr;
    pending_commit_timestamp_ = absl::InfiniteFuture();
    pending_commit_cvar_.SignalAll();
  }
  handle->Reset();
}

absl::StatusOr<absl::Time> LockManager::ReserveCommitTimestamp(
    LockHandle* handle) {
  absl::MutexLock lock(mu_);

  absl::Status status = handle->status();
  if (!status.ok()) return status;
  while (committing_handle_ != nullptr && committing_handle_ != handle) {
    pending_commit_cvar_.Wait(&mu_);
    status = handle->status();
    if (!status.ok()) return status;
  }
  if (committing_handle_ == handle) return pending_commit_timestamp_;
  committing_handle_ = handle;
  pending_commit_timestamp_ = clock_->Now();
  return pending_commit_timestamp_;
}

absl::Status LockManager::MarkCommitted(LockHandle* handle) {
  absl::MutexLock lock(mu_);

  GOOGLESQL_RET_CHECK_EQ(committing_handle_, handle)
      << absl::Substitute("Transaction $0 has no reserved commit timestamp.",
                          handle->tid());

  last_commit_timestamp_ = pending_commit_timestamp_;
  pending_commit_timestamp_ = absl::InfiniteFuture();
  committing_handle_ = nullptr;
  pending_commit_cvar_.SignalAll();
  return absl::OkStatus();
}

void LockManager::WaitForSafeRead(absl::Time read_time) {
  absl::MutexLock lock(mu_);

  // Wait for read time to become current if passed a future timestamp  for the
  // case of exact timestamp bound for snapshot read.
  // https://cloud.google.com/spanner/docs/timestamp-bounds#introduction
  bool f = false;
  mu_.AwaitWithDeadline(absl::Condition(&f), read_time);

  while (pending_commit_timestamp_ < read_time) {
    pending_commit_cvar_.Wait(&mu_);
  }
}

absl::Time LockManager::AcquireSnapshot(LockHandle* handle) {
  absl::MutexLock lock(mu_);
  // Choosing the timestamp under mu_ orders it against commit timestamp
  // reservations: every later commit sees this snapshot and records its rows.
  const absl::Time snapshot = clock_->Now();
  snapshots_[handle] = snapshot;
  return snapshot;
}

void LockManager::RecordCommittedWrites(LockHandle* handle,
                                        absl::Span<const CommittedRow> rows) {
  absl::MutexLock lock(mu_);
  if (snapshots_.empty() || committing_handle_ != handle) {
    return;
  }
  for (const CommittedRow& row : rows) {
    committed_writes_[row] = pending_commit_timestamp_;
  }
}

bool LockManager::HasCommittedWriteAfter(absl::Time snapshot,
                                         absl::Span<const CommittedRow> rows,
                                         absl::Span<const LockedRange> ranges) {
  absl::MutexLock lock(mu_);
  for (const CommittedRow& row : rows) {
    auto it = committed_writes_.find(row);
    if (it != committed_writes_.end() && it->second > snapshot) {
      return true;
    }
  }
  for (const auto& [table_id, key_range] : ranges) {
    for (auto it = committed_writes_.lower_bound(CommittedRow(table_id, Key()));
         it != committed_writes_.end() && it->first.first == table_id; ++it) {
      if (it->second > snapshot && key_range.Contains(it->first.second)) {
        return true;
      }
    }
  }
  return false;
}

void LockManager::PruneCommittedWrites() {
  if (snapshots_.empty()) {
    committed_writes_.clear();
    return;
  }
  absl::Time oldest_snapshot = absl::InfiniteFuture();
  for (const auto& [handle, snapshot] : snapshots_) {
    oldest_snapshot = std::min(oldest_snapshot, snapshot);
  }
  std::erase_if(committed_writes_, [oldest_snapshot](const auto& entry) {
    return entry.second <= oldest_snapshot;
  });
}

absl::Time LockManager::LastCommitTimestamp() {
  absl::ReaderMutexLock lock(mu_);
  return last_commit_timestamp_;
}

absl::StatusOr<absl::Time> LockManager::RunWithCommitSerialization(
    const std::function<absl::Status()>& action) {
  absl::MutexLock lock(mu_);
  while (pending_commit_timestamp_ != absl::InfiniteFuture()) {
    pending_commit_cvar_.Wait(&mu_);
  }
  const absl::Time serialized_timestamp = clock_->Now();
  absl::Status status = action();
  if (!status.ok()) return status;
  return serialized_timestamp;
}

}  // namespace backend
}  // namespace emulator
}  // namespace spanner
}  // namespace google

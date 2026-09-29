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

#ifndef THIRD_PARTY_CLOUD_SPANNER_EMULATOR_BACKEND_LOCKING_MANAGER_H_
#define THIRD_PARTY_CLOUD_SPANNER_EMULATOR_BACKEND_LOCKING_MANAGER_H_

#include <deque>
#include <functional>
#include <map>
#include <memory>
#include <optional>
#include <utility>
#include <vector>

#include "absl/base/thread_annotations.h"
#include "absl/container/flat_hash_map.h"
#include "absl/status/status.h"
#include "absl/status/statusor.h"
#include "absl/synchronization/mutex.h"
#include "absl/time/time.h"
#include "backend/common/ids.h"
#include "backend/locking/handle.h"
#include "common/clock.h"

namespace google {
namespace spanner {
namespace emulator {
namespace backend {

// LockManager represents the lock manager for a database.
//
// Transactions interact with the LockManager via a LockHandle which they obtain
// at initialization time. All subsequent communication with the LockManager
// happens via the LockHandle. See LockHandle methods for more details about
// this interaction.
//
// Locks are scoped to table and key range. Empty table IDs represent
// database-wide schema locks.
//
// Conflicts between transactions follow Cloud Spanner's wound-wait scheme. A
// transaction is older than another if it has a lower priority value (its
// first attempt started earlier), with ties broken by transaction ID. An older
// requester aborts (wounds) a younger holder. A younger requester waits until
// the older holder releases its lock, and aborts if that takes longer than
// config::lock_wait_timeout(). A transaction waits only for an older one, or
// for one that is committing and so acquires no more locks, which keeps waits
// from forming a cycle. With config::abort_current_transaction_probability(),
// a younger requester instead wounds an older holder that is not executing a
// request.
//
// Handles without an abort function, such as those of schema changes, are
// neither wounded nor waited for: a request that conflicts with their locks
// aborts. Their own conflicting requests don't wait either: they wound the
// holder with config::abort_current_transaction_probability() if it is not
// executing a request, and abort otherwise.
class LockManager {
 public:
  explicit LockManager(Clock* clock) : clock_(clock) {}

  // Returns a handle for a single transaction with the given id and priority.
  // Subsequent communication between the transaction and the lock manager
  // happens via the handle. See LockHandle methods for more details.
  std::unique_ptr<LockHandle> CreateHandle(
      TransactionID id, const std::function<absl::Status()>& abort_fn,
      TransactionPriority priority);

  // Observes lock requests of transaction `requester` that conflicted with a
  // lock that transaction `holder` held, once the conflict is resolved: by
  // wounding the holder, by aborting the requester, or after the requester
  // waited `lock_wait` for the holder to release the lock. Observers run under
  // the lock manager's mutex and must not call back into the lock manager.
  using ConflictObserver = std::function<void(
      TransactionID requester, const LockRequest& request,
      TransactionID holder, const LockRequest& held, absl::Duration lock_wait)>;

  // Sets the conflict observer. Must be called before any lock is requested.
  void SetConflictObserver(ConflictObserver observer) {
    conflict_observer_ = std::move(observer);
  }

  // Returns the timestamp at which last schema update or commit completed.
  absl::Time LastCommitTimestamp();

  // Runs an action after every previously timestamped commit has completed and
  // prevents later commits from reserving a timestamp until the action
  // finishes. Returns a timestamp that externally orders the action between
  // those two sets of commits.
  absl::StatusOr<absl::Time> RunWithCommitSerialization(
      const std::function<absl::Status()>& action)
      ABSL_LOCKS_EXCLUDED(mu_);

 private:
  // LockHandle simply forwards requests to the LockManager.
  friend class LockHandle;
  void EnqueueLock(LockHandle* handle, const LockRequest& request)
      ABSL_LOCKS_EXCLUDED(mu_);
  bool IsWaiting(LockHandle* handle) ABSL_LOCKS_EXCLUDED(mu_);
  void WaitForLocks(LockHandle* handle) ABSL_LOCKS_EXCLUDED(mu_);
  void UnlockAll(LockHandle* handle) ABSL_LOCKS_EXCLUDED(mu_);
  absl::StatusOr<absl::Time> ReserveCommitTimestamp(LockHandle* handle)
      ABSL_LOCKS_EXCLUDED(mu_);
  absl::Status MarkCommitted(LockHandle* handle) ABSL_LOCKS_EXCLUDED(mu_);
  void WaitForSafeRead(absl::Time read_time) ABSL_LOCKS_EXCLUDED(mu_);
  absl::Time AcquireSnapshot(LockHandle* handle) ABSL_LOCKS_EXCLUDED(mu_);
  void RecordCommittedWrites(LockHandle* handle,
                             absl::Span<const CommittedRow> rows)
      ABSL_LOCKS_EXCLUDED(mu_);
  bool HasCommittedWriteAfter(absl::Time snapshot,
                              absl::Span<const CommittedRow> rows,
                              absl::Span<const LockedRange> ranges)
      ABSL_LOCKS_EXCLUDED(mu_);
  // Drops committed writes no active repeatable-read snapshot can conflict
  // with.
  void PruneCommittedWrites() ABSL_EXCLUSIVE_LOCKS_REQUIRED(mu_);

  struct HeldLocks {
    std::vector<LockRequest> shared;
    std::vector<LockRequest> exclusive;
  };

  // The lock requests of a handle that are not granted yet because the first
  // one waits for a conflicting lock.
  struct Waiter {
    // In the order they were requested.
    std::deque<LockRequest> requests;
    // When the first request aborts if it is still waiting.
    std::optional<absl::Time> deadline;
    // The transaction whose lock the first request waits for, since when, and
    // that lock. Reset once the first request stops waiting for it.
    TransactionID holder = kInvalidTransactionID;
    absl::Time wait_start;
    std::optional<LockRequest> held;
  };

  // How a conflict between a requester and a holder is resolved.
  enum class Resolution { kWound, kWait, kAbortRequester };

  // How a lock request ended up.
  enum class Acquisition { kGranted, kWaiting, kAborted };

  // Resolves the conflicts of `request` of `handle` with locks that other
  // handles hold, and grants the request once no conflict remains, unless
  // `handle` has to wait or was aborted. `waiter` tracks the wait of `handle`
  // for the request.
  Acquisition Acquire(LockHandle* handle, const LockRequest& request,
                      Waiter& waiter) ABSL_EXCLUSIVE_LOCKS_REQUIRED(mu_);

  // Decides how to resolve a conflict of `requester` with a lock of `holder`,
  // and wounds the holder if that resolves it.
  Resolution ResolveConflict(LockHandle* requester, LockHandle* holder)
      ABSL_EXCLUSIVE_LOCKS_REQUIRED(mu_);

  // Grants the requests of waiting `handle` in order until one has to wait.
  // Returns false if `handle` still waits.
  bool GrantWaitingRequests(LockHandle* handle)
      ABSL_EXCLUSIVE_LOCKS_REQUIRED(mu_);

  // Reports how long `request` of `handle` waited for the holder that `waiter`
  // tracks, if it waited.
  void EndWait(LockHandle* handle, const LockRequest& request, Waiter& waiter)
      ABSL_EXCLUSIVE_LOCKS_REQUIRED(mu_);

  // Aborts `handle` and releases its locks.
  void AbortRequester(LockHandle* handle, const absl::Status& status)
      ABSL_EXCLUSIVE_LOCKS_REQUIRED(mu_);

  // Mutex to guard state below.
  absl::Mutex mu_;

  // Group by handle so a transaction's own locks are skipped. Separate shared
  // requests so exclusive writes need not scan prior exclusive writes merely
  // to check whether this request upgrades a read lock.
  absl::flat_hash_map<LockHandle*, HeldLocks> held_locks_
      ABSL_GUARDED_BY(mu_);

  // Handles whose lock requests wait for conflicting locks.
  absl::flat_hash_map<LockHandle*, Waiter> waiters_ ABSL_GUARDED_BY(mu_);

  // Signals that locks were released, or that a waiting handle was aborted.
  absl::CondVar locks_released_cvar_;

  // Only one commit may have a reserved timestamp at a time.
  LockHandle* committing_handle_ ABSL_GUARDED_BY(mu_) = nullptr;

  // System wide monotonic clock used to provide commit and read timestamps.
  Clock* clock_;

  // Timestamp at which last schema update or commit completed.
  absl::Time last_commit_timestamp_ ABSL_GUARDED_BY(mu_) = absl::InfinitePast();

  // Commit timestamp being used by an in-progress commit.
  absl::Time pending_commit_timestamp_ ABSL_GUARDED_BY(mu_) =
      absl::InfiniteFuture();

  // Signals completion of pending commit.
  absl::CondVar pending_commit_cvar_ ABSL_GUARDED_BY(mu_);

  // Snapshot timestamps of active repeatable-read transactions.
  absl::flat_hash_map<LockHandle*, absl::Time> snapshots_ ABSL_GUARDED_BY(mu_);

  // Latest commit timestamp of each row written while a repeatable-read
  // snapshot was active. Ordered so a table's rows are contiguous.
  std::map<CommittedRow, absl::Time> committed_writes_ ABSL_GUARDED_BY(mu_);

  ConflictObserver conflict_observer_;
};

}  // namespace backend
}  // namespace emulator
}  // namespace spanner
}  // namespace google

#endif  // THIRD_PARTY_CLOUD_SPANNER_EMULATOR_BACKEND_LOCKING_MANAGER_H_

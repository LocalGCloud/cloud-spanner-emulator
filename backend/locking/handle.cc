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

#include "backend/locking/handle.h"

#include <functional>

#include "absl/status/status.h"
#include "absl/status/statusor.h"
#include "absl/strings/str_cat.h"
#include "backend/locking/manager.h"
#include "common/errors.h"

namespace google {
namespace spanner {
namespace emulator {
namespace backend {

LockHandle::LockHandle(LockManager* manager, TransactionID tid,
                       const std::function<absl::Status()>& abort_fn,
                       TransactionPriority priority)
    : manager_(manager),
      tid_(tid),
      try_abort_transaction_fn_(abort_fn),
      priority_(priority) {}

LockHandle::~LockHandle() {
  {
    absl::MutexLock lock(mu_);
    try_abort_transaction_fn_ = nullptr;
  }
  manager_->UnlockAll(this);
}

void LockHandle::EnqueueLock(const LockRequest& request) {
  manager_->EnqueueLock(this, request);
}

void LockHandle::UnlockAll() { manager_->UnlockAll(this); }

bool LockHandle::IsBlocked() { return manager_->IsWaiting(this); }

bool LockHandle::IsAborted() {
  absl::MutexLock lock(mu_);
  return !status_.ok();
}

absl::Status LockHandle::Wait() {
  manager_->WaitForLocks(this);
  return status();
}

absl::Status LockHandle::status() {
  absl::MutexLock lock(mu_);
  return status_;
}

bool LockHandle::IsAbortable() {
  absl::MutexLock lock(mu_);
  return try_abort_transaction_fn_ != nullptr;
}

void LockHandle::Abort(const absl::Status& status) {
  absl::MutexLock lock(mu_);
  status_ = status;
}

absl::Status LockHandle::TryAbortTransaction(const absl::Status& status) {
  if (mu_.try_lock()) {
    if (try_abort_transaction_fn_ != nullptr) {
      auto aborted = try_abort_transaction_fn_();
      if (aborted.ok()) {
        status_ = status;
        mu_.unlock();
        return absl::OkStatus();
      }
    }
    mu_.unlock();
  }
  return error::CouldNotObtainLockHandleMutex(tid_);
}

bool LockHandle::ForceAbortTransaction(const absl::Status& status) {
  absl::MutexLock lock(mu_);
  if (try_abort_transaction_fn_ == nullptr || !status_.ok()) {
    return false;
  }
  status_ = status;
  return true;
}

void LockHandle::Reset() {
  absl::MutexLock lock(mu_);
  status_ = absl::OkStatus();
}

absl::StatusOr<absl::Time> LockHandle::ReserveCommitTimestamp() {
  return manager_->ReserveCommitTimestamp(this);
}

absl::Status LockHandle::MarkCommitted() {
  return manager_->MarkCommitted(this);
}

void LockHandle::WaitForSafeRead(absl::Time read_time) {
  manager_->WaitForSafeRead(read_time);
}

absl::Time LockHandle::AcquireSnapshot() {
  return manager_->AcquireSnapshot(this);
}

void LockHandle::RecordCommittedWrites(absl::Span<const CommittedRow> rows) {
  manager_->RecordCommittedWrites(this, rows);
}

bool LockHandle::HasCommittedWriteAfter(absl::Time snapshot,
                                        absl::Span<const CommittedRow> rows,
                                        absl::Span<const LockedRange> ranges) {
  return manager_->HasCommittedWriteAfter(snapshot, rows, ranges);
}

}  // namespace backend
}  // namespace emulator
}  // namespace spanner
}  // namespace google

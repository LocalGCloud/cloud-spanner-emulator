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

#ifndef THIRD_PARTY_CLOUD_SPANNER_EMULATOR_BACKEND_DATABASE_ROW_DELETION_POLICY_SWEEPER_H_
#define THIRD_PARTY_CLOUD_SPANNER_EMULATOR_BACKEND_DATABASE_ROW_DELETION_POLICY_SWEEPER_H_

#include <functional>
#include <memory>
#include <thread>  // NOLINT
#include <vector>

#include "absl/base/thread_annotations.h"
#include "absl/status/status.h"
#include "absl/status/statusor.h"
#include "absl/strings/string_view.h"
#include "absl/synchronization/mutex.h"
#include "absl/time/time.h"
#include "backend/datamodel/key_set.h"
#include "backend/schema/catalog/schema.h"
#include "backend/stats/system_stats_collector.h"
#include "backend/transaction/options.h"
#include "backend/transaction/read_only_transaction.h"
#include "backend/transaction/read_write_transaction.h"

namespace google {
namespace spanner {
namespace emulator {
namespace backend {

// Deletes the rows of a database that its tables' row deletion policies (TTL)
// have expired: rows whose policy timestamp column is not NULL and is older
// than the policy's interval.
//
// Once the schema has a row deletion policy, a background thread sweeps the
// database every `sweep_interval`. A sweep reads each table that has a policy
// at a strong timestamp without taking locks, then deletes its expired rows in
// batches of at most `batch_size` rows. Each batch is its own read-write
// transaction that re-reads the batch's rows and deletes the ones that are
// still expired, so a row whose timestamp was updated after the scan is kept.
// The deletes run through the regular mutation path: they cascade to
// interleaved children and foreign keys and update indexes like a user Delete
// mutation. The transactions are marked as row deletion policy transactions,
// so change streams record the deletes with the transaction tag
// "RowDeletionPolicy" as a system transaction, and streams that exclude TTL
// deletes skip them.
//
// After sweeping a table, the sweeper records the sweep in the statistics
// collector, which SPANNER_SYS.ROW_DELETION_POLICIES shows.
class RowDeletionPolicySweeper {
 public:
  using CreateReadOnlyTransactionFn =
      std::function<absl::StatusOr<std::unique_ptr<ReadOnlyTransaction>>(
          const ReadOnlyOptions& options)>;
  using CreateReadWriteTransactionFn =
      std::function<absl::StatusOr<std::unique_ptr<ReadWriteTransaction>>(
          const ReadWriteOptions& options, const RetryState& retry_state)>;

  // Rows deleted by one transaction when the database creates the sweeper.
  static constexpr int kDefaultBatchSize = 1000;

  // A `sweep_interval` that is not positive disables the background thread;
  // SweepOnce() still works. `stats_collector` may be null.
  RowDeletionPolicySweeper(
      CreateReadOnlyTransactionFn create_read_only_transaction_fn,
      CreateReadWriteTransactionFn create_read_write_transaction_fn,
      absl::Duration sweep_interval, int batch_size,
      SystemStatsCollector* stats_collector = nullptr);

  // Stops the background thread after the batch in progress, if any.
  ~RowDeletionPolicySweeper();

  RowDeletionPolicySweeper(const RowDeletionPolicySweeper&) = delete;
  RowDeletionPolicySweeper& operator=(const RowDeletionPolicySweeper&) =
      delete;

  // Starts the background thread the first time `schema` has a row deletion
  // policy. The thread first sweeps one interval after it starts, and keeps
  // running if the policies are later dropped.
  void Update(const Schema* schema);

  // Deletes the rows that are expired now. Keeps going after a table or batch
  // fails and returns the first error.
  absl::Status SweepOnce();

 private:
  // Deletes the rows among `keys` of the table named `table_name` that are
  // still expired at `now`, in one transaction retried while it aborts.
  absl::Status DeleteExpiredRows(absl::string_view table_name, absl::Time now,
                                 const KeySet& keys);

  absl::Status TryDeleteExpiredRows(absl::string_view table_name,
                                    absl::Time now, const KeySet& keys,
                                    const RetryState& retry_state);

  void Run();

  // Waits for `duration` or until the sweeper stops. Returns false if it
  // stopped.
  bool WaitUnlessStopped(absl::Duration duration) ABSL_LOCKS_EXCLUDED(mu_);

  bool stopped() ABSL_LOCKS_EXCLUDED(mu_);

  const CreateReadOnlyTransactionFn create_read_only_transaction_fn_;
  const CreateReadWriteTransactionFn create_read_write_transaction_fn_;
  const absl::Duration sweep_interval_;
  const int batch_size_;
  SystemStatsCollector* const stats_collector_;

  absl::Mutex mu_;
  bool stop_ ABSL_GUARDED_BY(mu_) = false;
  std::thread thread_ ABSL_GUARDED_BY(mu_);
};

}  // namespace backend
}  // namespace emulator
}  // namespace spanner
}  // namespace google

#endif  // THIRD_PARTY_CLOUD_SPANNER_EMULATOR_BACKEND_DATABASE_ROW_DELETION_POLICY_SWEEPER_H_

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

#include "backend/database/row_deletion_policy_sweeper.h"

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <string>
#include <thread>  // NOLINT
#include <utility>
#include <vector>

#include "absl/algorithm/container.h"
#include "absl/log/log.h"
#include "absl/random/random.h"
#include "absl/status/status.h"
#include "absl/status/statusor.h"
#include "absl/strings/string_view.h"
#include "absl/synchronization/mutex.h"
#include "absl/time/time.h"
#include "absl/types/span.h"
#include "backend/access/read.h"
#include "backend/access/write.h"
#include "backend/datamodel/key.h"
#include "backend/datamodel/key_set.h"
#include "backend/schema/catalog/schema.h"
#include "backend/schema/catalog/table.h"
#include "backend/schema/ddl/operations.pb.h"
#include "backend/transaction/options.h"
#include "backend/transaction/read_only_transaction.h"
#include "backend/transaction/read_write_transaction.h"
#include "googlesql/base/status_macros.h"
#include "googlesql/public/value.h"

namespace google {
namespace spanner {
namespace emulator {
namespace backend {

namespace {

// Attempts of a batch's transaction that aborts before the sweep leaves the
// batch to the next sweep. Every attempt that conflicts with a user
// transaction may abort that transaction, so the sweep gives up quickly rather
// than competing with user transactions for the same rows.
constexpr int kMaxAttempts = 5;

// Backoff before the first retry of an aborted batch; it doubles on each retry.
constexpr absl::Duration kInitialRetryBackoff = absl::Milliseconds(50);

// Returns how old a row's timestamp must be for `policy` to delete the row.
absl::Duration ExpirationAge(const ddl::RowDeletionPolicy& policy) {
  // A DDLTimeLengthProto::Unit value is the unit's length in seconds.
  return absl::Seconds(policy.older_than().count()) *
         static_cast<int64_t>(policy.older_than().unit());
}

// A row that a row deletion policy expired.
struct ExpiredRow {
  Key key;
  // The value of the policy's timestamp column.
  absl::Time timestamp;
};

// Returns the rows of `table` in `key_set` that `reader` sees expired at `now`
// under the table's row deletion policy.
absl::StatusOr<std::vector<ExpiredRow>> ReadExpiredRows(RowReader* reader,
                                                        const Table* table,
                                                        const KeySet& key_set,
                                                        absl::Time now) {
  // row_deletion_policy() returns a copy, so keep one alive for this scope.
  const ddl::RowDeletionPolicy policy = *table->row_deletion_policy();
  const absl::Time cutoff = now - ExpirationAge(policy);
  const absl::Span<const KeyColumn* const> primary_key = table->primary_key();
  const int timestamp_index = primary_key.size();

  ReadArg read_arg;
  read_arg.table = table->Name();
  read_arg.key_set = key_set;
  for (const KeyColumn* key_column : primary_key) {
    read_arg.columns.push_back(key_column->column()->Name());
  }
  read_arg.columns.push_back(policy.column_name());
  std::unique_ptr<RowCursor> cursor;
  GOOGLESQL_RETURN_IF_ERROR(reader->Read(read_arg, &cursor));

  std::vector<ExpiredRow> rows;
  while (cursor->Next()) {
    const googlesql::Value timestamp = cursor->ColumnValue(timestamp_index);
    if (timestamp.is_null() || timestamp.ToTime() >= cutoff) {
      continue;
    }
    Key key;
    for (int i = 0; i < timestamp_index; ++i) {
      key.AddColumn(cursor->ColumnValue(i), primary_key[i]->is_descending(),
                    primary_key[i]->is_nulls_last());
    }
    rows.push_back({std::move(key), timestamp.ToTime()});
  }
  GOOGLESQL_RETURN_IF_ERROR(cursor->Status());
  return rows;
}

KeySet ToKeySet(absl::Span<const ExpiredRow> rows) {
  KeySet key_set;
  for (const ExpiredRow& row : rows) {
    key_set.AddKey(row.key);
  }
  return key_set;
}

}  // namespace

RowDeletionPolicySweeper::RowDeletionPolicySweeper(
    CreateReadOnlyTransactionFn create_read_only_transaction_fn,
    CreateReadWriteTransactionFn create_read_write_transaction_fn,
    absl::Duration sweep_interval, int batch_size,
    SystemStatsCollector* stats_collector)
    : create_read_only_transaction_fn_(
          std::move(create_read_only_transaction_fn)),
      create_read_write_transaction_fn_(
          std::move(create_read_write_transaction_fn)),
      sweep_interval_(sweep_interval),
      batch_size_(batch_size),
      stats_collector_(stats_collector) {}

RowDeletionPolicySweeper::~RowDeletionPolicySweeper() {
  std::thread thread;
  {
    absl::MutexLock lock(mu_);
    stop_ = true;
    thread = std::move(thread_);
  }
  if (thread.joinable()) {
    thread.join();
  }
}

void RowDeletionPolicySweeper::Update(const Schema* schema) {
  if (sweep_interval_ <= absl::ZeroDuration() ||
      absl::c_none_of(schema->tables(), [](const Table* table) {
        return table->row_deletion_policy().has_value();
      })) {
    return;
  }
  absl::MutexLock lock(mu_);
  if (!thread_.joinable() && !stop_) {
    thread_ = std::thread(&RowDeletionPolicySweeper::Run, this);
  }
}

void RowDeletionPolicySweeper::Run() {
  while (WaitUnlessStopped(sweep_interval_)) {
    absl::Status status = SweepOnce();
    if (!status.ok()) {
      ABSL_LOG(WARNING) << "Row deletion policy sweep failed: " << status;
    }
  }
}

absl::Status RowDeletionPolicySweeper::SweepOnce() {
  GOOGLESQL_ASSIGN_OR_RETURN(std::unique_ptr<ReadOnlyTransaction> snapshot,
                             create_read_only_transaction_fn_(ReadOnlyOptions()));
  const absl::Time now = snapshot->read_timestamp();
  absl::Status sweep_status;
  for (const Table* table : snapshot->schema()->tables()) {
    if (!table->row_deletion_policy().has_value()) {
      continue;
    }
    absl::StatusOr<std::vector<ExpiredRow>> expired_rows =
        ReadExpiredRows(snapshot.get(), table, KeySet::All(), now);
    if (!expired_rows.ok()) {
      sweep_status.Update(expired_rows.status());
      continue;
    }
    RowDeletionPolicySweep sweep{.processed_watermark = now};
    bool stopped_early = false;
    for (size_t start = 0; start < expired_rows->size();
         start += batch_size_) {
      if (stopped()) {
        stopped_early = true;
        break;
      }
      const absl::Span<const ExpiredRow> batch =
          absl::MakeConstSpan(*expired_rows)
              .subspan(start, std::min<size_t>(batch_size_,
                                               expired_rows->size() - start));
      const absl::Status status =
          DeleteExpiredRows(table->Name(), now, ToKeySet(batch));
      if (status.ok()) {
        continue;
      }
      sweep_status.Update(status);
      sweep.undeletable_rows += batch.size();
      for (const ExpiredRow& row : batch) {
        if (!sweep.min_undeletable_timestamp.has_value() ||
            row.timestamp < *sweep.min_undeletable_timestamp) {
          sweep.min_undeletable_timestamp = row.timestamp;
        }
      }
    }
    if (!stopped_early && stats_collector_ != nullptr) {
      stats_collector_->RecordRowDeletionPolicySweep(table->Name(), sweep);
    }
  }
  return sweep_status;
}

absl::Status RowDeletionPolicySweeper::DeleteExpiredRows(
    absl::string_view table_name, absl::Time now, const KeySet& keys) {
  // Like a client, retry an aborted transaction after a randomized backoff
  // with its abort count, which also exempts the retry from fault injection.
  RetryState retry_state;
  absl::BitGen bitgen;
  absl::Duration backoff = kInitialRetryBackoff;
  absl::Status status;
  for (int attempt = 1; attempt <= kMaxAttempts; ++attempt) {
    status = TryDeleteExpiredRows(table_name, now, keys, retry_state);
    if (!absl::IsAborted(status) || attempt == kMaxAttempts ||
        !WaitUnlessStopped(backoff * absl::Uniform(bitgen, 1.0, 2.0))) {
      break;
    }
    backoff *= 2;
    ++retry_state.abort_retry_count;
  }
  return status;
}

absl::Status RowDeletionPolicySweeper::TryDeleteExpiredRows(
    absl::string_view table_name, absl::Time now, const KeySet& keys,
    const RetryState& retry_state) {
  ReadWriteOptions options;
  options.row_deletion_policy_txn = true;
  GOOGLESQL_ASSIGN_OR_RETURN(
      std::unique_ptr<ReadWriteTransaction> txn,
      create_read_write_transaction_fn_(options, retry_state));
  // The table or its policy may have been dropped or changed since the scan.
  const Table* table = txn->schema()->FindTable(std::string(table_name));
  if (table == nullptr || !table->row_deletion_policy().has_value()) {
    return txn->Rollback();
  }
  GOOGLESQL_ASSIGN_OR_RETURN(std::vector<ExpiredRow> expired_rows,
                             ReadExpiredRows(txn.get(), table, keys, now));
  if (expired_rows.empty()) {
    return txn->Rollback();
  }
  Mutation mutation;
  mutation.AddDeleteOp(table->Name(), ToKeySet(expired_rows));
  GOOGLESQL_RETURN_IF_ERROR(txn->Write(mutation));
  return txn->Commit();
}

bool RowDeletionPolicySweeper::WaitUnlessStopped(absl::Duration duration) {
  absl::MutexLock lock(mu_);
  mu_.AwaitWithTimeout(absl::Condition(&stop_), duration);
  return !stop_;
}

bool RowDeletionPolicySweeper::stopped() {
  absl::MutexLock lock(mu_);
  return stop_;
}

}  // namespace backend
}  // namespace emulator
}  // namespace spanner
}  // namespace google

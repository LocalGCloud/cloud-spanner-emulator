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

#ifndef THIRD_PARTY_CLOUD_SPANNER_EMULATOR_BACKEND_STATS_SYSTEM_STATS_COLLECTOR_H_
#define THIRD_PARTY_CLOUD_SPANNER_EMULATOR_BACKEND_STATS_SYSTEM_STATS_COLLECTOR_H_

#include <cstdint>
#include <memory>
#include <optional>
#include <string>
#include <vector>

#include "googlesql/public/value.h"
#include "absl/base/thread_annotations.h"
#include "absl/container/flat_hash_map.h"
#include "absl/status/status.h"
#include "absl/strings/string_view.h"
#include "absl/synchronization/mutex.h"
#include "absl/time/time.h"
#include "backend/common/ids.h"
#include "backend/datamodel/key.h"
#include "backend/schema/catalog/schema.h"
#include "backend/stats/operation_stats.h"

namespace google {
namespace spanner {
namespace emulator {
namespace backend {

// The aggregation intervals of the interval-based SPANNER_SYS tables.
enum class StatsInterval { kMinute, kTenMinutes, kHour };

// Returns the end of the `interval`-long, clock-aligned interval that contains
// `time`. Intervals include their start and exclude their end.
absl::Time IntervalEnd(absl::Time time, StatsInterval interval);

// Returns how long statistics of the given interval are kept.
absl::Duration StatsRetention(StatsInterval interval);

// Returns `tag` as Cloud Spanner stores request and transaction tags: only
// printable ASCII characters, without leading underscores, at most 50
// characters long.
std::string NormalizeTag(absl::string_view tag);

// Formats a key value the way the SPANNER_SYS tables show keys, e.g. in
// Singers(2) or Singers(abc): strings are unquoted.
std::string FormatKeyValue(const googlesql::Value& value);

// One execution of a SQL statement.
struct QueryExecution {
  std::string text;
  std::string request_tag;
  // Executed with a partition token or as partitioned DML.
  bool partitioned = false;
  bool in_read_write_transaction = false;
  absl::StatusCode status = absl::StatusCode::kOk;
  absl::Duration latency;
  absl::Duration cpu_time;
  absl::Duration plan_creation_time;
  int64_t rows_returned = 0;
  int64_t bytes_returned = 0;
  int64_t rows_scanned = 0;
  int64_t rows_written = 0;
  int64_t bytes_written = 0;
};

// One call of the Read or StreamingRead API.
struct ReadExecution {
  std::string table;
  std::vector<std::string> columns;
  std::string request_tag;
  // Executed with a partition token.
  bool partitioned = false;
  bool in_read_write_transaction = false;
  int64_t rows = 0;
  int64_t bytes = 0;
  absl::Duration cpu_time;
};

// One attempt of a read-write transaction that committed, failed to commit,
// or aborted.
struct TransactionAttempt {
  enum class Outcome {
    kCommitted,
    // The commit failed with a precondition error, such as a violated unique
    // index or a row that already exists.
    kCommitFailedPrecondition,
    kCommitFailed,
    kAborted,
  };
  enum class ConcurrencyMode {
    kSerializablePessimistic,
    kSerializableOptimistic,
    kRepeatableReadOptimistic,
    kRepeatableReadPessimistic,
  };

  std::string transaction_tag;
  AccessFootprint footprint;
  Outcome outcome = Outcome::kCommitted;
  bool commit_attempted = false;
  // The attempt retries a previously aborted attempt.
  bool retry = false;
  ConcurrencyMode concurrency_mode = ConcurrencyMode::kSerializablePessimistic;
  // From the attempt's first operation until it committed or aborted.
  absl::Duration total_latency;
  absl::Duration commit_latency;
};

// A lock request that conflicted with a lock held by another transaction.
struct LockConflict {
  struct Request {
    TransactionID transaction_id;
    bool exclusive = false;
    // Empty for a lock on whole rows.
    std::vector<ColumnID> column_ids;
  };

  TableID table_id;
  Key start_key;
  // The conflict covers more than the keys with the prefix `start_key`.
  bool is_range = false;
  // The requesting and the holding lock request.
  std::vector<Request> requests;
  // How long the requester waited for the holder to release its lock. Zero if
  // the conflict was resolved by aborting either transaction.
  absl::Duration lock_wait;
};

// A SQL statement that is executing.
struct ActiveQuery {
  std::string text;
  std::string request_tag;
  std::string session_id;
  std::string priority;
  // READ_ONLY, READ_WRITE or NONE.
  std::string transaction_type;
  std::string client_ip_address;
  std::string api_client_header;
  std::string user_agent_header;
  absl::Time start_time;
};

// A partitioned DML statement that is executing.
struct ActivePartitionedDml {
  std::string text;
  std::string session_id;
  absl::Time start_time;
};

// The outcome of a row deletion policy (TTL) sweep of one table.
struct RowDeletionPolicySweep {
  // The sweep processed every row that had expired at this time.
  absl::Time processed_watermark;
  // The expired rows that the sweep failed to delete.
  int64_t undeletable_rows = 0;
  // The oldest policy timestamp of an undeletable row, if there is one.
  std::optional<absl::Time> min_undeletable_timestamp;
};

// A split point added with the AddSplitPoints RPC.
struct UserSplitPoint {
  std::string table_name;
  // Empty for a split point of the table itself.
  std::string index_name;
  std::string initiator;
  // The split key, formatted as SPANNER_SYS.USER_SPLIT_POINTS shows it.
  std::string split_key;
  absl::Time expire_time;
};

// A row of a SPANNER_SYS table, keyed by upper-case column name. Columns that
// a table does not have are ignored by the catalog, and columns missing from
// the row are NULL.
using SpannerSysRow = absl::flat_hash_map<std::string, googlesql::Value>;

// Collects the query, read, transaction, lock, table operation and table size
// statistics of one database in memory and serves them as rows of the
// SPANNER_SYS statistics tables. Statistics are aggregated into clock-aligned
// minute, 10-minute and hour intervals that are kept for 6 hours, 4 days and
// 30 days respectively. Unless the collector has a persistence file, they are
// lost when the emulator restarts.
//
// It also serves the SPANNER_SYS tables that describe the current state of the
// database: active queries and partitioned DMLs, row deletion policies and
// user split points. The persistence file also keeps the last row deletion
// policy sweep of each table; executing statements are never saved, and user
// split points are saved with the database data instead.
//
// Recording and snapshots take explicit times so that callers control the
// clock. This class is thread safe.
class SystemStatsCollector {
 public:
  // The maximum number of rows per interval of a TOP table.
  static constexpr int kTopN = 100;

  // The longest statement text that is kept, in bytes.
  static constexpr int kMaxTextBytes = 64 * 1024;

  SystemStatsCollector();

  // Keeps the interval statistics in the file `persistence_path` so that they
  // survive a restart. The collector starts with the statistics saved in the
  // file, except those past their retention period at `now`, and saves them
  // again when a minute interval ends and on Persist(). A missing or
  // unreadable file leaves the collector empty.
  SystemStatsCollector(std::string persistence_path, absl::Time now);

  ~SystemStatsCollector();

  // Returns true if the rows of the SPANNER_SYS table `table_name` come from
  // collected statistics.
  static bool ServesTable(absl::string_view table_name);

  // Records a SQL statement that ended at `end_time`. `footprint` holds the
  // tables and columns that the statement accessed.
  void RecordQuery(absl::Time end_time, const QueryExecution& query,
                   const AccessFootprint& footprint) ABSL_LOCKS_EXCLUDED(mu_);

  // Records a read that ended at `end_time`.
  void RecordRead(absl::Time end_time, const ReadExecution& read)
      ABSL_LOCKS_EXCLUDED(mu_);

  // Records the table and column operations of mutations that were committed
  // or submitted for commit at `time`.
  void RecordMutations(absl::Time time, const AccessFootprint& footprint)
      ABSL_LOCKS_EXCLUDED(mu_);

  // Records a read-write transaction attempt that ended at `end_time`.
  void RecordTransactionAttempt(absl::Time end_time,
                                const TransactionAttempt& attempt)
      ABSL_LOCKS_EXCLUDED(mu_);

  // Records a lock conflict resolved at `time`. Conflicts are recorded only
  // if the requester is a registered user transaction.
  void RecordLockConflict(absl::Time time, const LockConflict& conflict)
      ABSL_LOCKS_EXCLUDED(mu_);

  // Registers a user read-write transaction and its (normalized) tag for lock
  // statistics. Registering again replaces the tag.
  void RegisterTransaction(TransactionID id, absl::string_view tag)
      ABSL_LOCKS_EXCLUDED(mu_);
  void UnregisterTransaction(TransactionID id) ABSL_LOCKS_EXCLUDED(mu_);

  // Registers an executing statement and returns its query ID, which must be
  // passed to EndQuery once the statement finishes.
  int64_t StartQuery(ActiveQuery query) ABSL_LOCKS_EXCLUDED(mu_);
  void EndQuery(int64_t query_id) ABSL_LOCKS_EXCLUDED(mu_);

  // Registers an executing partitioned DML statement and returns its ID,
  // which must be passed to EndPartitionedDml once the statement finishes.
  // The emulator runs a partitioned DML as a single partition, so it shows no
  // progress until it finishes.
  int64_t StartPartitionedDml(ActivePartitionedDml dml)
      ABSL_LOCKS_EXCLUDED(mu_);
  void EndPartitionedDml(int64_t id) ABSL_LOCKS_EXCLUDED(mu_);

  // Records a sample, taken at `time`, of the logical size in bytes of each
  // table and index, by name. TABLE_SIZES_STATS_1HOUR averages the samples of
  // each hour.
  void RecordTableSizes(absl::Time time,
                        const absl::flat_hash_map<std::string, double>& sizes)
      ABSL_LOCKS_EXCLUDED(mu_);

  // Records the last sweep of the row deletion policy of `table_name`.
  void RecordRowDeletionPolicySweep(absl::string_view table_name,
                                    const RowDeletionPolicySweep& sweep)
      ABSL_LOCKS_EXCLUDED(mu_);

  // Adds a split point, or replaces the one with the same table, index and
  // split key. Expired split points are not shown.
  void AddUserSplitPoint(UserSplitPoint split_point) ABSL_LOCKS_EXCLUDED(mu_);

  // Returns the rows of the SPANNER_SYS table `table_name` at time `now`.
  // Interval-based tables include only intervals that ended by `now`, unless
  // `include_open_intervals` is set. `schema` resolves the table and column
  // IDs of lock statistics and lists the tables with row deletion policies.
  std::vector<SpannerSysRow> Snapshot(absl::string_view table_name,
                                      absl::Time now,
                                      bool include_open_intervals,
                                      const Schema* schema) const
      ABSL_LOCKS_EXCLUDED(mu_);

  // Returns the interval statistics, table size samples and row deletion
  // policy sweeps in the format of the persistence file. Executing queries and
  // partitioned DMLs, registered transactions and user split points are not
  // saved.
  std::string Save() const ABSL_LOCKS_EXCLUDED(mu_);

  // Replaces the interval statistics with statistics returned by Save(),
  // except those past their retention period at `now`.
  absl::Status Restore(absl::string_view saved, absl::Time now)
      ABSL_LOCKS_EXCLUDED(mu_);

  // Saves the interval statistics to the persistence file, if the collector
  // has one and its directory exists.
  void Persist() const ABSL_LOCKS_EXCLUDED(persist_mu_, mu_);

 private:
  // The collected statistics, defined in the .cc file.
  struct State;

  // Persists the statistics if `time` is in a later minute interval than any
  // statistics recorded before, which means that an interval has ended.
  void PersistIfIntervalEnded(absl::Time time)
      ABSL_LOCKS_EXCLUDED(persist_mu_, mu_);

  // Empty if statistics are kept in memory only.
  const std::string persistence_path_;

  // Serializes writes of the persistence file.
  mutable absl::Mutex persist_mu_ ABSL_ACQUIRED_BEFORE(mu_);

  mutable absl::Mutex mu_;
  std::unique_ptr<State> state_ ABSL_GUARDED_BY(mu_);
};

}  // namespace backend
}  // namespace emulator
}  // namespace spanner
}  // namespace google

#endif  // THIRD_PARTY_CLOUD_SPANNER_EMULATOR_BACKEND_STATS_SYSTEM_STATS_COLLECTOR_H_

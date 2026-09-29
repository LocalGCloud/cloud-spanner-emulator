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

#include "backend/stats/system_stats_collector.h"

#include <algorithm>
#include <array>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <memory>
#include <optional>
#include <string>
#include <system_error>
#include <tuple>
#include <utility>
#include <vector>

#include "googlesql/public/value.h"
#include "absl/container/btree_map.h"
#include "absl/container/btree_set.h"
#include "absl/container/flat_hash_map.h"
#include "absl/log/log.h"
#include "absl/status/status.h"
#include "absl/strings/ascii.h"
#include "absl/strings/match.h"
#include "absl/strings/str_cat.h"
#include "absl/strings/str_join.h"
#include "absl/strings/string_view.h"
#include "absl/strings/strip.h"
#include "absl/synchronization/mutex.h"
#include "absl/time/time.h"
#include "backend/common/ids.h"
#include "backend/datamodel/key.h"
#include "backend/schema/catalog/column.h"
#include "backend/schema/catalog/index.h"
#include "backend/schema/catalog/schema.h"
#include "backend/schema/catalog/table.h"
#include "backend/stats/latency_distribution.h"
#include "backend/stats/operation_stats.h"
#include "backend/stats/spanner_sys_types.h"
#include "common/constants.h"
#include "farmhash.h"
#include "nlohmann/json.hpp"

namespace google {
namespace spanner {
namespace emulator {
namespace backend {

namespace {

using ::googlesql::values::Bool;
using ::googlesql::values::Bytes;
using ::googlesql::values::Double;
using ::googlesql::values::Int64;
using ::googlesql::values::Int64Array;
using ::googlesql::values::NullDouble;
using ::googlesql::values::NullTimestamp;
using ::googlesql::values::String;
using ::googlesql::values::StringArray;
using ::googlesql::values::Timestamp;

constexpr std::array<StatsInterval, 3> kIntervals = {
    StatsInterval::kMinute, StatsInterval::kTenMinutes, StatsInterval::kHour};

// Suffixes of the table names of each interval, in kIntervals order.
constexpr std::array<absl::string_view, 3> kIntervalSuffixes = {
    "MINUTE", "10MINUTE", "HOUR"};

// The index of StatsInterval::kHour in kIntervals.
constexpr int kHourIntervalIndex = 2;

// Up to this many sample lock requests are kept for each row range.
constexpr int kMaxSampleLockRequests = 20;

constexpr int kMaxTagLength = 50;

absl::Duration IntervalLength(StatsInterval interval) {
  switch (interval) {
    case StatsInterval::kMinute:
      return absl::Minutes(1);
    case StatsInterval::kTenMinutes:
      return absl::Minutes(10);
    case StatsInterval::kHour:
      return absl::Hours(1);
  }
}

int64_t Fingerprint(absl::string_view text) {
  return static_cast<int64_t>(farmhash::Fingerprint64(text));
}

// Truncates `text` to SystemStatsCollector::kMaxTextBytes without splitting a
// UTF-8 character.
absl::string_view TruncateText(absl::string_view text, bool* truncated) {
  *truncated = text.size() > SystemStatsCollector::kMaxTextBytes;
  if (!*truncated) {
    return text;
  }
  size_t size = SystemStatsCollector::kMaxTextBytes;
  while (size > 0 && (static_cast<unsigned char>(text[size]) & 0xC0) == 0x80) {
    --size;
  }
  return text.substr(0, size);
}

googlesql::Value Average(double sum, int64_t count) {
  return count == 0 ? NullDouble() : Double(sum / count);
}

googlesql::Value AverageSeconds(absl::Duration sum, int64_t count) {
  return Average(absl::ToDoubleSeconds(sum), count);
}

// Returns the columns of `columns_by_table` as sorted "Table.column" names.
std::vector<std::string> QualifiedColumns(
    const absl::btree_map<std::string, absl::btree_set<std::string>>&
        columns_by_table) {
  std::vector<std::string> columns;
  for (const auto& [table, table_columns] : columns_by_table) {
    for (const std::string& column : table_columns) {
      columns.push_back(absl::StrCat(table, ".", column));
    }
  }
  return columns;
}

std::vector<std::string> QualifiedColumns(absl::string_view table,
                                          std::vector<std::string> columns) {
  for (std::string& column : columns) {
    column = absl::StrCat(table, ".", column);
  }
  std::sort(columns.begin(), columns.end());
  columns.erase(std::unique(columns.begin(), columns.end()), columns.end());
  return columns;
}

}  // namespace

absl::Time IntervalEnd(absl::Time time, StatsInterval interval) {
  const absl::Duration length = IntervalLength(interval);
  return absl::UnixEpoch() + absl::Floor(time - absl::UnixEpoch(), length) +
         length;
}

absl::Duration StatsRetention(StatsInterval interval) {
  switch (interval) {
    case StatsInterval::kMinute:
      return absl::Hours(6);
    case StatsInterval::kTenMinutes:
      return absl::Hours(4 * 24);
    case StatsInterval::kHour:
      return absl::Hours(30 * 24);
  }
}

std::string FormatKeyValue(const googlesql::Value& value) {
  if (!value.is_null() && value.type()->IsString()) {
    return value.string_value();
  }
  return value.DebugString();
}

std::string NormalizeTag(absl::string_view tag) {
  std::string normalized;
  for (char c : tag) {
    if (c >= 32 && c <= 126 && (c != '_' || !normalized.empty())) {
      normalized.push_back(c);
    }
  }
  if (normalized.size() > kMaxTagLength) {
    normalized.resize(kMaxTagLength);
  }
  return normalized;
}

namespace {

// Statistics of the executions of one statement shape, or of all statements.
struct QueryAccumulator {
  std::shared_ptr<const std::string> text;
  bool text_truncated = false;
  std::string request_tag;
  bool partitioned = false;
  // Successful executions, which the averages describe.
  int64_t execution_count = 0;
  int64_t read_write_execution_count = 0;
  absl::Duration latency;
  absl::Duration cpu_time;
  absl::Duration plan_creation_time;
  int64_t rows = 0;
  int64_t bytes = 0;
  int64_t rows_scanned = 0;
  int64_t rows_written = 0;
  int64_t bytes_written = 0;
  LatencyDistribution distribution;
  // Failed executions.
  int64_t failed_count = 0;
  int64_t cancelled_count = 0;
  int64_t timed_out_count = 0;
  absl::Duration failed_latency;
  absl::Duration failed_cpu_time;

  void Add(const QueryExecution& query) {
    if (query.status != absl::StatusCode::kOk) {
      ++failed_count;
      cancelled_count += query.status == absl::StatusCode::kCancelled;
      timed_out_count += query.status == absl::StatusCode::kDeadlineExceeded;
      failed_latency += query.latency;
      failed_cpu_time += query.cpu_time;
      return;
    }
    ++execution_count;
    read_write_execution_count += query.in_read_write_transaction;
    latency += query.latency;
    cpu_time += query.cpu_time;
    plan_creation_time += query.plan_creation_time;
    rows += query.rows_returned;
    bytes += query.bytes_returned;
    rows_scanned += query.rows_scanned;
    rows_written += query.rows_written;
    bytes_written += query.bytes_written;
    distribution.Add(query.latency);
  }

  // TOP tables rank statements by CPU time.
  auto Rank() const {
    return std::make_tuple(cpu_time + failed_cpu_time, latency + failed_latency,
                           execution_count + failed_count);
  }

  // Adds the columns shared by the TOP and TOTAL tables.
  void AddColumns(SpannerSysRow& row) const {
    const int64_t n = execution_count;
    row["EXECUTION_COUNT"] = Int64(n);
    row["AVG_LATENCY_SECONDS"] = AverageSeconds(latency, n);
    row["AVG_ROWS"] = Average(rows, n);
    row["AVG_BYTES"] = Average(bytes, n);
    row["AVG_ROWS_SCANNED"] = Average(rows_scanned, n);
    row["AVG_CPU_SECONDS"] = AverageSeconds(cpu_time, n);
    row["CANCELLED_OR_DISCONNECTED_EXECUTION_COUNT"] = Int64(cancelled_count);
    row["TIMED_OUT_EXECUTION_COUNT"] = Int64(timed_out_count);
    row["ALL_FAILED_EXECUTION_COUNT"] = Int64(failed_count);
    row["ALL_FAILED_AVG_LATENCY_SECONDS"] =
        AverageSeconds(failed_latency, failed_count);
    row["AVG_BYTES_WRITTEN"] = Average(bytes_written, n);
    row["AVG_ROWS_WRITTEN"] = Average(rows_written, n);
    if (n > 0) {
      row["LATENCY_DISTRIBUTION"] = distribution.ToValue();
      row["LATENCY_DISTRIBUTION_JSON_STRING"] = String(distribution.ToJson());
    }
    row["RUN_IN_RW_TRANSACTION_EXECUTION_COUNT"] =
        Int64(read_write_execution_count);
    row["AVG_QUERY_PLAN_CREATION_TIME_SECS"] =
        AverageSeconds(plan_creation_time, n);
    // The emulator has no memory accounting, file system, remote calls, spools
    // or disk I/O to measure.
    for (const char* column :
         {"AVG_MEMORY_PEAK_USAGE_BYTES", "AVG_MEMORY_USAGE_PERCENTAGE",
          "AVG_FILESYSTEM_DELAY_SECS", "AVG_REMOTE_SERVER_CALLS",
          "AVG_ROWS_SPOOLED", "AVG_DISK_IO_COST"}) {
      row[column] = Average(0, n);
    }
  }

  SpannerSysRow TopRow(int64_t fingerprint) const {
    SpannerSysRow row;
    AddColumns(row);
    row["TEXT"] = String(*text);
    row["TEXT_TRUNCATED"] = Bool(text_truncated);
    row["TEXT_FINGERPRINT"] = Int64(fingerprint);
    row["REQUEST_TAG"] = String(request_tag);
    row["STATEMENT_COUNT"] = Int64(execution_count);
    row["QUERY_TYPE"] = String(partitioned ? "PARTITIONED_QUERY" : "QUERY");
    row["QUERY_OPTIMIZER_VERSIONS"] = Int64Array({kDefaultOptimizerVersion});
    row["STATISTICS_PACKAGE_NAMES"] = StringArray(std::vector<std::string>{});
    return row;
  }

  SpannerSysRow TotalRow() const {
    SpannerSysRow row;
    AddColumns(row);
    return row;
  }
};

// Statistics of the reads of one read shape, or of all reads.
struct ReadAccumulator {
  std::vector<std::string> read_columns;
  std::string request_tag;
  bool partitioned = false;
  int64_t execution_count = 0;
  int64_t read_write_execution_count = 0;
  int64_t rows = 0;
  int64_t bytes = 0;
  absl::Duration cpu_time;

  void Add(const ReadExecution& read) {
    ++execution_count;
    read_write_execution_count += read.in_read_write_transaction;
    rows += read.rows;
    bytes += read.bytes;
    cpu_time += read.cpu_time;
  }

  // TOP tables rank reads by CPU time.
  auto Rank() const { return std::make_tuple(cpu_time, rows, execution_count); }

  void AddColumns(SpannerSysRow& row) const {
    const int64_t n = execution_count;
    row["EXECUTION_COUNT"] = Int64(n);
    row["AVG_ROWS"] = Average(rows, n);
    row["AVG_BYTES"] = Average(bytes, n);
    row["AVG_CPU_SECONDS"] = AverageSeconds(cpu_time, n);
    // The emulator does not measure how long reads wait for locks. Its reads
    // never wait for clients or leaders, and do no disk I/O.
    for (const char* column :
         {"AVG_LOCKING_DELAY_SECONDS", "AVG_CLIENT_WAIT_SECONDS",
          "AVG_LEADER_REFRESH_DELAY_SECONDS", "AVG_DISK_IO_COST"}) {
      row[column] = Average(0, n);
    }
    row["RUN_IN_RW_TRANSACTION_EXECUTION_COUNT"] =
        Int64(read_write_execution_count);
  }

  SpannerSysRow TopRow(int64_t fingerprint) const {
    SpannerSysRow row;
    AddColumns(row);
    row["READ_COLUMNS"] = StringArray(read_columns);
    row["FPRINT"] = Int64(fingerprint);
    row["REQUEST_TAG"] = String(request_tag);
    row["READ_TYPE"] = String(partitioned ? "PARTITIONED_READ" : "READ");
    return row;
  }

  SpannerSysRow TotalRow() const {
    SpannerSysRow row;
    AddColumns(row);
    return row;
  }
};

// Statistics of the attempts of one transaction shape, or of all read-write
// transactions.
struct TransactionAccumulator {
  std::string transaction_tag;
  AccessFootprint footprint;
  int64_t attempt_count = 0;
  int64_t commit_attempt_count = 0;
  int64_t commit_failed_precondition_count = 0;
  int64_t commit_abort_count = 0;
  int64_t commit_retry_count = 0;
  int64_t serializable_pessimistic_count = 0;
  int64_t serializable_optimistic_count = 0;
  int64_t repeatable_read_optimistic_count = 0;
  absl::Duration total_latency;
  absl::Duration commit_latency;
  int64_t bytes = 0;
  LatencyDistribution distribution;

  void Add(const TransactionAttempt& attempt) {
    footprint.Merge(attempt.footprint);
    ++attempt_count;
    commit_attempt_count += attempt.commit_attempted;
    commit_failed_precondition_count +=
        attempt.outcome ==
        TransactionAttempt::Outcome::kCommitFailedPrecondition;
    commit_abort_count +=
        attempt.outcome == TransactionAttempt::Outcome::kAborted;
    commit_retry_count += attempt.retry;
    switch (attempt.concurrency_mode) {
      case TransactionAttempt::ConcurrencyMode::kSerializablePessimistic:
        ++serializable_pessimistic_count;
        break;
      case TransactionAttempt::ConcurrencyMode::kSerializableOptimistic:
        ++serializable_optimistic_count;
        break;
      case TransactionAttempt::ConcurrencyMode::kRepeatableReadOptimistic:
        ++repeatable_read_optimistic_count;
        break;
      case TransactionAttempt::ConcurrencyMode::kRepeatableReadPessimistic:
        break;
    }
    total_latency += attempt.total_latency;
    commit_latency += attempt.commit_latency;
    bytes += attempt.footprint.bytes_written;
    distribution.Add(attempt.total_latency);
  }

  // TOP tables rank transactions by latency, commit attempts and bytes
  // written.
  auto Rank() const {
    return std::make_tuple(total_latency, commit_attempt_count, bytes);
  }

  void AddColumns(SpannerSysRow& row) const {
    row["ATTEMPT_COUNT"] = Int64(attempt_count);
    row["COMMIT_ATTEMPT_COUNT"] = Int64(commit_attempt_count);
    row["COMMIT_FAILED_PRECONDITION_COUNT"] =
        Int64(commit_failed_precondition_count);
    row["COMMIT_ABORT_COUNT"] = Int64(commit_abort_count);
    row["COMMIT_RETRY_COUNT"] = Int64(commit_retry_count);
    row["SERIALIZABLE_PESSIMISTIC_TXN_COUNT"] =
        Int64(serializable_pessimistic_count);
    row["SERIALIZABLE_OPTIMISTIC_TXN_COUNT"] =
        Int64(serializable_optimistic_count);
    row["REPEATABLE_READ_OPTIMISTIC_TXN_COUNT"] =
        Int64(repeatable_read_optimistic_count);
    // Every emulator commit has a single participant.
    row["AVG_PARTICIPANTS"] =
        Average(commit_attempt_count, commit_attempt_count);
    row["AVG_TOTAL_LATENCY_SECONDS"] =
        AverageSeconds(total_latency, attempt_count);
    row["AVG_COMMIT_LATENCY_SECONDS"] =
        AverageSeconds(commit_latency, commit_attempt_count);
    row["AVG_BYTES"] = Average(bytes, attempt_count);
    row["TOTAL_LATENCY_DISTRIBUTION"] = distribution.ToValue();
    row["TOTAL_LATENCY_DISTRIBUTION_JSON_STRING"] =
        String(distribution.ToJson());

    const googlesql::ArrayType* operations_type = OperationsByTableType();
    std::vector<googlesql::Value> operations;
    nlohmann::json operations_json = nlohmann::json::array();
    for (const auto& [table, writes] : footprint.writes) {
      operations.push_back(googlesql::Value::Struct(
          operations_type->element_type()->AsStruct(),
          {String(table), Int64(writes.rows), Int64(writes.bytes)}));
      operations_json.push_back({{"TABLE_NAME", table},
                                 {"INSERT_OR_UPDATE_COUNT", writes.rows},
                                 {"INSERT_OR_UPDATE_BYTES", writes.bytes}});
    }
    row["OPERATIONS_BY_TABLE"] =
        googlesql::values::Array(operations_type, operations);
    row["OPERATIONS_BY_TABLE_JSON_STRING"] = String(operations_json.dump());
  }

  SpannerSysRow TopRow(int64_t fingerprint) const {
    SpannerSysRow row;
    AddColumns(row);
    row["FPRINT"] = Int64(fingerprint);
    row["TRANSACTION_TAG"] = String(transaction_tag);
    row["READ_COLUMNS"] = StringArray(QualifiedColumns(footprint.read_columns));
    row["WRITE_CONSTRUCTIVE_COLUMNS"] =
        StringArray(QualifiedColumns(footprint.written_columns));
    row["WRITE_DELETE_TABLES"] = StringArray(std::vector<std::string>(
        footprint.deleted_tables.begin(), footprint.deleted_tables.end()));
    return row;
  }

  SpannerSysRow TotalRow() const {
    SpannerSysRow row;
    AddColumns(row);
    return row;
  }
};

// A sample of a lock request that took part in a conflict.
struct SampleLockRequest {
  // Empty for a lock on whole rows.
  ColumnID column_id;
  bool exclusive = false;
  std::string transaction_tag;

  bool operator==(const SampleLockRequest& other) const {
    return column_id == other.column_id && exclusive == other.exclusive &&
           transaction_tag == other.transaction_tag;
  }
};

// The lock conflicts of one row range, or of all row ranges.
struct LockAccumulator {
  TableID table_id;
  // The formatted values of the key where the row range starts.
  std::vector<std::string> start_key;
  bool is_range = false;
  int64_t conflict_count = 0;
  absl::Duration lock_wait;
  std::vector<SampleLockRequest> samples;

  void Add(const std::vector<SampleLockRequest>& conflict_samples,
           absl::Duration conflict_lock_wait) {
    ++conflict_count;
    lock_wait += conflict_lock_wait;
    for (const SampleLockRequest& sample : conflict_samples) {
      if (samples.size() < kMaxSampleLockRequests &&
          std::find(samples.begin(), samples.end(), sample) == samples.end()) {
        samples.push_back(sample);
      }
    }
  }

  // TOP tables rank row ranges by lock wait, then by conflicts.
  auto Rank() const { return std::make_tuple(lock_wait, conflict_count); }

  SpannerSysRow TopRow(int64_t fingerprint, const Schema* schema) const;

  SpannerSysRow TotalRow() const {
    return {{"TOTAL_LOCK_WAIT_SECONDS",
             Double(absl::ToDoubleSeconds(lock_wait))}};
  }
};

// Returns the table (or index) named by `table_id` in `schema`, if any.
const Table* FindTableById(const Schema* schema, const TableID& table_id,
                           std::string* name) {
  if (schema == nullptr) {
    return nullptr;
  }
  for (const Table* table : schema->tables()) {
    if (table->id() == table_id) {
      *name = table->Name();
      return table;
    }
    for (const Index* index : table->indexes()) {
      if (index->index_data_table() != nullptr &&
          index->index_data_table()->id() == table_id) {
        *name = index->Name();
        return index->index_data_table();
      }
    }
  }
  return nullptr;
}

SpannerSysRow LockAccumulator::TopRow(int64_t fingerprint,
                                      const Schema* schema) const {
  std::string table_name = table_id;
  const Table* table = FindTableById(schema, table_id, &table_name);

  // Formats the start key like Cloud Spanner, e.g. Singers(2) for a key and
  // Albums(2,1+) for the start of a key range. A prefix of the primary key
  // also stands for a range of rows.
  const bool range =
      is_range ||
      (table != nullptr && start_key.size() < table->primary_key().size());
  std::string row_range_start_key = absl::StrCat(
      table_name, "(", absl::StrJoin(start_key, ","), range ? "+" : "", ")");

  const googlesql::ArrayType* samples_type = SampleLockRequestsType();
  std::vector<googlesql::Value> sample_values;
  nlohmann::json samples_json = nlohmann::json::array();
  for (const SampleLockRequest& sample : samples) {
    // Whole-row locks are reported on the _exists pseudo-column.
    std::string column_name =
        sample.column_id.empty() ? "_exists" : sample.column_id;
    if (table != nullptr && !sample.column_id.empty()) {
      for (const Column* column : table->columns()) {
        if (column->id() == sample.column_id) {
          column_name = column->Name();
          break;
        }
      }
    }
    const std::string column = absl::StrCat(table_name, ".", column_name);
    const std::string lock_mode =
        sample.exclusive ? "Exclusive" : "ReaderShared";
    sample_values.push_back(googlesql::Value::Struct(
        samples_type->element_type()->AsStruct(),
        {String(column), String(lock_mode), String(sample.transaction_tag)}));
    samples_json.push_back({{"COLUMN", column},
                            {"LOCK_MODE", lock_mode},
                            {"TRANSACTION_TAG", sample.transaction_tag}});
  }
  return {
      {"ROW_RANGE_START_KEY", Bytes(row_range_start_key)},
      {"LOCK_WAIT_SECONDS", Double(absl::ToDoubleSeconds(lock_wait))},
      {"SAMPLE_LOCK_REQUESTS",
       googlesql::values::Array(samples_type, sample_values)},
      {"SAMPLE_LOCK_REQUESTS_JSON_STRING", String(samples_json.dump())},
  };
}

// Operation counts of one table.
struct TableOperationCounts {
  int64_t read_query_count = 0;
  int64_t write_count = 0;
  int64_t delete_count = 0;
};

// Operation counts of one column.
struct ColumnOperationCounts {
  int64_t query_count = 0;
  int64_t read_count = 0;
  int64_t write_count = 0;
};

// The table and column operations of one interval.
struct OperationCounts {
  absl::btree_map<std::string, TableOperationCounts> tables;
  absl::btree_map<std::pair<std::string, std::string>, ColumnOperationCounts>
      columns;
};

// Statistics of one interval.
template <typename Accumulator>
struct IntervalStats {
  // The highest-ranked shapes, keyed by fingerprint.
  absl::flat_hash_map<int64_t, Accumulator> top;
  // All executions, including those that did not rank high enough for `top`.
  Accumulator total;
};

// Statistics by interval end, for each of kIntervals.
template <typename Stats>
using Series = std::array<absl::btree_map<absl::Time, Stats>, 3>;

// Updates the statistics of every interval that contains `time` with
// `update`, which is applied to the TOTAL accumulator and to the TOP
// accumulator of `fingerprint`. When an interval already tracks kTopN shapes,
// a new shape replaces the lowest-ranked one only if it ranks higher.
template <typename Accumulator, typename Update>
void Record(Series<IntervalStats<Accumulator>>& series, absl::Time time,
            int64_t fingerprint, const Update& update) {
  for (int i = 0; i < kIntervals.size(); ++i) {
    IntervalStats<Accumulator>& stats =
        series[i][IntervalEnd(time, kIntervals[i])];
    update(stats.total);
    auto it = stats.top.find(fingerprint);
    if (it != stats.top.end()) {
      update(it->second);
      continue;
    }
    Accumulator candidate;
    update(candidate);
    if (stats.top.size() >= SystemStatsCollector::kTopN) {
      auto lowest = std::min_element(
          stats.top.begin(), stats.top.end(), [](const auto& a, const auto& b) {
            return a.second.Rank() < b.second.Rank();
          });
      if (!(lowest->second.Rank() < candidate.Rank())) {
        continue;
      }
      stats.top.erase(lowest);
    }
    stats.top.emplace(fingerprint, std::move(candidate));
  }
}

// Returns true if statistics of an interval that ends at `interval_end` are
// visible at time `now`.
bool IsVisible(absl::Time interval_end, int interval_index, absl::Time now,
               bool include_open_intervals) {
  return interval_end >= now - StatsRetention(kIntervals[interval_index]) &&
         (include_open_intervals || interval_end <= now);
}

// Appends the TOP or TOTAL rows of `series` for the interval `interval_index`.
// `top_row` builds the row of a TOP entry from its fingerprint and
// accumulator.
template <typename Accumulator, typename TopRow>
void AppendRows(const Series<IntervalStats<Accumulator>>& series,
                int interval_index, bool top, absl::Time now,
                bool include_open_intervals, const TopRow& top_row,
                std::vector<SpannerSysRow>* rows) {
  for (const auto& [interval_end, stats] : series[interval_index]) {
    if (!IsVisible(interval_end, interval_index, now,
                   include_open_intervals)) {
      continue;
    }
    if (!top) {
      rows->push_back(stats.total.TotalRow());
      (*rows).back()["INTERVAL_END"] = Timestamp(interval_end);
      continue;
    }
    std::vector<std::pair<int64_t, const Accumulator*>> entries;
    for (const auto& [fingerprint, accumulator] : stats.top) {
      entries.emplace_back(fingerprint, &accumulator);
    }
    std::sort(entries.begin(), entries.end(),
              [](const auto& a, const auto& b) {
                return std::make_tuple(b.second->Rank(), a.first) <
                       std::make_tuple(a.second->Rank(), b.first);
              });
    for (const auto& [fingerprint, accumulator] : entries) {
      rows->push_back(top_row(fingerprint, *accumulator));
      (*rows).back()["INTERVAL_END"] = Timestamp(interval_end);
    }
  }
}

// The statistics tables, other than the per-interval suffix.
enum class StatsTable {
  kQueryStatsTop,
  kQueryStatsTotal,
  kReadStatsTop,
  kReadStatsTotal,
  kTxnStatsTop,
  kTxnStatsTotal,
  kLockStatsTop,
  kLockStatsTotal,
  kTableOperationsStats,
  kColumnOperationsStats,
  kOldestActiveQueries,
  kActiveQueriesSummary,
  kActivePartitionedDmls,
  kTableSizes,
  kRowDeletionPolicies,
  kUserSplitPoints,
};

struct ParsedTableName {
  StatsTable table;
  // The index into kIntervals, for interval-based tables.
  int interval_index = -1;
};

std::optional<ParsedTableName> ParseTableName(absl::string_view name) {
  if (name == "OLDEST_ACTIVE_QUERIES") {
    return ParsedTableName{StatsTable::kOldestActiveQueries};
  }
  if (name == "ACTIVE_QUERIES_SUMMARY") {
    return ParsedTableName{StatsTable::kActiveQueriesSummary};
  }
  if (name == "ACTIVE_PARTITIONED_DMLS") {
    return ParsedTableName{StatsTable::kActivePartitionedDmls};
  }
  if (name == "TABLE_SIZES_STATS_1HOUR") {
    return ParsedTableName{StatsTable::kTableSizes, kHourIntervalIndex};
  }
  if (name == "ROW_DELETION_POLICIES") {
    return ParsedTableName{StatsTable::kRowDeletionPolicies};
  }
  if (name == "USER_SPLIT_POINTS") {
    return ParsedTableName{StatsTable::kUserSplitPoints};
  }
  static constexpr std::pair<absl::string_view, StatsTable> kPrefixes[] = {
      {"QUERY_STATS_TOP_", StatsTable::kQueryStatsTop},
      {"QUERY_STATS_TOTAL_", StatsTable::kQueryStatsTotal},
      {"READ_STATS_TOP_", StatsTable::kReadStatsTop},
      {"READ_STATS_TOTAL_", StatsTable::kReadStatsTotal},
      {"TXN_STATS_TOP_", StatsTable::kTxnStatsTop},
      {"TXN_STATS_TOTAL_", StatsTable::kTxnStatsTotal},
      {"LOCK_STATS_TOP_", StatsTable::kLockStatsTop},
      {"LOCK_STATS_TOTAL_", StatsTable::kLockStatsTotal},
      {"TABLE_OPERATIONS_STATS_", StatsTable::kTableOperationsStats},
      {"COLUMN_OPERATIONS_STATS_", StatsTable::kColumnOperationsStats},
  };
  for (const auto& [prefix, table] : kPrefixes) {
    absl::string_view suffix = name;
    if (!absl::ConsumePrefix(&suffix, prefix)) {
      continue;
    }
    for (int i = 0; i < kIntervalSuffixes.size(); ++i) {
      if (suffix == kIntervalSuffixes[i]) {
        return ParsedTableName{table, i};
      }
    }
  }
  return std::nullopt;
}

// The persistence file format. Statistics are saved as JSON, with durations
// in nanoseconds and interval ends in seconds since the Unix epoch.
using Json = nlohmann::json;
constexpr int kPersistenceVersion = 1;

Json DurationToJson(absl::Duration duration) {
  return absl::ToInt64Nanoseconds(duration);
}

absl::Duration DurationFromJson(const Json& json) {
  return absl::Nanoseconds(json.get<int64_t>());
}

Json ToJson(const LatencyDistribution& distribution) {
  return {{"count", distribution.count()},
          {"mean", distribution.mean()},
          {"sum_of_squared_deviation", distribution.sum_of_squared_deviation()},
          {"bucket_counts", distribution.bucket_counts()}};
}

LatencyDistribution LatencyDistributionFromJson(const Json& json) {
  return LatencyDistribution(
      json.at("count").get<int64_t>(), json.at("mean").get<double>(),
      json.at("sum_of_squared_deviation").get<double>(),
      json.at("bucket_counts").get<std::vector<int64_t>>());
}

Json ToJson(const absl::btree_map<std::string, absl::btree_set<std::string>>&
                columns_by_table) {
  Json json = Json::object();
  for (const auto& [table, columns] : columns_by_table) {
    json[table] = std::vector<std::string>(columns.begin(), columns.end());
  }
  return json;
}

absl::btree_map<std::string, absl::btree_set<std::string>>
ColumnsByTableFromJson(const Json& json) {
  absl::btree_map<std::string, absl::btree_set<std::string>> columns_by_table;
  for (const auto& [table, columns] : json.items()) {
    for (const Json& column : columns) {
      columns_by_table[table].insert(column.get<std::string>());
    }
  }
  return columns_by_table;
}

Json ToJson(const AccessFootprint& footprint) {
  Json writes = Json::object();
  for (const auto& [table, table_writes] : footprint.writes) {
    writes[table] = {table_writes.rows, table_writes.bytes};
  }
  return {{"read_columns", ToJson(footprint.read_columns)},
          {"written_columns", ToJson(footprint.written_columns)},
          {"deleted_tables",
           std::vector<std::string>(footprint.deleted_tables.begin(),
                                    footprint.deleted_tables.end())},
          {"writes", writes},
          {"bytes_written", footprint.bytes_written}};
}

AccessFootprint AccessFootprintFromJson(const Json& json) {
  AccessFootprint footprint;
  footprint.read_columns = ColumnsByTableFromJson(json.at("read_columns"));
  footprint.written_columns =
      ColumnsByTableFromJson(json.at("written_columns"));
  for (const Json& table : json.at("deleted_tables")) {
    footprint.deleted_tables.insert(table.get<std::string>());
  }
  for (const auto& [table, table_writes] : json.at("writes").items()) {
    footprint.writes[table] = {table_writes.at(0).get<int64_t>(),
                               table_writes.at(1).get<int64_t>()};
  }
  footprint.bytes_written = json.at("bytes_written").get<int64_t>();
  return footprint;
}

// Statement texts are saved once per fingerprint rather than with each
// accumulator, see SystemStatsCollector::Save().
Json ToJson(const QueryAccumulator& query) {
  return {{"text_truncated", query.text_truncated},
          {"request_tag", query.request_tag},
          {"partitioned", query.partitioned},
          {"execution_count", query.execution_count},
          {"read_write_execution_count", query.read_write_execution_count},
          {"latency", DurationToJson(query.latency)},
          {"cpu_time", DurationToJson(query.cpu_time)},
          {"plan_creation_time", DurationToJson(query.plan_creation_time)},
          {"rows", query.rows},
          {"bytes", query.bytes},
          {"rows_scanned", query.rows_scanned},
          {"rows_written", query.rows_written},
          {"bytes_written", query.bytes_written},
          {"distribution", ToJson(query.distribution)},
          {"failed_count", query.failed_count},
          {"cancelled_count", query.cancelled_count},
          {"timed_out_count", query.timed_out_count},
          {"failed_latency", DurationToJson(query.failed_latency)},
          {"failed_cpu_time", DurationToJson(query.failed_cpu_time)}};
}

void FromJson(const Json& json, QueryAccumulator* query) {
  query->text_truncated = json.at("text_truncated").get<bool>();
  query->request_tag = json.at("request_tag").get<std::string>();
  query->partitioned = json.at("partitioned").get<bool>();
  query->execution_count = json.at("execution_count").get<int64_t>();
  query->read_write_execution_count =
      json.at("read_write_execution_count").get<int64_t>();
  query->latency = DurationFromJson(json.at("latency"));
  query->cpu_time = DurationFromJson(json.at("cpu_time"));
  query->plan_creation_time = DurationFromJson(json.at("plan_creation_time"));
  query->rows = json.at("rows").get<int64_t>();
  query->bytes = json.at("bytes").get<int64_t>();
  query->rows_scanned = json.at("rows_scanned").get<int64_t>();
  query->rows_written = json.at("rows_written").get<int64_t>();
  query->bytes_written = json.at("bytes_written").get<int64_t>();
  query->distribution = LatencyDistributionFromJson(json.at("distribution"));
  query->failed_count = json.at("failed_count").get<int64_t>();
  query->cancelled_count = json.at("cancelled_count").get<int64_t>();
  query->timed_out_count = json.at("timed_out_count").get<int64_t>();
  query->failed_latency = DurationFromJson(json.at("failed_latency"));
  query->failed_cpu_time = DurationFromJson(json.at("failed_cpu_time"));
}

Json ToJson(const ReadAccumulator& read) {
  return {{"read_columns", read.read_columns},
          {"request_tag", read.request_tag},
          {"partitioned", read.partitioned},
          {"execution_count", read.execution_count},
          {"read_write_execution_count", read.read_write_execution_count},
          {"rows", read.rows},
          {"bytes", read.bytes},
          {"cpu_time", DurationToJson(read.cpu_time)}};
}

void FromJson(const Json& json, ReadAccumulator* read) {
  read->read_columns =
      json.at("read_columns").get<std::vector<std::string>>();
  read->request_tag = json.at("request_tag").get<std::string>();
  read->partitioned = json.at("partitioned").get<bool>();
  read->execution_count = json.at("execution_count").get<int64_t>();
  read->read_write_execution_count =
      json.at("read_write_execution_count").get<int64_t>();
  read->rows = json.at("rows").get<int64_t>();
  read->bytes = json.at("bytes").get<int64_t>();
  read->cpu_time = DurationFromJson(json.at("cpu_time"));
}

Json ToJson(const TransactionAccumulator& transaction) {
  return {
      {"transaction_tag", transaction.transaction_tag},
      {"footprint", ToJson(transaction.footprint)},
      {"attempt_count", transaction.attempt_count},
      {"commit_attempt_count", transaction.commit_attempt_count},
      {"commit_failed_precondition_count",
       transaction.commit_failed_precondition_count},
      {"commit_abort_count", transaction.commit_abort_count},
      {"commit_retry_count", transaction.commit_retry_count},
      {"serializable_pessimistic_count",
       transaction.serializable_pessimistic_count},
      {"serializable_optimistic_count",
       transaction.serializable_optimistic_count},
      {"repeatable_read_optimistic_count",
       transaction.repeatable_read_optimistic_count},
      {"total_latency", DurationToJson(transaction.total_latency)},
      {"commit_latency", DurationToJson(transaction.commit_latency)},
      {"bytes", transaction.bytes},
      {"distribution", ToJson(transaction.distribution)}};
}

void FromJson(const Json& json, TransactionAccumulator* transaction) {
  transaction->transaction_tag = json.at("transaction_tag").get<std::string>();
  transaction->footprint = AccessFootprintFromJson(json.at("footprint"));
  transaction->attempt_count = json.at("attempt_count").get<int64_t>();
  transaction->commit_attempt_count =
      json.at("commit_attempt_count").get<int64_t>();
  transaction->commit_failed_precondition_count =
      json.at("commit_failed_precondition_count").get<int64_t>();
  transaction->commit_abort_count =
      json.at("commit_abort_count").get<int64_t>();
  transaction->commit_retry_count =
      json.at("commit_retry_count").get<int64_t>();
  transaction->serializable_pessimistic_count =
      json.at("serializable_pessimistic_count").get<int64_t>();
  transaction->serializable_optimistic_count =
      json.at("serializable_optimistic_count").get<int64_t>();
  transaction->repeatable_read_optimistic_count =
      json.at("repeatable_read_optimistic_count").get<int64_t>();
  transaction->total_latency = DurationFromJson(json.at("total_latency"));
  transaction->commit_latency = DurationFromJson(json.at("commit_latency"));
  transaction->bytes = json.at("bytes").get<int64_t>();
  transaction->distribution =
      LatencyDistributionFromJson(json.at("distribution"));
}

Json ToJson(const LockAccumulator& lock) {
  Json samples = Json::array();
  for (const SampleLockRequest& sample : lock.samples) {
    samples.push_back(
        {sample.column_id, sample.exclusive, sample.transaction_tag});
  }
  return {{"table_id", lock.table_id},
          {"start_key", lock.start_key},
          {"is_range", lock.is_range},
          {"conflict_count", lock.conflict_count},
          {"lock_wait", DurationToJson(lock.lock_wait)},
          {"samples", samples}};
}

void FromJson(const Json& json, LockAccumulator* lock) {
  lock->table_id = json.at("table_id").get<std::string>();
  lock->start_key = json.at("start_key").get<std::vector<std::string>>();
  lock->is_range = json.at("is_range").get<bool>();
  lock->conflict_count = json.at("conflict_count").get<int64_t>();
  lock->lock_wait = DurationFromJson(json.at("lock_wait"));
  for (const Json& sample : json.at("samples")) {
    lock->samples.push_back({sample.at(0).get<std::string>(),
                             sample.at(1).get<bool>(),
                             sample.at(2).get<std::string>()});
  }
}

Json ToJson(const OperationCounts& counts) {
  Json tables = Json::array();
  for (const auto& [table, table_counts] : counts.tables) {
    tables.push_back({table, table_counts.read_query_count,
                      table_counts.write_count, table_counts.delete_count});
  }
  Json columns = Json::array();
  for (const auto& [table_and_column, column_counts] : counts.columns) {
    columns.push_back({table_and_column.first, table_and_column.second,
                       column_counts.query_count, column_counts.read_count,
                       column_counts.write_count});
  }
  return {{"tables", tables}, {"columns", columns}};
}

void FromJson(const Json& json, OperationCounts* counts) {
  for (const Json& table : json.at("tables")) {
    counts->tables[table.at(0).get<std::string>()] = {
        table.at(1).get<int64_t>(), table.at(2).get<int64_t>(),
        table.at(3).get<int64_t>()};
  }
  for (const Json& column : json.at("columns")) {
    counts->columns[{column.at(0).get<std::string>(),
                     column.at(1).get<std::string>()}] = {
        column.at(2).get<int64_t>(), column.at(3).get<int64_t>(),
        column.at(4).get<int64_t>()};
  }
}

template <typename Accumulator>
Json ToJson(const IntervalStats<Accumulator>& stats) {
  Json top = Json::array();
  for (const auto& [fingerprint, accumulator] : stats.top) {
    top.push_back({fingerprint, ToJson(accumulator)});
  }
  return {{"total", ToJson(stats.total)}, {"top", top}};
}

template <typename Accumulator>
void FromJson(const Json& json, IntervalStats<Accumulator>* stats) {
  FromJson(json.at("total"), &stats->total);
  for (const Json& entry : json.at("top")) {
    FromJson(entry.at(1), &stats->top[entry.at(0).get<int64_t>()]);
  }
}

// Saves the statistics of each interval of `series` as an array of
// [interval end, statistics] pairs.
template <typename Stats>
Json SeriesToJson(const Series<Stats>& series) {
  Json json = Json::array();
  for (const auto& intervals : series) {
    Json saved_intervals = Json::array();
    for (const auto& [interval_end, stats] : intervals) {
      saved_intervals.push_back({absl::ToUnixSeconds(interval_end),
                                 ToJson(stats)});
    }
    json.push_back(saved_intervals);
  }
  return json;
}

template <typename Stats>
void SeriesFromJson(const Json& json, Series<Stats>* series) {
  for (int i = 0; i < kIntervals.size(); ++i) {
    for (const Json& interval : json.at(i)) {
      FromJson(interval.at(1),
               &(*series)[i][absl::FromUnixSeconds(
                   interval.at(0).get<int64_t>())]);
    }
  }
}

// Saves the table size samples as an array of [interval end, {name: [sum,
// number of samples]}] pairs.
Json TableSizesToJson(
    const absl::btree_map<
        absl::Time, absl::btree_map<std::string, std::pair<double, int64_t>>>&
        table_sizes) {
  Json json = Json::array();
  for (const auto& [interval_end, sizes] : table_sizes) {
    Json saved_sizes = Json::object();
    for (const auto& [name, total_and_samples] : sizes) {
      saved_sizes[name] = {total_and_samples.first, total_and_samples.second};
    }
    json.push_back({absl::ToUnixSeconds(interval_end), saved_sizes});
  }
  return json;
}

void TableSizesFromJson(
    const Json& json,
    absl::btree_map<absl::Time,
                    absl::btree_map<std::string, std::pair<double, int64_t>>>*
        table_sizes) {
  for (const Json& interval : json) {
    auto& sizes =
        (*table_sizes)[absl::FromUnixSeconds(interval.at(0).get<int64_t>())];
    for (const auto& [name, total_and_samples] : interval.at(1).items()) {
      sizes[name] = {total_and_samples.at(0).get<double>(),
                     total_and_samples.at(1).get<int64_t>()};
    }
  }
}

// Saves the last row deletion policy sweep of each table, with times in
// microseconds since the Unix epoch.
Json SweepsToJson(
    const absl::flat_hash_map<std::string, RowDeletionPolicySweep>& sweeps) {
  Json json = Json::object();
  for (const auto& [table, sweep] : sweeps) {
    Json saved = {
        {"processed_watermark", absl::ToUnixMicros(sweep.processed_watermark)},
        {"undeletable_rows", sweep.undeletable_rows},
    };
    if (sweep.min_undeletable_timestamp.has_value()) {
      saved["min_undeletable_timestamp"] =
          absl::ToUnixMicros(*sweep.min_undeletable_timestamp);
    }
    json[table] = saved;
  }
  return json;
}

void SweepsFromJson(
    const Json& json,
    absl::flat_hash_map<std::string, RowDeletionPolicySweep>* sweeps) {
  for (const auto& [table, saved] : json.items()) {
    RowDeletionPolicySweep& sweep = (*sweeps)[table];
    sweep.processed_watermark =
        absl::FromUnixMicros(saved.at("processed_watermark").get<int64_t>());
    sweep.undeletable_rows = saved.at("undeletable_rows").get<int64_t>();
    if (saved.contains("min_undeletable_timestamp")) {
      sweep.min_undeletable_timestamp = absl::FromUnixMicros(
          saved.at("min_undeletable_timestamp").get<int64_t>());
    }
  }
}

}  // namespace

struct SystemStatsCollector::State {
  Series<IntervalStats<QueryAccumulator>> queries;
  Series<IntervalStats<ReadAccumulator>> reads;
  Series<IntervalStats<TransactionAccumulator>> transactions;
  Series<IntervalStats<LockAccumulator>> locks;
  Series<OperationCounts> operations;
  // Statement texts by fingerprint, shared by the accumulators that use them.
  absl::flat_hash_map<int64_t, std::weak_ptr<const std::string>> texts;
  // Tags of the registered user read-write transactions.
  absl::flat_hash_map<TransactionID, std::string> transaction_tags;
  absl::btree_map<int64_t, ActiveQuery> active_queries;
  int64_t next_query_id = 1;
  absl::btree_map<int64_t, ActivePartitionedDml> active_partitioned_dmls;
  int64_t next_partitioned_dml_id = 1;
  // The sum and number of the size samples of each table and index, by hour
  // interval end and name.
  absl::btree_map<absl::Time,
                  absl::btree_map<std::string, std::pair<double, int64_t>>>
      table_sizes;
  // The last row deletion policy sweep of each table, by table name.
  absl::flat_hash_map<std::string, RowDeletionPolicySweep>
      row_deletion_policy_sweeps;
  // Split points by table name, index name and split key.
  absl::btree_map<std::tuple<std::string, std::string, std::string>,
                  UserSplitPoint>
      user_split_points;
  // The end of the latest minute interval with recorded statistics.
  absl::Time latest_minute_end = absl::InfinitePast();

  // Returns true if statistics recorded at `time` fall into a later minute
  // interval than the statistics recorded before, if any.
  bool StartsInterval(absl::Time time) {
    const absl::Time minute_end = IntervalEnd(time, StatsInterval::kMinute);
    const bool starts = latest_minute_end != absl::InfinitePast() &&
                        minute_end > latest_minute_end;
    latest_minute_end = std::max(latest_minute_end, minute_end);
    return starts;
  }

  // Returns the stored copy of the (truncated) statement text with the given
  // fingerprint.
  std::shared_ptr<const std::string> InternText(int64_t fingerprint,
                                                absl::string_view text) {
    std::weak_ptr<const std::string>& stored = texts[fingerprint];
    std::shared_ptr<const std::string> shared = stored.lock();
    if (shared == nullptr) {
      shared = std::make_shared<const std::string>(text);
      stored = shared;
    }
    return shared;
  }

  // Adds `footprint` to the operation counts of every interval that contains
  // `time`. Reads count as queries if `is_query`.
  void RecordOperations(absl::Time time, const AccessFootprint& footprint,
                        bool is_query) {
    for (int i = 0; i < kIntervals.size(); ++i) {
      OperationCounts& counts =
          operations[i][IntervalEnd(time, kIntervals[i])];
      for (const auto& [table, columns] : footprint.read_columns) {
        ++counts.tables[table].read_query_count;
        for (const std::string& column : columns) {
          ColumnOperationCounts& column_counts =
              counts.columns[{table, column}];
          ++(is_query ? column_counts.query_count : column_counts.read_count);
        }
      }
      for (const auto& [table, columns] : footprint.written_columns) {
        ++counts.tables[table].write_count;
        for (const std::string& column : columns) {
          ++counts.columns[{table, column}].write_count;
        }
      }
      for (const std::string& table : footprint.deleted_tables) {
        ++counts.tables[table].delete_count;
      }
    }
  }

  // Drops statistics that are older than their retention period.
  void Prune(absl::Time now) {
    bool pruned = false;
    auto prune = [&](auto& series) {
      for (int i = 0; i < kIntervals.size(); ++i) {
        const absl::Time cutoff = now - StatsRetention(kIntervals[i]);
        while (!series[i].empty() && series[i].begin()->first < cutoff) {
          series[i].erase(series[i].begin());
          pruned = true;
        }
      }
    };
    prune(queries);
    prune(reads);
    prune(transactions);
    prune(locks);
    prune(operations);
    table_sizes.erase(
        table_sizes.begin(),
        table_sizes.lower_bound(now - StatsRetention(StatsInterval::kHour)));
    if (pruned) {
      absl::erase_if(texts, [](const auto& entry) {
        return entry.second.expired();
      });
    }
  }
};

SystemStatsCollector::SystemStatsCollector()
    : state_(std::make_unique<State>()) {}

SystemStatsCollector::SystemStatsCollector(std::string persistence_path,
                                           absl::Time now)
    : persistence_path_(std::move(persistence_path)),
      state_(std::make_unique<State>()) {
  std::ifstream file(persistence_path_, std::ios::binary);
  if (!file) {
    return;
  }
  const std::string saved((std::istreambuf_iterator<char>(file)),
                          std::istreambuf_iterator<char>());
  const absl::Status status = Restore(saved, now);
  if (!status.ok()) {
    LOG(WARNING) << "Ignoring the SPANNER_SYS statistics saved in "
                 << persistence_path_ << ": " << status;
  }
}

SystemStatsCollector::~SystemStatsCollector() = default;

std::string SystemStatsCollector::Save() const {
  absl::MutexLock lock(mu_);
  // Accumulators share statement texts, so each text is saved once.
  Json texts = Json::object();
  for (const auto& intervals : state_->queries) {
    for (const auto& [interval_end, stats] : intervals) {
      for (const auto& [fingerprint, query] : stats.top) {
        texts[absl::StrCat(fingerprint)] = *query.text;
      }
    }
  }
  const Json saved = {
      {"version", kPersistenceVersion},
      {"texts", texts},
      {"queries", SeriesToJson(state_->queries)},
      {"reads", SeriesToJson(state_->reads)},
      {"transactions", SeriesToJson(state_->transactions)},
      {"locks", SeriesToJson(state_->locks)},
      {"operations", SeriesToJson(state_->operations)},
      {"table_sizes", TableSizesToJson(state_->table_sizes)},
      {"row_deletion_policy_sweeps",
       SweepsToJson(state_->row_deletion_policy_sweeps)},
  };
  return saved.dump(/*indent=*/-1, /*indent_char=*/' ', /*ensure_ascii=*/false,
                    Json::error_handler_t::replace);
}

absl::Status SystemStatsCollector::Restore(absl::string_view saved,
                                           absl::Time now) {
  auto state = std::make_unique<State>();
  try {
    const Json json = Json::parse(saved);
    const int version = json.at("version").get<int>();
    if (version != kPersistenceVersion) {
      return absl::DataLossError(
          absl::StrCat("Unsupported statistics version ", version));
    }
    SeriesFromJson(json.at("queries"), &state->queries);
    SeriesFromJson(json.at("reads"), &state->reads);
    SeriesFromJson(json.at("transactions"), &state->transactions);
    SeriesFromJson(json.at("locks"), &state->locks);
    SeriesFromJson(json.at("operations"), &state->operations);
    if (json.contains("table_sizes")) {
      TableSizesFromJson(json.at("table_sizes"), &state->table_sizes);
    }
    if (json.contains("row_deletion_policy_sweeps")) {
      SweepsFromJson(json.at("row_deletion_policy_sweeps"),
                     &state->row_deletion_policy_sweeps);
    }
    const Json& texts = json.at("texts");
    for (auto& intervals : state->queries) {
      for (auto& [interval_end, stats] : intervals) {
        for (auto& [fingerprint, query] : stats.top) {
          query.text = state->InternText(
              fingerprint,
              texts.at(absl::StrCat(fingerprint)).get<std::string>());
        }
      }
    }
  } catch (const Json::exception& error) {
    return absl::DataLossError(
        absl::StrCat("Malformed statistics: ", error.what()));
  }
  state->Prune(now);

  absl::MutexLock lock(mu_);
  state->transaction_tags = std::move(state_->transaction_tags);
  state->active_queries = std::move(state_->active_queries);
  state->next_query_id = state_->next_query_id;
  state->active_partitioned_dmls = std::move(state_->active_partitioned_dmls);
  state->next_partitioned_dml_id = state_->next_partitioned_dml_id;
  // Split points are saved with the database data, not with the statistics.
  state->user_split_points = std::move(state_->user_split_points);
  // A sweep recorded since the collector started is newer than a saved one.
  for (auto& [table, sweep] : state_->row_deletion_policy_sweeps) {
    state->row_deletion_policy_sweeps.insert_or_assign(table, sweep);
  }
  state_ = std::move(state);
  return absl::OkStatus();
}

void SystemStatsCollector::Persist() const {
  if (persistence_path_.empty()) {
    return;
  }
  absl::MutexLock lock(persist_mu_);
  const std::string saved = Save();
  // Replace the file atomically, so that a crash leaves either version.
  const std::string temporary_path = absl::StrCat(persistence_path_, ".tmp");
  {
    std::ofstream file(temporary_path, std::ios::binary | std::ios::trunc);
    // The directory is gone once the database was deleted.
    if (!file) {
      return;
    }
    file << saved;
    file.close();
    if (!file) {
      LOG(WARNING) << "Failed to save SPANNER_SYS statistics to "
                   << temporary_path;
      return;
    }
  }
  std::error_code error;
  std::filesystem::rename(temporary_path, persistence_path_, error);
  if (error) {
    LOG(WARNING) << "Failed to save SPANNER_SYS statistics to "
                 << persistence_path_ << ": " << error.message();
  }
}

void SystemStatsCollector::PersistIfIntervalEnded(absl::Time time) {
  if (persistence_path_.empty()) {
    return;
  }
  bool interval_ended;
  {
    absl::MutexLock lock(mu_);
    interval_ended = state_->StartsInterval(time);
  }
  if (interval_ended) {
    Persist();
  }
}

bool SystemStatsCollector::ServesTable(absl::string_view table_name) {
  return ParseTableName(table_name).has_value();
}

void SystemStatsCollector::RecordQuery(absl::Time end_time,
                                       const QueryExecution& query,
                                       const AccessFootprint& footprint) {
  PersistIfIntervalEnded(end_time);
  const std::string request_tag = NormalizeTag(query.request_tag);
  const int64_t fingerprint =
      Fingerprint(request_tag.empty() ? query.text : request_tag);
  bool text_truncated = false;
  const absl::string_view text = TruncateText(query.text, &text_truncated);

  absl::MutexLock lock(mu_);
  state_->Prune(end_time);
  std::shared_ptr<const std::string> stored_text =
      state_->InternText(fingerprint, text);
  Record(state_->queries, end_time, fingerprint,
         [&](QueryAccumulator& accumulator) {
           if (accumulator.text == nullptr) {
             accumulator.text = stored_text;
             accumulator.text_truncated = text_truncated;
             accumulator.request_tag = request_tag;
             accumulator.partitioned = query.partitioned;
           }
           accumulator.Add(query);
         });
  if (query.status == absl::StatusCode::kOk) {
    state_->RecordOperations(end_time, footprint, /*is_query=*/true);
  }
}

void SystemStatsCollector::RecordRead(absl::Time end_time,
                                      const ReadExecution& read) {
  PersistIfIntervalEnded(end_time);
  const std::string request_tag = NormalizeTag(read.request_tag);
  std::vector<std::string> read_columns =
      QualifiedColumns(read.table, read.columns);
  const int64_t fingerprint = Fingerprint(
      request_tag.empty() ? absl::StrJoin(read_columns, ",") : request_tag);
  AccessFootprint footprint;
  footprint.AddRead(read.table, read.columns);

  absl::MutexLock lock(mu_);
  state_->Prune(end_time);
  Record(state_->reads, end_time, fingerprint,
         [&](ReadAccumulator& accumulator) {
           if (accumulator.execution_count == 0) {
             accumulator.read_columns = read_columns;
             accumulator.request_tag = request_tag;
             accumulator.partitioned = read.partitioned;
           }
           accumulator.Add(read);
         });
  state_->RecordOperations(end_time, footprint, /*is_query=*/false);
}

void SystemStatsCollector::RecordMutations(absl::Time time,
                                           const AccessFootprint& footprint) {
  PersistIfIntervalEnded(time);
  absl::MutexLock lock(mu_);
  state_->Prune(time);
  state_->RecordOperations(time, footprint, /*is_query=*/false);
}

void SystemStatsCollector::RecordTransactionAttempt(
    absl::Time end_time, const TransactionAttempt& attempt) {
  PersistIfIntervalEnded(end_time);
  const std::string transaction_tag = NormalizeTag(attempt.transaction_tag);
  std::string shape = transaction_tag;
  if (shape.empty()) {
    const AccessFootprint& footprint = attempt.footprint;
    shape = absl::StrCat(
        "R:", absl::StrJoin(QualifiedColumns(footprint.read_columns), ","),
        ";W:", absl::StrJoin(QualifiedColumns(footprint.written_columns), ","),
        ";D:", absl::StrJoin(footprint.deleted_tables, ","));
  }
  const int64_t fingerprint = Fingerprint(shape);

  absl::MutexLock lock(mu_);
  state_->Prune(end_time);
  Record(state_->transactions, end_time, fingerprint,
         [&](TransactionAccumulator& accumulator) {
           if (accumulator.attempt_count == 0) {
             accumulator.transaction_tag = transaction_tag;
           }
           accumulator.Add(attempt);
         });
}

// Lock conflicts are recorded under the lock manager's mutex, so they never
// write the persistence file.
void SystemStatsCollector::RecordLockConflict(absl::Time time,
                                              const LockConflict& conflict) {
  const bool is_range = conflict.is_range;
  const int64_t fingerprint =
      Fingerprint(absl::StrCat(conflict.table_id, ":",
                               conflict.start_key.DebugString(),
                               is_range ? "+" : ""));

  absl::MutexLock lock(mu_);
  if (conflict.requests.empty() ||
      !state_->transaction_tags.contains(
          conflict.requests.front().transaction_id)) {
    return;
  }
  std::vector<SampleLockRequest> samples;
  for (const LockConflict::Request& request : conflict.requests) {
    auto tag = state_->transaction_tags.find(request.transaction_id);
    const std::string transaction_tag =
        tag == state_->transaction_tags.end() ? "" : tag->second;
    if (request.column_ids.empty()) {
      samples.push_back({"", request.exclusive, transaction_tag});
    }
    for (const ColumnID& column_id : request.column_ids) {
      samples.push_back({column_id, request.exclusive, transaction_tag});
    }
  }
  state_->Prune(time);
  Record(state_->locks, time, fingerprint,
         [&](LockAccumulator& accumulator) {
           if (accumulator.conflict_count == 0) {
             accumulator.table_id = conflict.table_id;
             for (int i = 0; i < conflict.start_key.NumColumns(); ++i) {
               accumulator.start_key.push_back(
                   FormatKeyValue(conflict.start_key.ColumnValue(i)));
             }
             accumulator.is_range = is_range;
           }
           accumulator.Add(samples, conflict.lock_wait);
         });
}

void SystemStatsCollector::RegisterTransaction(TransactionID id,
                                               absl::string_view tag) {
  absl::MutexLock lock(mu_);
  state_->transaction_tags[id] = NormalizeTag(tag);
}

void SystemStatsCollector::UnregisterTransaction(TransactionID id) {
  absl::MutexLock lock(mu_);
  state_->transaction_tags.erase(id);
}

int64_t SystemStatsCollector::StartQuery(ActiveQuery query) {
  absl::MutexLock lock(mu_);
  const int64_t query_id = state_->next_query_id++;
  state_->active_queries.emplace(query_id, std::move(query));
  return query_id;
}

void SystemStatsCollector::EndQuery(int64_t query_id) {
  absl::MutexLock lock(mu_);
  state_->active_queries.erase(query_id);
}

int64_t SystemStatsCollector::StartPartitionedDml(ActivePartitionedDml dml) {
  absl::MutexLock lock(mu_);
  const int64_t id = state_->next_partitioned_dml_id++;
  state_->active_partitioned_dmls.emplace(id, std::move(dml));
  return id;
}

void SystemStatsCollector::EndPartitionedDml(int64_t id) {
  absl::MutexLock lock(mu_);
  state_->active_partitioned_dmls.erase(id);
}

void SystemStatsCollector::RecordTableSizes(
    absl::Time time, const absl::flat_hash_map<std::string, double>& sizes) {
  // Samples are saved with the statistics that statements record; sampling
  // alone never writes the persistence file.
  absl::MutexLock lock(mu_);
  auto& interval_sizes =
      state_->table_sizes[IntervalEnd(time, StatsInterval::kHour)];
  for (const auto& [name, size] : sizes) {
    auto& [total, samples] = interval_sizes[name];
    total += size;
    ++samples;
  }
  // Forget the intervals that are no longer shown.
  const absl::Time oldest_shown =
      time - StatsRetention(StatsInterval::kHour);
  state_->table_sizes.erase(
      state_->table_sizes.begin(),
      state_->table_sizes.lower_bound(oldest_shown));
}

void SystemStatsCollector::RecordRowDeletionPolicySweep(
    absl::string_view table_name, const RowDeletionPolicySweep& sweep) {
  absl::MutexLock lock(mu_);
  state_->row_deletion_policy_sweeps[table_name] = sweep;
}

void SystemStatsCollector::AddUserSplitPoint(UserSplitPoint split_point) {
  absl::MutexLock lock(mu_);
  auto key = std::make_tuple(split_point.table_name, split_point.index_name,
                             split_point.split_key);
  state_->user_split_points.insert_or_assign(std::move(key),
                                             std::move(split_point));
}

std::vector<SpannerSysRow> SystemStatsCollector::Snapshot(
    absl::string_view table_name, absl::Time now, bool include_open_intervals,
    const Schema* schema) const {
  std::optional<ParsedTableName> parsed = ParseTableName(table_name);
  std::vector<SpannerSysRow> rows;
  if (!parsed.has_value()) {
    return rows;
  }
  const int interval = parsed->interval_index;

  absl::MutexLock lock(mu_);
  const State& state = *state_;
  switch (parsed->table) {
    case StatsTable::kQueryStatsTop:
    case StatsTable::kQueryStatsTotal:
      AppendRows(state.queries, interval,
                 parsed->table == StatsTable::kQueryStatsTop, now,
                 include_open_intervals,
                 [](int64_t fingerprint, const QueryAccumulator& accumulator) {
                   return accumulator.TopRow(fingerprint);
                 },
                 &rows);
      break;
    case StatsTable::kReadStatsTop:
    case StatsTable::kReadStatsTotal:
      AppendRows(state.reads, interval,
                 parsed->table == StatsTable::kReadStatsTop, now,
                 include_open_intervals,
                 [](int64_t fingerprint, const ReadAccumulator& accumulator) {
                   return accumulator.TopRow(fingerprint);
                 },
                 &rows);
      break;
    case StatsTable::kTxnStatsTop:
    case StatsTable::kTxnStatsTotal:
      AppendRows(state.transactions, interval,
                 parsed->table == StatsTable::kTxnStatsTop, now,
                 include_open_intervals,
                 [](int64_t fingerprint,
                    const TransactionAccumulator& accumulator) {
                   return accumulator.TopRow(fingerprint);
                 },
                 &rows);
      break;
    case StatsTable::kLockStatsTop:
    case StatsTable::kLockStatsTotal:
      AppendRows(state.locks, interval,
                 parsed->table == StatsTable::kLockStatsTop, now,
                 include_open_intervals,
                 [schema](int64_t fingerprint,
                          const LockAccumulator& accumulator) {
                   return accumulator.TopRow(fingerprint, schema);
                 },
                 &rows);
      break;
    case StatsTable::kTableOperationsStats:
    case StatsTable::kColumnOperationsStats:
      for (const auto& [interval_end, counts] : state.operations[interval]) {
        if (!IsVisible(interval_end, interval, now, include_open_intervals)) {
          continue;
        }
        if (parsed->table == StatsTable::kTableOperationsStats) {
          for (const auto& [table, table_counts] : counts.tables) {
            rows.push_back({
                {"INTERVAL_END", Timestamp(interval_end)},
                {"TABLE_NAME", String(table)},
                {"READ_QUERY_COUNT", Int64(table_counts.read_query_count)},
                {"WRITE_COUNT", Int64(table_counts.write_count)},
                {"DELETE_COUNT", Int64(table_counts.delete_count)},
            });
          }
          continue;
        }
        for (const auto& [table_and_column, column_counts] : counts.columns) {
          rows.push_back({
              {"INTERVAL_END", Timestamp(interval_end)},
              {"TABLE_NAME", String(table_and_column.first)},
              {"COLUMN_NAME", String(table_and_column.second)},
              {"QUERY_COUNT", Int64(column_counts.query_count)},
              {"READ_COUNT", Int64(column_counts.read_count)},
              {"WRITE_COUNT", Int64(column_counts.write_count)},
              {"IS_QUERY_CACHE_MEMORY_CAPPED", Bool(false)},
          });
        }
      }
      break;
    case StatsTable::kOldestActiveQueries: {
      std::vector<std::pair<int64_t, const ActiveQuery*>> queries;
      for (const auto& [query_id, query] : state.active_queries) {
        queries.emplace_back(query_id, &query);
      }
      std::stable_sort(queries.begin(), queries.end(),
                       [](const auto& a, const auto& b) {
                         return a.second->start_time < b.second->start_time;
                       });
      for (const auto& [query_id, query] : queries) {
        const std::string request_tag = NormalizeTag(query->request_tag);
        bool text_truncated = false;
        const absl::string_view text =
            TruncateText(query->text, &text_truncated);
        rows.push_back({
            {"START_TIME", Timestamp(query->start_time)},
            {"TEXT_FINGERPRINT",
             Int64(Fingerprint(request_tag.empty() ? query->text
                                                   : request_tag))},
            {"TEXT", String(text)},
            {"TEXT_TRUNCATED", Bool(text_truncated)},
            {"SESSION_ID", String(query->session_id)},
            {"QUERY_ID", String(absl::StrCat(query_id))},
            {"CLIENT_IP_ADDRESS", String(query->client_ip_address)},
            {"API_CLIENT_HEADER", String(query->api_client_header)},
            {"USER_AGENT_HEADER", String(query->user_agent_header)},
            {"PRIORITY", String(query->priority)},
            {"TRANSACTION_TYPE", String(query->transaction_type)},
        });
      }
      break;
    }
    case StatsTable::kActiveQueriesSummary: {
      std::optional<absl::Time> oldest_start_time;
      int64_t older_than_1s = 0;
      int64_t older_than_10s = 0;
      int64_t older_than_100s = 0;
      for (const auto& [query_id, query] : state.active_queries) {
        if (!oldest_start_time.has_value() ||
            query.start_time < *oldest_start_time) {
          oldest_start_time = query.start_time;
        }
        const absl::Duration age = now - query.start_time;
        older_than_1s += age > absl::Seconds(1);
        older_than_10s += age > absl::Seconds(10);
        older_than_100s += age > absl::Seconds(100);
      }
      rows.push_back({
          {"ACTIVE_COUNT", Int64(state.active_queries.size())},
          {"OLDEST_START_TIME", oldest_start_time.has_value()
                                    ? Timestamp(*oldest_start_time)
                                    : NullTimestamp()},
          {"COUNT_OLDER_THAN_1S", Int64(older_than_1s)},
          {"COUNT_OLDER_THAN_10S", Int64(older_than_10s)},
          {"COUNT_OLDER_THAN_100S", Int64(older_than_100s)},
      });
      break;
    }
    case StatsTable::kActivePartitionedDmls: {
      std::vector<const ActivePartitionedDml*> dmls;
      for (const auto& [id, dml] : state.active_partitioned_dmls) {
        dmls.push_back(&dml);
      }
      std::stable_sort(dmls.begin(), dmls.end(),
                       [](const ActivePartitionedDml* a,
                          const ActivePartitionedDml* b) {
                         return a->start_time < b->start_time;
                       });
      // The single partition completes when the statement does.
      for (const ActivePartitionedDml* dml : dmls) {
        rows.push_back({
            {"TEXT", String(dml->text)},
            {"TEXT_FINGERPRINT", Int64(Fingerprint(dml->text))},
            {"SESSION_ID", String(dml->session_id)},
            {"NUM_PARTITIONS_TOTAL", Int64(1)},
            {"NUM_PARTITIONS_COMPLETE", Int64(0)},
            {"NUM_TRIVIAL_PARTITIONS_COMPLETE", Int64(0)},
            {"PROGRESS", Double(0)},
            {"ROWS_PROCESSED", Int64(0)},
            {"START_TIMESTAMP", Timestamp(dml->start_time)},
            {"LAST_UPDATE_TIMESTAMP", Timestamp(dml->start_time)},
        });
      }
      break;
    }
    case StatsTable::kTableSizes:
      for (const auto& [interval_end, sizes] : state.table_sizes) {
        if (!IsVisible(interval_end, interval, now, include_open_intervals)) {
          continue;
        }
        for (const auto& [name, total_and_samples] : sizes) {
          // The emulator keeps all data on SSD.
          const double used_bytes =
              total_and_samples.first / total_and_samples.second;
          rows.push_back({
              {"INTERVAL_END", Timestamp(interval_end)},
              {"TABLE_NAME", String(name)},
              {"USED_BYTES", Double(used_bytes)},
              {"USED_SSD_BYTES", Double(used_bytes)},
              {"USED_HDD_BYTES", Double(0)},
          });
        }
      }
      break;
    case StatsTable::kRowDeletionPolicies:
      if (schema == nullptr) {
        break;
      }
      for (const Table* table : schema->tables()) {
        if (!table->row_deletion_policy().has_value()) {
          continue;
        }
        SpannerSysRow& row = rows.emplace_back();
        row["TABLE_NAME"] = String(table->Name());
        row["UNDELETABLE_ROWS"] = Int64(0);
        auto it = state.row_deletion_policy_sweeps.find(table->Name());
        if (it == state.row_deletion_policy_sweeps.end()) {
          continue;
        }
        const RowDeletionPolicySweep& sweep = it->second;
        row["PROCESSED_WATERMARK"] = Timestamp(sweep.processed_watermark);
        row["UNDELETABLE_ROWS"] = Int64(sweep.undeletable_rows);
        if (sweep.min_undeletable_timestamp.has_value()) {
          row["MIN_UNDELETABLE_TIMESTAMP"] =
              Timestamp(*sweep.min_undeletable_timestamp);
        }
      }
      break;
    case StatsTable::kUserSplitPoints:
      for (const auto& [key, split_point] : state.user_split_points) {
        if (split_point.expire_time <= now) {
          continue;
        }
        rows.push_back({
            {"TABLE_NAME", String(split_point.table_name)},
            {"INDEX_NAME", String(split_point.index_name)},
            {"INITIATOR", String(split_point.initiator)},
            {"SPLIT_KEY", String(split_point.split_key)},
            {"EXPIRE_TIME", Timestamp(split_point.expire_time)},
        });
      }
      break;
  }
  return rows;
}

}  // namespace backend
}  // namespace emulator
}  // namespace spanner
}  // namespace google

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

#include "common/config.h"

#include <string>

#include "absl/flags/flag.h"
#include "absl/time/time.h"

ABSL_FLAG(std::string, host_port, "localhost:10007",
          "Emulator host IP and port that serves Cloud Spanner gRPC requests.");

ABSL_FLAG(bool, log_requests, false,
          "If true, gRPC request and response messages are streamed to the "
          "INFO log. This switch is intended for emulator debugging.");

ABSL_FLAG(
    bool, enable_fault_injection, false,
    "If true, the emulator will inject faults to allow testing application "
    "error handling behavior. For instance, transaction Commits may be aborted "
    "to facilitate application abort-retry testing.");

ABSL_FLAG(bool, disable_query_null_filtered_index_check, false,
          "If true, then queries that use NULL_FILTERED indexes will be "
          "answered. Please test all queries using null filtered indexes "
          "against production Cloud Spanner before disabling this check."
          "\n"
          "Please consider using the query hint "
          "`@{spanner_emulator.disable_query_null_filtered_index_check=true}` "
          "to disable this check per query, instead of disabling this check "
          "for all the queries at once.");

ABSL_FLAG(std::string, data_dir, "",
          "Directory path for persistent data storage. When empty (default), "
          "the emulator uses in-memory storage. When set, the emulator uses "
          "LevelDB-backed persistent storage at the specified directory.");

ABSL_FLAG(bool, repair_corrupted_databases, false,
          "If true, a database that fails to restore from --data_dir at "
          "startup is quarantined: its on-disk storage directory is moved "
          "aside under --data_dir/.quarantine and its metadata.json entry is "
          "removed, so it no longer blocks subsequent startups. A database "
          "that fails to restore never blocks the emulator from starting or "
          "affects other databases regardless of this flag; it only controls "
          "whether the corrupted database is left in place for inspection "
          "(default) or cleaned up automatically.");

ABSL_FLAG(bool, spanner_sys_expose_open_interval, false,
          "If true, the SPANNER_SYS statistics tables also show the interval "
          "that is still in progress, so that statistics are visible right "
          "after an operation completes. Production Cloud Spanner shows only "
          "intervals that have ended.");

ABSL_FLAG(bool, enforce_placement_dml_restrictions, true,
          "If true, read-write transactions enforce the geo-partitioning "
          "(placement) DML limits of production Cloud Spanner: an INSERT or "
          "DELETE on a placement table must be the only statement in its "
          "transaction, and WHERE clauses may reference only the primary key "
          "columns of placement tables. Partitioned DML and read-only "
          "transactions are not affected.");

ABSL_FLAG(
    int, abort_current_transaction_probability, 20,
    "The probability, in percent, that a transaction that requests a lock "
    "held by an older transaction tries to abort that transaction instead of "
    "waiting for it (see --lock_wait_timeout_ms). The attempt succeeds only if "
    "the older transaction is not executing a request. A higher value gives "
    "higher priority to new transactions. A value of zero means that locks "
    "follow Cloud Spanner's wound-wait scheme: a younger transaction always "
    "waits for an older one. Requests that cannot wait, such as schema "
    "changes, abort the holder with this probability or abort themselves.");
ABSL_FLAG(int, lock_wait_timeout_ms, 10000,
          "How long, in milliseconds, a read-write transaction waits for a "
          "lock that an older transaction holds before it aborts. Following "
          "Cloud Spanner's wound-wait locking, an older transaction never "
          "waits for a younger one: it aborts (wounds) the younger holder "
          "instead. Zero means that a transaction that requests a conflicting "
          "lock aborts at once instead of waiting.");

ABSL_FLAG(int, row_deletion_policy_sweep_interval_seconds, 60,
          "How often, in seconds, each database deletes the rows that its row "
          "deletion policies (TTL) have expired. Production Cloud Spanner "
          "deletes expired rows within about 72 hours; the emulator deletes "
          "them within this interval. Zero or a negative value disables "
          "background row deletion.");

namespace google {
namespace spanner {
namespace emulator {
namespace config {

std::string grpc_host_port() { return absl::GetFlag(FLAGS_host_port); }

bool should_log_requests() { return absl::GetFlag(FLAGS_log_requests); }

bool fault_injection_enabled() {
  return absl::GetFlag(FLAGS_enable_fault_injection);
}

bool disable_query_null_filtered_index_check() {
  return absl::GetFlag(FLAGS_disable_query_null_filtered_index_check);
}

bool spanner_sys_expose_open_interval() {
  return absl::GetFlag(FLAGS_spanner_sys_expose_open_interval);
}

void set_spanner_sys_expose_open_interval(bool expose) {
  absl::SetFlag(&FLAGS_spanner_sys_expose_open_interval, expose);
}

bool enforce_placement_dml_restrictions() {
  return absl::GetFlag(FLAGS_enforce_placement_dml_restrictions);
}

void set_enforce_placement_dml_restrictions(bool enforce) {
  absl::SetFlag(&FLAGS_enforce_placement_dml_restrictions, enforce);
}

int abort_current_transaction_probability() {
  return absl::GetFlag(FLAGS_abort_current_transaction_probability);
}

void set_abort_current_transaction_probability(int probability) {
  absl::SetFlag(&FLAGS_abort_current_transaction_probability, probability);
}

absl::Duration lock_wait_timeout() {
  return absl::Milliseconds(absl::GetFlag(FLAGS_lock_wait_timeout_ms));
}

void set_lock_wait_timeout_ms(int milliseconds) {
  absl::SetFlag(&FLAGS_lock_wait_timeout_ms, milliseconds);
}

std::string data_dir() { return absl::GetFlag(FLAGS_data_dir); }

bool repair_corrupted_databases() {
  return absl::GetFlag(FLAGS_repair_corrupted_databases);
}

absl::Duration row_deletion_policy_sweep_interval() {
  return absl::Seconds(
      absl::GetFlag(FLAGS_row_deletion_policy_sweep_interval_seconds));
}

void set_row_deletion_policy_sweep_interval_seconds(int seconds) {
  absl::SetFlag(&FLAGS_row_deletion_policy_sweep_interval_seconds, seconds);
}

}  // namespace config
}  // namespace emulator
}  // namespace spanner
}  // namespace google

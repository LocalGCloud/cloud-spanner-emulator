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

#include "backend/database/table_size_sampler.h"

#include <memory>
#include <string>
#include <thread>  // NOLINT
#include <utility>
#include <vector>

#include "absl/container/flat_hash_map.h"
#include "absl/log/log.h"
#include "absl/status/status.h"
#include "absl/status/statusor.h"
#include "absl/synchronization/mutex.h"
#include "absl/time/time.h"
#include "backend/access/read.h"
#include "backend/datamodel/key_set.h"
#include "backend/schema/catalog/column.h"
#include "backend/schema/catalog/index.h"
#include "backend/schema/catalog/table.h"
#include "backend/stats/operation_stats.h"
#include "googlesql/base/status_macros.h"

namespace google {
namespace spanner {
namespace emulator {
namespace backend {

namespace {

// Returns the logical size of the rows that `read_arg` reads.
absl::StatusOr<double> ReadSize(ReadOnlyTransaction* txn,
                                const ReadArg& read_arg) {
  std::unique_ptr<RowCursor> cursor;
  GOOGLESQL_RETURN_IF_ERROR(txn->Read(read_arg, &cursor));
  double size = 0;
  while (cursor->Next()) {
    for (int i = 0; i < cursor->NumColumns(); ++i) {
      size += LogicalByteSize(cursor->ColumnValue(i));
    }
  }
  GOOGLESQL_RETURN_IF_ERROR(cursor->Status());
  return size;
}

// Returns a read of every column of every row of `table`, or of `index` of
// `table` if it is set.
ReadArg ReadAll(const Table* table, const Index* index) {
  ReadArg read_arg;
  read_arg.table = table->Name();
  read_arg.key_set = KeySet::All();
  const Table* read_table = table;
  if (index != nullptr) {
    read_arg.index = index->Name();
    read_table = index->index_data_table();
  }
  for (const Column* column : read_table->columns()) {
    read_arg.columns.push_back(column->Name());
  }
  return read_arg;
}

}  // namespace

TableSizeSampler::TableSizeSampler(
    CreateReadOnlyTransactionFn create_read_only_transaction_fn,
    SystemStatsCollector* stats_collector, absl::Duration sample_interval)
    : create_read_only_transaction_fn_(
          std::move(create_read_only_transaction_fn)),
      stats_collector_(stats_collector),
      sample_interval_(sample_interval) {
  if (sample_interval_ > absl::ZeroDuration()) {
    thread_ = std::thread(&TableSizeSampler::Run, this);
  }
}

TableSizeSampler::~TableSizeSampler() {
  {
    absl::MutexLock lock(mu_);
    stop_ = true;
  }
  if (thread_.joinable()) {
    thread_.join();
  }
}

absl::Status TableSizeSampler::SampleOnce() {
  GOOGLESQL_ASSIGN_OR_RETURN(std::unique_ptr<ReadOnlyTransaction> txn,
                             create_read_only_transaction_fn_(ReadOnlyOptions()));
  absl::flat_hash_map<std::string, double> sizes;
  for (const Table* table : txn->schema()->tables()) {
    GOOGLESQL_ASSIGN_OR_RETURN(sizes[table->Name()],
                               ReadSize(txn.get(), ReadAll(table, nullptr)));
    for (const Index* index : table->indexes()) {
      GOOGLESQL_ASSIGN_OR_RETURN(sizes[index->Name()],
                                 ReadSize(txn.get(), ReadAll(table, index)));
    }
  }
  stats_collector_->RecordTableSizes(txn->read_timestamp(), sizes);
  return absl::OkStatus();
}

void TableSizeSampler::Run() {
  while (true) {
    absl::Status status = SampleOnce();
    if (!status.ok()) {
      ABSL_LOG(WARNING) << "Table size sample failed: " << status;
    }
    absl::MutexLock lock(mu_);
    mu_.AwaitWithTimeout(absl::Condition(&stop_), sample_interval_);
    if (stop_) {
      return;
    }
  }
}

}  // namespace backend
}  // namespace emulator
}  // namespace spanner
}  // namespace google

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

#ifndef THIRD_PARTY_CLOUD_SPANNER_EMULATOR_BACKEND_DATABASE_TABLE_SIZE_SAMPLER_H_
#define THIRD_PARTY_CLOUD_SPANNER_EMULATOR_BACKEND_DATABASE_TABLE_SIZE_SAMPLER_H_

#include <functional>
#include <memory>
#include <thread>  // NOLINT

#include "absl/base/thread_annotations.h"
#include "absl/status/status.h"
#include "absl/status/statusor.h"
#include "absl/synchronization/mutex.h"
#include "absl/time/time.h"
#include "backend/stats/system_stats_collector.h"
#include "backend/transaction/options.h"
#include "backend/transaction/read_only_transaction.h"

namespace google {
namespace spanner {
namespace emulator {
namespace backend {

// Samples the size of each table and index of a database for
// SPANNER_SYS.TABLE_SIZES_STATS_1HOUR, which averages the samples of each
// clock hour.
//
// Like Cloud Spanner, a background thread takes a sample every
// `sample_interval`, starting when the sampler is created. A sample reads
// every row at a strong timestamp and sums the logical size of its key and
// column values (see LogicalByteSize), which estimates the size of the data
// without storage overhead or compression.
class TableSizeSampler {
 public:
  using CreateReadOnlyTransactionFn =
      std::function<absl::StatusOr<std::unique_ptr<ReadOnlyTransaction>>(
          const ReadOnlyOptions& options)>;

  // How often the database creates its sampler samples.
  static constexpr absl::Duration kDefaultSampleInterval = absl::Minutes(5);

  // A `sample_interval` that is not positive disables the background thread;
  // SampleOnce() still works.
  TableSizeSampler(CreateReadOnlyTransactionFn create_read_only_transaction_fn,
                   SystemStatsCollector* stats_collector,
                   absl::Duration sample_interval);

  // Stops the background thread after the sample in progress, if any.
  ~TableSizeSampler();

  TableSizeSampler(const TableSizeSampler&) = delete;
  TableSizeSampler& operator=(const TableSizeSampler&) = delete;

  // Samples the sizes of the tables and indexes now.
  absl::Status SampleOnce();

 private:
  void Run();

  const CreateReadOnlyTransactionFn create_read_only_transaction_fn_;
  SystemStatsCollector* const stats_collector_;
  const absl::Duration sample_interval_;

  absl::Mutex mu_;
  bool stop_ ABSL_GUARDED_BY(mu_) = false;
  std::thread thread_;
};

}  // namespace backend
}  // namespace emulator
}  // namespace spanner
}  // namespace google

#endif  // THIRD_PARTY_CLOUD_SPANNER_EMULATOR_BACKEND_DATABASE_TABLE_SIZE_SAMPLER_H_

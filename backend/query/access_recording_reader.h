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

#ifndef THIRD_PARTY_CLOUD_SPANNER_EMULATOR_BACKEND_QUERY_ACCESS_RECORDING_READER_H_
#define THIRD_PARTY_CLOUD_SPANNER_EMULATOR_BACKEND_QUERY_ACCESS_RECORDING_READER_H_

#include <cstdint>
#include <memory>

#include "absl/status/status.h"
#include "backend/access/read.h"
#include "backend/stats/operation_stats.h"

namespace google {
namespace spanner {
namespace emulator {
namespace backend {

// A RowReader that forwards reads to another reader while recording the
// tables and columns that a statement reads and counting the rows it scans.
// Rows are counted as the statement consumes them.
class AccessRecordingReader : public RowReader {
 public:
  explicit AccessRecordingReader(RowReader* reader) : reader_(reader) {}

  absl::Status Read(const ReadArg& read_arg,
                    std::unique_ptr<RowCursor>* cursor) override;

  int64_t rows_scanned() const { return rows_scanned_; }

  const AccessFootprint& footprint() const { return footprint_; }

 private:
  RowReader* reader_;
  int64_t rows_scanned_ = 0;
  AccessFootprint footprint_;
};

}  // namespace backend
}  // namespace emulator
}  // namespace spanner
}  // namespace google

#endif  // THIRD_PARTY_CLOUD_SPANNER_EMULATOR_BACKEND_QUERY_ACCESS_RECORDING_READER_H_

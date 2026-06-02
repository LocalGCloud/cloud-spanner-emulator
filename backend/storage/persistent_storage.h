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

#ifndef THIRD_PARTY_CLOUD_SPANNER_EMULATOR_BACKEND_STORAGE_PERSISTENT_STORAGE_H_
#define THIRD_PARTY_CLOUD_SPANNER_EMULATOR_BACKEND_STORAGE_PERSISTENT_STORAGE_H_

#include <condition_variable>
#include <memory>
#include <mutex>
#include <queue>
#include <string>
#include <thread>

#include "zetasql/public/value.h"
#include "absl/status/status.h"
#include "absl/status/statusor.h"
#include "absl/synchronization/mutex.h"
#include "absl/time/time.h"
#include "backend/common/ids.h"
#include "backend/datamodel/key.h"
#include "backend/datamodel/key_range.h"
#include "backend/storage/iterator.h"
#include "backend/storage/storage.h"
#include "leveldb/db.h"
#include "leveldb/write_batch.h"

namespace google {
namespace spanner {
namespace emulator {
namespace backend {

// PersistentStorage implements a LevelDB-backed multi-version data store.
//
// LevelDB key format (length-prefixed, no separators):
//   {table_id_len:4BE}{table_id}{encoded_key_len:4BE}{encoded_key}
//   {column_id_len:4BE}{column_id}{timestamp:8}
//
// Each component except the fixed-size timestamp is preceded by a 4-byte
// big-endian length prefix, eliminating ambiguity from embedded \x00 bytes.
//
// The encoded_key is produced by key_codec.h and preserves the sort order
// defined by Key::Compare().
//
// Values are serialized using value_codec.h.
//
// NOTE: Timestamps are encoded at microsecond precision (via
// absl::ToUnixMicros). This matches Cloud Spanner's commit timestamp
// resolution but differs from InMemoryStorage which preserves full
// nanosecond precision. Two writes within the same microsecond will
// overwrite each other.
//
// This class is thread-safe.
class PersistentStorage : public Storage {
 public:
  // Creates a new PersistentStorage backed by a LevelDB database at the
  // given directory path.
  static absl::StatusOr<std::unique_ptr<PersistentStorage>> Create(
      const std::string& data_dir);

  ~PersistentStorage() override;

  absl::Status Lookup(absl::Time timestamp, const TableID& table_id,
                      const Key& key, const std::vector<ColumnID>& column_ids,
                      std::vector<zetasql::Value>* values) const override;

  absl::Status Read(absl::Time timestamp, const TableID& table_id,
                    const KeyRange& key_range,
                    const std::vector<ColumnID>& column_ids,
                    std::unique_ptr<StorageIterator>* itr) const override;

  absl::Status Write(absl::Time timestamp, const TableID& table_id,
                     const Key& key, const std::vector<ColumnID>& column_ids,
                     const std::vector<zetasql::Value>& values) override;

  absl::Status Delete(absl::Time timestamp, const TableID& table_id,
                      const KeyRange& key_range) override;

  void SetVersionRetentionPeriod(
      absl::Duration version_retention_period) override;

  void CleanUpDeletedTables(absl::Time timestamp) override
      ABSL_LOCKS_EXCLUDED(mu_);

  void CleanUpDeletedColumns(absl::Time timestamp) override
      ABSL_LOCKS_EXCLUDED(mu_);

  void MarkDroppedTable(absl::Time timestamp, TableID dropped_table_id) override
      ABSL_LOCKS_EXCLUDED(mu_);

  void MarkDroppedColumn(absl::Time timestamp, TableID dropped_table_id,
                         ColumnID dropped_column_id) override
      ABSL_LOCKS_EXCLUDED(mu_);

 private:
  // WriteQueue serializes all LevelDB writes through a single worker thread.
  // This eliminates interleaving of concurrent WriteBatch submissions.
  class WriteQueue {
   public:
    explicit WriteQueue(leveldb::DB* db);
    ~WriteQueue();

    // Submits a batch to the worker thread. Blocks until committed.
    // Returns the LevelDB Status from db_->Write().
    leveldb::Status Submit(leveldb::WriteBatch batch);

    // Stops accepting submissions, processes remaining batches, joins worker.
    void Shutdown();

   private:
    void WorkerLoop();

    std::thread worker_;
    std::mutex mu_;
    std::condition_variable cv_;
    std::queue<leveldb::WriteBatch> queue_;
    std::queue<leveldb::Status> results_;
    leveldb::DB* db_;
    bool shutdown_ = false;
  };

  explicit PersistentStorage(std::unique_ptr<leveldb::DB> db);

  // Builds a LevelDB key from the components.
  static std::string MakeLevelDBKey(const TableID& table_id,
                                    const std::string& encoded_key,
                                    const ColumnID& column_id,
                                    absl::Time timestamp);

  // Builds a LevelDB key prefix for scanning all columns of a row.
  static std::string MakeRowPrefix(const TableID& table_id,
                                   const std::string& encoded_key);

  // Builds a LevelDB key prefix for scanning a table.
  static std::string MakeTablePrefix(const TableID& table_id);

  // Encodes a timestamp as 8 big-endian bytes.
  static std::string EncodeTimestamp(absl::Time timestamp);

  // Returns true if the row identified by the given encoded key exists at the
  // specified timestamp by checking the _exists column.
  bool Exists(const TableID& table_id, const std::string& encoded_key,
              absl::Time timestamp) const;

  // Finds the value for a specific cell at or before the given timestamp.
  // Uses LevelDB iterator seek to find the most recent version.
  zetasql::Value GetCellValueAtTimestamp(const TableID& table_id,
                                           const std::string& encoded_key,
                                           const ColumnID& column_id,
                                           absl::Time timestamp) const;

  // Collects all distinct encoded keys in a table within a key range.
  std::vector<std::string> CollectKeysInRange(
      const TableID& table_id, const std::string& start_encoded,
      const std::string& limit_encoded) const;

  // Removes old versions of a cell that are past the retention period.
  // Mirrors InMemoryStorage::RemoveExpiredVersions behavior: keeps the most
  // recent version within the retention window, deletes everything older.
  void RemoveExpiredVersions(const std::string& cell_prefix,
                             absl::Time timestamp,
                             leveldb::WriteBatch* batch);

  mutable absl::Mutex mu_;
  std::unique_ptr<leveldb::DB> db_;
  WriteQueue write_queue_;

  // Tracks when tables were dropped.
  std::map<absl::Time, TableID> dropped_tables_ ABSL_GUARDED_BY(mu_);

  // Tracks when columns were dropped.
  std::map<absl::Time, std::pair<TableID, ColumnID>> dropped_columns_
      ABSL_GUARDED_BY(mu_);

  mutable absl::Mutex version_retention_period_mu_ ABSL_ACQUIRED_AFTER(mu_);
  absl::Duration version_retention_period_
      ABSL_GUARDED_BY(version_retention_period_mu_) = absl::Hours(1);
};

}  // namespace backend
}  // namespace emulator
}  // namespace spanner
}  // namespace google

#endif  // THIRD_PARTY_CLOUD_SPANNER_EMULATOR_BACKEND_STORAGE_PERSISTENT_STORAGE_H_

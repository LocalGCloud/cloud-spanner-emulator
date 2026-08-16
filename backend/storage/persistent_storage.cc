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

#include "backend/storage/persistent_storage.h"

#include <atomic>
#include <chrono>
#include <cstdint>
#include <cstring>
#include <filesystem>  // C++17
#include <map>
#include <memory>
#include <set>
#include <string>
#include <utility>
#include <vector>

#include "absl/status/status.h"
#include "absl/status/statusor.h"
#include "absl/strings/str_cat.h"
#include "absl/time/time.h"
#include "backend/storage/in_memory_iterator.h"
#include "backend/storage/key_codec.h"
#include "backend/storage/value_codec.h"
#include "common/errors.h"
#include "googlesql/base/status_macros.h"
#include "googlesql/public/value.h"
#include "leveldb/db.h"
#include "leveldb/iterator.h"
#include "leveldb/options.h"
#include "leveldb/status.h"
#include "leveldb/write_batch.h"

namespace google {
namespace spanner {
namespace emulator {
namespace backend {

namespace {

static constexpr char kExistsColumn[] = "_exists";

absl::Status LevelDBStatusToAbsl(const leveldb::Status& status) {
  if (status.ok()) return absl::OkStatus();
  if (status.IsNotFound()) return absl::NotFoundError(status.ToString());
  if (status.IsCorruption()) return absl::DataLossError(status.ToString());
  if (status.IsIOError()) return absl::UnavailableError(status.ToString());
  if (status.IsNotSupportedError())
    return absl::UnimplementedError(status.ToString());
  if (status.IsInvalidArgument())
    return absl::InvalidArgumentError(status.ToString());
  return absl::InternalError(status.ToString());
}

// Appends a 4-byte big-endian length prefix followed by the data.
void AppendLengthPrefixed(std::string* out, const std::string& data) {
  uint32_t len = static_cast<uint32_t>(data.size());
  char buf[4];
  buf[0] = static_cast<char>((len >> 24) & 0xFF);
  buf[1] = static_cast<char>((len >> 16) & 0xFF);
  buf[2] = static_cast<char>((len >> 8) & 0xFF);
  buf[3] = static_cast<char>(len & 0xFF);
  out->append(buf, 4);
  out->append(data);
}

// Reads a 4-byte big-endian length at the given offset and returns the
// length. Returns false if there isn't enough data.
bool ReadLengthPrefix(const char* data, size_t data_size, size_t offset,
                      uint32_t* len) {
  if (offset + 4 > data_size) return false;
  const auto* p = reinterpret_cast<const unsigned char*>(data + offset);
  *len = (static_cast<uint32_t>(p[0]) << 24) |
         (static_cast<uint32_t>(p[1]) << 16) |
         (static_cast<uint32_t>(p[2]) << 8) | static_cast<uint32_t>(p[3]);
  return true;
}

// Extracts the encoded key portion from a length-prefixed LevelDB key.
// Format: {table_id_len:4BE}{table_id}{encoded_key_len:4BE}{encoded_key}
//         {column_id_len:4BE}{column_id}{timestamp:8}
std::string ExtractEncodedKeyFromLevelDBKey(const leveldb::Slice& ldb_key) {
  const char* data = ldb_key.data();
  size_t size = ldb_key.size();
  size_t offset = 0;

  // Skip table_id.
  uint32_t table_id_len;
  if (!ReadLengthPrefix(data, size, offset, &table_id_len)) return "";
  offset += 4 + table_id_len;

  // Read encoded_key.
  uint32_t encoded_key_len;
  if (!ReadLengthPrefix(data, size, offset, &encoded_key_len)) return "";
  offset += 4;
  if (offset + encoded_key_len > size) return "";
  std::string result(data + offset, encoded_key_len);
  return result;
}

// Extracts the column_id portion from a length-prefixed LevelDB key.
std::string ExtractColumnIdFromLevelDBKey(const leveldb::Slice& ldb_key) {
  const char* data = ldb_key.data();
  size_t size = ldb_key.size();
  size_t offset = 0;

  // Skip table_id.
  uint32_t table_id_len;
  if (!ReadLengthPrefix(data, size, offset, &table_id_len)) return "";
  offset += 4 + table_id_len;

  // Skip encoded_key.
  uint32_t encoded_key_len;
  if (!ReadLengthPrefix(data, size, offset, &encoded_key_len)) return "";
  offset += 4 + encoded_key_len;

  // Read column_id.
  uint32_t column_id_len;
  if (!ReadLengthPrefix(data, size, offset, &column_id_len)) return "";
  offset += 4;
  if (offset + column_id_len > size) return "";
  return std::string(data + offset, column_id_len);
}

// Checks the iterator status after a scan loop. Returns an error if the
// iterator encountered an I/O or corruption error during iteration.
absl::Status CheckIteratorStatus(const leveldb::Iterator& it) {
  if (!it.status().ok()) {
    return LevelDBStatusToAbsl(it.status());
  }
  return absl::OkStatus();
}

class SnapshotGuard {
 public:
  explicit SnapshotGuard(leveldb::DB* db)
      : db_(db), snapshot_(db == nullptr ? nullptr : db->GetSnapshot()) {}

  SnapshotGuard(const SnapshotGuard&) = delete;
  SnapshotGuard& operator=(const SnapshotGuard&) = delete;

  ~SnapshotGuard() {
    if (db_ != nullptr && snapshot_ != nullptr) {
      db_->ReleaseSnapshot(snapshot_);
    }
  }

  const leveldb::Snapshot* get() const { return snapshot_; }

  const leveldb::Snapshot* Release() {
    return std::exchange(snapshot_, nullptr);
  }

 private:
  leveldb::DB* db_;
  const leveldb::Snapshot* snapshot_;
};

class DirectoryCleanupGuard {
 public:
  explicit DirectoryCleanupGuard(std::filesystem::path path)
      : path_(std::move(path)) {}

  DirectoryCleanupGuard(const DirectoryCleanupGuard&) = delete;
  DirectoryCleanupGuard& operator=(const DirectoryCleanupGuard&) = delete;

  ~DirectoryCleanupGuard() {
    if (armed_) {
      std::error_code ignored;
      std::filesystem::remove_all(path_, ignored);
    }
  }

  void Disarm() { armed_ = false; }

 private:
  std::filesystem::path path_;
  bool armed_ = true;
};

}  // namespace

// A StorageIterator wrapper that releases a LevelDB snapshot on destruction.
// Used by PersistentStorage::Read() to ensure the snapshot lives until the
// caller is done iterating through the results.
class SnapshotOwningIterator : public StorageIterator {
 public:
  SnapshotOwningIterator(std::vector<FixedRowStorageIterator::Row> rows,
                         leveldb::DB* db, const leveldb::Snapshot* snapshot)
      : inner_(std::move(rows)), db_(db), snapshot_(snapshot) {}

  ~SnapshotOwningIterator() override {
    if (db_ && snapshot_) {
      db_->ReleaseSnapshot(snapshot_);
    }
  }

  bool Next() override { return inner_.Next(); }
  absl::Status Status() const override { return inner_.Status(); }
  const class Key& Key() const override { return inner_.Key(); }
  int NumColumns() const override { return inner_.NumColumns(); }
  const googlesql::Value& ColumnValue(int i) const override {
    return inner_.ColumnValue(i);
  }

 private:
  FixedRowStorageIterator inner_;
  leveldb::DB* db_;
  const leveldb::Snapshot* snapshot_;
};

PersistentStorage::PersistentStorage(std::unique_ptr<leveldb::DB> db)
    : db_(std::move(db)), write_queue_(db_.get()) {}

PersistentStorage::~PersistentStorage() { write_queue_.Shutdown(); }

absl::StatusOr<std::unique_ptr<PersistentStorage>> PersistentStorage::Create(
    const std::string& data_dir) {
  leveldb::Options options;
  options.create_if_missing = true;
  // Use a larger write buffer for better performance.
  options.write_buffer_size = 64 * 1024 * 1024;  // 64MB
  options.max_open_files = 1000;

  // Create all intermediate directories (mkdir -p equivalent).
  // leveldb::DB::Open() with create_if_missing only creates the leaf directory,
  // not parent directories. Without this, new databases fail with LOCK errors.
  std::error_code ec;
  std::filesystem::create_directories(data_dir, ec);
  if (ec) {
    return absl::InternalError(absl::StrCat("Failed to create directory ",
                                            data_dir, ": ", ec.message()));
  }

  leveldb::DB* raw_db = nullptr;
  leveldb::Status status = leveldb::DB::Open(options, data_dir, &raw_db);
  if (!status.ok()) {
    return absl::InternalError(absl::StrCat("Failed to open LevelDB at ",
                                            data_dir, ": ", status.ToString()));
  }

  return absl::WrapUnique(
      new PersistentStorage(std::unique_ptr<leveldb::DB>(raw_db)));
}

absl::Status PersistentStorage::CreateCheckpoint(
    const std::string& output_dir) const {
  if (output_dir.empty()) {
    return absl::InvalidArgumentError(
        "Backup checkpoint directory must not be empty");
  }
  std::error_code filesystem_error;
  const bool output_exists =
      std::filesystem::exists(output_dir, filesystem_error);
  if (filesystem_error) {
    return absl::InternalError(absl::StrCat(
        "Failed to inspect backup checkpoint destination: ",
        filesystem_error.message()));
  }
  if (output_exists) {
    return absl::AlreadyExistsError(
        absl::StrCat("Backup checkpoint already exists: ", output_dir));
  }

  const std::filesystem::path output_parent =
      std::filesystem::path(output_dir).parent_path();
  if (!output_parent.empty()) {
    std::filesystem::create_directories(output_parent, filesystem_error);
  }
  if (filesystem_error) {
    return absl::InternalError(
        absl::StrCat("Failed to create backup checkpoint parent: ",
                     filesystem_error.message()));
  }
  static std::atomic<uint64_t> temporary_sequence{0};
  const std::filesystem::path temporary_directory = absl::StrCat(
      output_dir, ".tmp-",
      std::chrono::steady_clock::now().time_since_epoch().count(), "-",
      temporary_sequence.fetch_add(1));
  DirectoryCleanupGuard cleanup(temporary_directory);

  leveldb::Options options;
  options.create_if_missing = true;
  leveldb::DB* destination_raw = nullptr;
  leveldb::Status open_status =
      leveldb::DB::Open(options, temporary_directory.string(),
                        &destination_raw);
  if (!open_status.ok()) {
    return LevelDBStatusToAbsl(open_status);
  }
  std::unique_ptr<leveldb::DB> destination(destination_raw);

  SnapshotGuard snapshot(db_.get());
  leveldb::ReadOptions read_options;
  read_options.snapshot = snapshot.get();
  std::unique_ptr<leveldb::Iterator> iterator(db_->NewIterator(read_options));
  leveldb::WriteOptions write_options;
  write_options.sync = true;
  leveldb::WriteBatch batch;
  size_t batch_bytes = 0;
  constexpr size_t kMaximumBatchBytes = 4 * 1024 * 1024;

  absl::Status result = absl::OkStatus();
  for (iterator->SeekToFirst(); iterator->Valid(); iterator->Next()) {
    batch.Put(iterator->key(), iterator->value());
    batch_bytes += iterator->key().size() + iterator->value().size();
    if (batch_bytes >= kMaximumBatchBytes) {
      leveldb::Status write_status = destination->Write(write_options, &batch);
      if (!write_status.ok()) {
        result = LevelDBStatusToAbsl(write_status);
        break;
      }
      batch.Clear();
      batch_bytes = 0;
    }
  }
  if (result.ok()) {
    result = CheckIteratorStatus(*iterator);
  }
  if (result.ok() && batch_bytes > 0) {
    result = LevelDBStatusToAbsl(destination->Write(write_options, &batch));
  }
  destination.reset();
  if (result.ok()) {
    std::filesystem::rename(temporary_directory, output_dir, filesystem_error);
    if (filesystem_error) {
      result = absl::InternalError(absl::StrCat(
          "Failed to publish backup checkpoint: ",
          filesystem_error.message()));
    }
  }
  if (result.ok()) {
    cleanup.Disarm();
  }
  return result;
}

// ---------------------------------------------------------------------------
// WriteQueue — serializes all LevelDB writes through a single worker thread.
// ---------------------------------------------------------------------------

PersistentStorage::WriteQueue::WriteQueue(leveldb::DB* db) : db_(db) {
  worker_ = std::thread(&WriteQueue::WorkerLoop, this);
}

PersistentStorage::WriteQueue::~WriteQueue() {
  if (!shutdown_) {
    Shutdown();
  }
}

leveldb::Status PersistentStorage::WriteQueue::Submit(
    leveldb::WriteBatch batch) {
  std::unique_lock<std::mutex> lock(mu_);
  if (shutdown_) {
    return leveldb::Status::IOError("WriteQueue is shutting down");
  }
  queue_.push(std::move(batch));
  cv_.notify_one();
  cv_.wait(lock, [this] { return !results_.empty() || shutdown_; });
  if (shutdown_ && results_.empty()) {
    return leveldb::Status::IOError("WriteQueue shut down");
  }
  leveldb::Status result = std::move(results_.front());
  results_.pop();
  return result;
}

void PersistentStorage::WriteQueue::Shutdown() {
  {
    std::unique_lock<std::mutex> lock(mu_);
    shutdown_ = true;
    cv_.notify_one();  // Wake the worker
  }
  if (worker_.joinable()) {
    worker_.join();
  }
  // Notify any remaining waiters (should be none after worker finishes).
  {
    std::unique_lock<std::mutex> lock(mu_);
    cv_.notify_all();
  }
}

void PersistentStorage::WriteQueue::WorkerLoop() {
  std::unique_lock<std::mutex> lock(mu_);
  while (true) {
    cv_.wait(lock, [this] { return !queue_.empty() || shutdown_; });
    if (shutdown_ && queue_.empty()) break;
    while (!queue_.empty()) {
      leveldb::WriteBatch batch = std::move(queue_.front());
      queue_.pop();
      lock.unlock();
      leveldb::Status s = db_->Write(leveldb::WriteOptions(), &batch);
      lock.lock();
      results_.push(std::move(s));
      cv_.notify_all();
    }
  }
}

std::string PersistentStorage::EncodeTimestamp(absl::Time timestamp) {
  // Encode as big-endian int64 microseconds for proper ordering.
  int64_t micros = absl::ToUnixMicros(timestamp);
  // Flip sign bit for unsigned comparison ordering.
  uint64_t encoded = static_cast<uint64_t>(micros) ^ (1ULL << 63);
  char buf[8];
  buf[0] = static_cast<char>((encoded >> 56) & 0xFF);
  buf[1] = static_cast<char>((encoded >> 48) & 0xFF);
  buf[2] = static_cast<char>((encoded >> 40) & 0xFF);
  buf[3] = static_cast<char>((encoded >> 32) & 0xFF);
  buf[4] = static_cast<char>((encoded >> 24) & 0xFF);
  buf[5] = static_cast<char>((encoded >> 16) & 0xFF);
  buf[6] = static_cast<char>((encoded >> 8) & 0xFF);
  buf[7] = static_cast<char>(encoded & 0xFF);
  return std::string(buf, 8);
}

std::string PersistentStorage::MakeLevelDBKey(const TableID& table_id,
                                              const std::string& encoded_key,
                                              const ColumnID& column_id,
                                              absl::Time timestamp) {
  std::string result;
  result.reserve(4 + table_id.size() + 4 + encoded_key.size() + 4 +
                 column_id.size() + 8);
  AppendLengthPrefixed(&result, table_id);
  AppendLengthPrefixed(&result, encoded_key);
  AppendLengthPrefixed(&result, column_id);
  result.append(EncodeTimestamp(timestamp));
  return result;
}

std::string PersistentStorage::MakeRowPrefix(const TableID& table_id,
                                             const std::string& encoded_key) {
  std::string result;
  result.reserve(4 + table_id.size() + 4 + encoded_key.size());
  AppendLengthPrefixed(&result, table_id);
  AppendLengthPrefixed(&result, encoded_key);
  return result;
}

std::string PersistentStorage::MakeTablePrefix(const TableID& table_id) {
  std::string result;
  result.reserve(4 + table_id.size());
  AppendLengthPrefixed(&result, table_id);
  return result;
}

googlesql::Value PersistentStorage::GetCellValueAtTimestamp(
    const TableID& table_id, const std::string& encoded_key,
    const ColumnID& column_id, absl::Time timestamp) const {
  // Build a seek key just past the target timestamp (upper bound).
  // Since timestamps are encoded in ascending order, we seek to the key
  // with the target timestamp and then look for the entry at or before it.
  std::string seek_key =
      MakeLevelDBKey(table_id, encoded_key, column_id, timestamp);

  // Build the prefix for this specific cell (table+key+column).
  std::string cell_prefix;
  AppendLengthPrefixed(&cell_prefix, table_id);
  AppendLengthPrefixed(&cell_prefix, encoded_key);
  AppendLengthPrefixed(&cell_prefix, column_id);

  std::unique_ptr<leveldb::Iterator> it(
      db_->NewIterator(leveldb::ReadOptions()));

  // Seek to the position just past the target timestamp.
  // We want the largest timestamp <= target, so we seek to target+1 and
  // go back, or seek to target and check.
  it->Seek(seek_key);
  if (!it->status().ok()) return googlesql::Value();

  // Check if we landed exactly on the target or need to go to previous.
  if (it->Valid()) {
    leveldb::Slice found_key = it->key();
    // Check if this key has the right cell prefix.
    if (found_key.starts_with(cell_prefix)) {
      // The key at this position has timestamp >= target.
      // If it's exactly the target, use it.
      if (found_key.compare(seek_key) == 0) {
        return DecodeValue(it->value().ToString());
      }
      // Otherwise the timestamp is > target, need to go to previous entry.
      it->Prev();
    } else {
      // We overshot past this cell entirely, go back.
      it->Prev();
    }
  } else {
    // Past the end of the database, go to the last entry.
    it->SeekToLast();
  }
  if (!it->status().ok()) return googlesql::Value();

  // Now check if the current position is within the right cell prefix.
  if (it->Valid()) {
    leveldb::Slice found_key = it->key();
    if (found_key.starts_with(cell_prefix)) {
      return DecodeValue(it->value().ToString());
    }
  }

  return googlesql::Value();
}

bool PersistentStorage::Exists(const TableID& table_id,
                               const std::string& encoded_key,
                               absl::Time timestamp) const {
  googlesql::Value value =
      GetCellValueAtTimestamp(table_id, encoded_key, kExistsColumn, timestamp);
  return value.is_valid() && !value.is_null() && value.bool_value();
}

std::vector<std::string> PersistentStorage::CollectKeysInRange(
    const TableID& table_id, const std::string& start_encoded,
    const std::string& limit_encoded) const {
  std::string table_prefix = MakeTablePrefix(table_id);
  std::string seek_start = MakeRowPrefix(table_id, start_encoded);

  std::set<std::string> unique_keys;
  std::unique_ptr<leveldb::Iterator> it(
      db_->NewIterator(leveldb::ReadOptions()));

  for (it->Seek(seek_start); it->Valid(); it->Next()) {
    leveldb::Slice ldb_key = it->key();

    // Stop if we've passed the table prefix.
    if (!ldb_key.starts_with(table_prefix)) break;

    // Extract the encoded key from the LevelDB key.
    std::string encoded_key = ExtractEncodedKeyFromLevelDBKey(ldb_key);
    if (encoded_key.empty()) continue;

    // Compare the extracted encoded key directly against limit_encoded.
    // We must NOT compare the full LevelDB key against
    // MakeRowPrefix(table_id, limit_encoded) because EncodeKeyForPrefixLimit
    // appends 0xFF bytes, changing the byte length. The 4-byte big-endian
    // length prefix in MakeRowPrefix would then differ (e.g. 0x09 vs 0x11),
    // causing ALL subsequent rows to sort before the limit — effectively
    // scanning the entire table instead of just the target range.
    if (!limit_encoded.empty() && encoded_key >= limit_encoded) break;

    unique_keys.insert(encoded_key);
  }
  // On I/O error, return whatever we collected so far (best effort).
  // Callers that need strict correctness check via Exists()/Lookup().

  return std::vector<std::string>(unique_keys.begin(), unique_keys.end());
}

absl::Status PersistentStorage::Lookup(
    absl::Time timestamp, const TableID& table_id, const Key& key,
    const std::vector<ColumnID>& column_ids,
    std::vector<googlesql::Value>* values) const {
  if (!column_ids.empty() && values == nullptr) {
    return error::Internal(
        "PersistentStorage::Lookup was passed a nullptr for "
        "values, but had non-empty column_ids.");
  }
  if (values != nullptr) {
    values->clear();
  }

  std::string encoded_key = EncodeKey(key);

  // Check if the row exists.
  if (!Exists(table_id, encoded_key, timestamp)) {
    return absl::Status(
        absl::StatusCode::kNotFound,
        absl::StrCat("Key: ", key.DebugString(), " not found for table: ",
                     table_id, " at timestamp: ", absl::FormatTime(timestamp)));
  }

  if (column_ids.empty()) {
    return absl::OkStatus();
  }

  for (const ColumnID& column_id : column_ids) {
    values->emplace_back(
        GetCellValueAtTimestamp(table_id, encoded_key, column_id, timestamp));
  }

  return absl::OkStatus();
}

// Decodes a Key from the serialized __key_data__ bytes.
static Key DecodeKeyData(const googlesql::Value& key_data) {
  Key reconstructed_key;
  if (!key_data.is_valid() || key_data.is_null()) return reconstructed_key;

  std::string data = key_data.bytes_value();
  const char* ptr = data.data();
  const char* end = ptr + data.size();
  if (end - ptr < 4) return reconstructed_key;

  const auto* up = reinterpret_cast<const unsigned char*>(ptr);
  int32_t num_cols =
      static_cast<int32_t>(up[0]) | (static_cast<int32_t>(up[1]) << 8) |
      (static_cast<int32_t>(up[2]) << 16) | (static_cast<int32_t>(up[3]) << 24);
  ptr += 4;
  for (int i = 0; i < num_cols && ptr < end; ++i) {
    if (end - ptr < 2) break;  // Need at least 2 bytes for desc + nulls_last.
    bool desc = (*ptr++ != 0);
    bool nulls_last = (*ptr++ != 0);
    if (end - ptr < 4) break;
    const auto* vp = reinterpret_cast<const unsigned char*>(ptr);
    int32_t val_len = static_cast<int32_t>(vp[0]) |
                      (static_cast<int32_t>(vp[1]) << 8) |
                      (static_cast<int32_t>(vp[2]) << 16) |
                      (static_cast<int32_t>(vp[3]) << 24);
    ptr += 4;
    if (val_len < 0 || end - ptr < val_len) break;
    std::string encoded_val(ptr, val_len);
    ptr += val_len;
    googlesql::Value col_val = DecodeValue(encoded_val);
    reconstructed_key.AddColumn(col_val, desc, nulls_last);
  }
  return reconstructed_key;
}

absl::Status PersistentStorage::Read(
    absl::Time timestamp, const TableID& table_id, const KeyRange& key_range,
    const std::vector<ColumnID>& column_ids,
    std::unique_ptr<StorageIterator>* itr) const {
  if (!key_range.IsClosedOpen()) {
    return error::Internal(
        absl::StrCat("PersistentStorage::Read should be called "
                     "with ClosedOpen key range, found: ",
                     key_range.DebugString()));
  }

  std::vector<FixedRowStorageIterator::Row> rows;

  if (key_range.start_key() >= key_range.limit_key()) {
    *itr = std::make_unique<FixedRowStorageIterator>();
    return absl::OkStatus();
  }

  std::string start_encoded = EncodeKey(key_range.start_key());
  // Use EncodeKeyForPrefixLimit for prefix-limit keys (e.g. from
  // KeyRange::Point), otherwise use EncodeKey for regular limit keys.
  // A prefix-limit key has the same columns but compares greater --
  // detected when both encode identically but the limit compares greater.
  // When both are empty (Key::Empty/Infinity), keep limit empty to mean
  // "no upper bound" (the original convention used by CollectKeysInRange).
  std::string limit_encoded = EncodeKey(key_range.limit_key());
  if (!limit_encoded.empty() &&
      limit_encoded == EncodeKey(key_range.start_key()) &&
      key_range.start_key() < key_range.limit_key()) {
    limit_encoded = EncodeKeyForPrefixLimit(key_range.limit_key());
  }
  std::string table_prefix = MakeTablePrefix(table_id);
  std::string seek_start = MakeRowPrefix(table_id, start_encoded);
  std::string ts_encoded = EncodeTimestamp(timestamp);

  // Build the set of columns we need to collect (requested + _exists +
  // __key_data__).
  std::set<std::string> needed_columns;
  needed_columns.insert(kExistsColumn);
  needed_columns.insert("__key_data__");
  for (const auto& col : column_ids) {
    needed_columns.insert(col);
  }

  // Single-pass scan: iterate through the range and collect the best
  // (latest <= timestamp) value for each (encoded_key, column) pair.
  struct CellData {
    std::string best_value;      // encoded value from LevelDB
    std::string best_timestamp;  // 8-byte encoded timestamp of best match
  };
  // Map: encoded_key -> (column_id -> best cell data)
  std::map<std::string, std::map<std::string, CellData>> rows_data;

  // Acquire a LevelDB snapshot for point-in-time read consistency. The guard
  // transfers ownership only after the result iterator is fully constructed.
  SnapshotGuard snapshot(db_.get());
  leveldb::ReadOptions read_options;
  read_options.snapshot = snapshot.get();

  std::unique_ptr<leveldb::Iterator> it(db_->NewIterator(read_options));
  for (it->Seek(seek_start); it->Valid(); it->Next()) {
    leveldb::Slice ldb_key = it->key();
    if (!ldb_key.starts_with(table_prefix)) break;

    // Parse the LevelDB key components.
    const char* kdata = ldb_key.data();
    size_t ksize = ldb_key.size();
    size_t offset = 0;

    // Read table_id.
    uint32_t tid_len;
    if (!ReadLengthPrefix(kdata, ksize, offset, &tid_len)) continue;
    offset += 4 + tid_len;

    // Read encoded_key.
    uint32_t ekey_len;
    if (!ReadLengthPrefix(kdata, ksize, offset, &ekey_len)) continue;
    size_t ekey_start = offset + 4;
    offset += 4 + ekey_len;
    if (offset > ksize) continue;
    std::string encoded_key(kdata + ekey_start, ekey_len);

    // Compare the extracted encoded key directly against limit_encoded.
    // We must NOT compare the full LevelDB key against
    // MakeRowPrefix(table_id, limit_encoded) because EncodeKeyForPrefixLimit
    // appends 0xFF bytes, changing the byte length. The 4-byte big-endian
    // length prefix in MakeRowPrefix would then differ (e.g. 0x09 vs 0x11),
    // causing ALL subsequent rows to sort before the limit — effectively
    // scanning the entire table instead of just the target range.
    if (!limit_encoded.empty() && encoded_key >= limit_encoded) break;

    // Read column_id.
    uint32_t col_len;
    if (!ReadLengthPrefix(kdata, ksize, offset, &col_len)) continue;
    size_t col_start = offset + 4;
    offset += 4 + col_len;
    if (offset > ksize) continue;
    std::string col_id(kdata + col_start, col_len);

    // Read timestamp (last 8 bytes).
    if (offset + 8 > ksize) continue;
    std::string entry_ts(kdata + offset, 8);

    // Only consider columns we need.
    if (needed_columns.find(col_id) == needed_columns.end()) continue;

    // Only consider entries at or before the target timestamp.
    // Timestamps are encoded ascending, so entry_ts <= ts_encoded means
    // the entry is at or before our target.
    if (entry_ts > ts_encoded) continue;

    // Keep the latest version at or before timestamp.
    auto& cell = rows_data[encoded_key][col_id];
    if (cell.best_timestamp.empty() || entry_ts > cell.best_timestamp) {
      cell.best_value = it->value().ToString();
      cell.best_timestamp = entry_ts;
    }
  }
  GOOGLESQL_RETURN_IF_ERROR(CheckIteratorStatus(*it));

  // Build result rows from collected data.
  for (auto& [encoded_key, columns] : rows_data) {
    // Check _exists.
    auto exists_it = columns.find(std::string(kExistsColumn));
    if (exists_it == columns.end()) continue;
    googlesql::Value exists_val = DecodeValue(exists_it->second.best_value);
    // Match Exists() logic: valid AND not-null AND true.
    if (!exists_val.is_valid() || exists_val.is_null() ||
        !exists_val.bool_value()) {
      continue;
    }

    // Collect column values in order.
    std::vector<googlesql::Value> values;
    values.reserve(column_ids.size());
    for (const ColumnID& column_id : column_ids) {
      auto col_it = columns.find(column_id);
      if (col_it != columns.end()) {
        values.emplace_back(DecodeValue(col_it->second.best_value));
      } else {
        values.emplace_back(googlesql::Value());
      }
    }

    // Reconstruct key from __key_data__.
    Key reconstructed_key;
    auto kd_it = columns.find("__key_data__");
    if (kd_it != columns.end()) {
      googlesql::Value key_data = DecodeValue(kd_it->second.best_value);
      reconstructed_key = DecodeKeyData(key_data);
    }

    rows.emplace_back(std::make_pair(reconstructed_key, std::move(values)));
  }

  *itr = std::make_unique<SnapshotOwningIterator>(
      std::move(rows), db_.get(), snapshot.Release());
  return absl::OkStatus();
}

absl::Status PersistentStorage::Write(
    absl::Time timestamp, const TableID& table_id, const Key& key,
    const std::vector<ColumnID>& column_ids,
    const std::vector<googlesql::Value>& values) {
  std::string encoded_key = EncodeKey(key);
  leveldb::WriteBatch batch;

  // Write _exists column if the row doesn't exist yet.
  if (!Exists(table_id, encoded_key, timestamp)) {
    std::string exists_ldb_key =
        MakeLevelDBKey(table_id, encoded_key, kExistsColumn, timestamp);
    std::string exists_value = EncodeValue(googlesql::values::Bool(true));
    batch.Put(exists_ldb_key, exists_value);
  }

  // Write the key metadata for reconstruction during Read().
  // Format: num_columns(4 bytes LE), then per column:
  //   is_descending(1 byte), is_nulls_last(1 byte),
  //   value_len(4 bytes LE), encoded_value
  {
    std::string key_data;
    int32_t num_cols = key.NumColumns();
    // Encode num_cols as explicit little-endian.
    char nc_buf[4];
    nc_buf[0] = static_cast<char>(num_cols & 0xFF);
    nc_buf[1] = static_cast<char>((num_cols >> 8) & 0xFF);
    nc_buf[2] = static_cast<char>((num_cols >> 16) & 0xFF);
    nc_buf[3] = static_cast<char>((num_cols >> 24) & 0xFF);
    key_data.append(nc_buf, 4);
    for (int i = 0; i < num_cols; ++i) {
      key_data.push_back(key.IsColumnDescending(i) ? 1 : 0);
      key_data.push_back(key.IsColumnNullsLast(i) ? 1 : 0);
      std::string encoded_val = EncodeValue(key.ColumnValue(i));
      int32_t val_len = static_cast<int32_t>(encoded_val.size());
      // Encode val_len as explicit little-endian.
      char vl_buf[4];
      vl_buf[0] = static_cast<char>(val_len & 0xFF);
      vl_buf[1] = static_cast<char>((val_len >> 8) & 0xFF);
      vl_buf[2] = static_cast<char>((val_len >> 16) & 0xFF);
      vl_buf[3] = static_cast<char>((val_len >> 24) & 0xFF);
      key_data.append(vl_buf, 4);
      key_data.append(encoded_val);
    }
    std::string key_meta_ldb_key =
        MakeLevelDBKey(table_id, encoded_key, "__key_data__", timestamp);
    std::string key_meta_value =
        EncodeValue(googlesql::values::Bytes(key_data));
    batch.Put(key_meta_ldb_key, key_meta_value);
  }

  // Write the column values.
  for (size_t i = 0; i < column_ids.size(); ++i) {
    std::string ldb_key =
        MakeLevelDBKey(table_id, encoded_key, column_ids[i], timestamp);
    std::string encoded_value = EncodeValue(values[i]);
    batch.Put(ldb_key, encoded_value);
  }

  leveldb::Status status = write_queue_.Submit(std::move(batch));
  if (!status.ok()) {
    return LevelDBStatusToAbsl(status);
  }

  // Remove expired versions for each cell that was written.
  leveldb::WriteBatch gc_batch;
  std::string row_prefix;
  AppendLengthPrefixed(&row_prefix, table_id);
  AppendLengthPrefixed(&row_prefix, encoded_key);

  std::string exists_prefix = row_prefix;
  AppendLengthPrefixed(&exists_prefix, std::string(kExistsColumn));
  RemoveExpiredVersions(exists_prefix, timestamp, &gc_batch);

  std::string key_data_prefix = row_prefix;
  AppendLengthPrefixed(&key_data_prefix, std::string("__key_data__"));
  RemoveExpiredVersions(key_data_prefix, timestamp, &gc_batch);

  for (const ColumnID& column_id : column_ids) {
    std::string cell_prefix = row_prefix;
    AppendLengthPrefixed(&cell_prefix, column_id);
    RemoveExpiredVersions(cell_prefix, timestamp, &gc_batch);
  }
  write_queue_.Submit(std::move(gc_batch));  // Best-effort GC.

  return absl::OkStatus();
}

absl::Status PersistentStorage::Delete(absl::Time timestamp,
                                       const TableID& table_id,
                                       const KeyRange& key_range) {
  if (!key_range.IsClosedOpen()) {
    return error::Internal(
        absl::StrCat("PersistentStorage::Delete should be called "
                     "with ClosedOpen key range, found: ",
                     key_range.DebugString()));
  }
  if (key_range.start_key() >= key_range.limit_key()) {
    return absl::OkStatus();
  }

  std::string start_encoded = EncodeKey(key_range.start_key());
  // Use EncodeKeyForPrefixLimit for prefix-limit keys (e.g. from
  // KeyRange::Point), otherwise use EncodeKey for regular limit keys.
  // A prefix-limit key has the same columns but compares greater --
  // detected when both encode identically but the limit compares greater.
  // When both are empty (Key::Empty/Infinity), keep limit empty to mean
  // "no upper bound" (the original convention used by CollectKeysInRange).
  std::string limit_encoded = EncodeKey(key_range.limit_key());
  if (!limit_encoded.empty() &&
      limit_encoded == EncodeKey(key_range.start_key()) &&
      key_range.start_key() < key_range.limit_key()) {
    limit_encoded = EncodeKeyForPrefixLimit(key_range.limit_key());
  }

  std::vector<std::string> encoded_keys =
      CollectKeysInRange(table_id, start_encoded, limit_encoded);

  leveldb::WriteBatch batch;

  // Track seen columns per key for GC pass.
  std::map<std::string, std::set<std::string>> per_key_columns;

  for (const std::string& encoded_key : encoded_keys) {
    if (!Exists(table_id, encoded_key, timestamp)) {
      continue;
    }

    // Mark _exists as false.
    std::string exists_ldb_key =
        MakeLevelDBKey(table_id, encoded_key, kExistsColumn, timestamp);
    std::string exists_value = EncodeValue(googlesql::values::Bool(false));
    batch.Put(exists_ldb_key, exists_value);

    // Scan all columns for this row and mark them as invalid.
    std::string row_prefix = MakeRowPrefix(table_id, encoded_key);
    std::unique_ptr<leveldb::Iterator> it(
        db_->NewIterator(leveldb::ReadOptions()));

    std::set<std::string>& seen_columns = per_key_columns[encoded_key];
    for (it->Seek(row_prefix); it->Valid(); it->Next()) {
      leveldb::Slice ldb_key = it->key();
      if (!ldb_key.starts_with(row_prefix)) break;

      // Extract column_id from the length-prefixed LevelDB key.
      std::string col_id = ExtractColumnIdFromLevelDBKey(ldb_key);
      if (col_id.empty()) continue;
      if (col_id == kExistsColumn || col_id == "__key_data__") continue;

      if (seen_columns.insert(col_id).second) {
        // Write an invalid value for this column at the delete timestamp.
        std::string del_ldb_key =
            MakeLevelDBKey(table_id, encoded_key, col_id, timestamp);
        std::string invalid_value = EncodeValue(googlesql::Value());
        batch.Put(del_ldb_key, invalid_value);
      }
    }
    GOOGLESQL_RETURN_IF_ERROR(CheckIteratorStatus(*it));
  }

  leveldb::Status status = write_queue_.Submit(std::move(batch));
  if (!status.ok()) {
    return LevelDBStatusToAbsl(status);
  }

  // Remove expired versions for all cells in deleted rows.
  leveldb::WriteBatch gc_batch;
  for (const auto& [encoded_key, columns] : per_key_columns) {
    std::string row_prefix = MakeRowPrefix(table_id, encoded_key);

    std::string exists_prefix = row_prefix;
    AppendLengthPrefixed(&exists_prefix, std::string(kExistsColumn));
    RemoveExpiredVersions(exists_prefix, timestamp, &gc_batch);

    std::string key_data_prefix = row_prefix;
    AppendLengthPrefixed(&key_data_prefix, std::string("__key_data__"));
    RemoveExpiredVersions(key_data_prefix, timestamp, &gc_batch);

    for (const std::string& col_id : columns) {
      std::string cell_prefix = row_prefix;
      AppendLengthPrefixed(&cell_prefix, col_id);
      RemoveExpiredVersions(cell_prefix, timestamp, &gc_batch);
    }
  }
  write_queue_.Submit(std::move(gc_batch));  // Best-effort GC.

  return absl::OkStatus();
}

void PersistentStorage::RemoveExpiredVersions(const std::string& cell_prefix,
                                              absl::Time timestamp,
                                              leveldb::WriteBatch* batch) {
  absl::MutexLock lock(&version_retention_period_mu_);
  absl::Time cutoff = timestamp - version_retention_period_;
  std::string cutoff_ts = EncodeTimestamp(cutoff);
  std::string cutoff_key = cell_prefix + cutoff_ts;

  // Scan all versions of this cell up to the cutoff timestamp.
  // Keep the most recent version at or before cutoff (needed for reads in the
  // retention window), delete everything older.
  std::unique_ptr<leveldb::Iterator> it(
      db_->NewIterator(leveldb::ReadOptions()));
  it->Seek(cell_prefix);

  std::vector<std::string> keys_to_delete;
  std::string last_key_before_cutoff;

  for (; it->Valid(); it->Next()) {
    leveldb::Slice key = it->key();
    if (!key.starts_with(cell_prefix)) break;

    std::string key_str = key.ToString();
    if (key_str <= cutoff_key) {
      if (!last_key_before_cutoff.empty()) {
        keys_to_delete.push_back(last_key_before_cutoff);
      }
      last_key_before_cutoff = key_str;
    } else {
      break;  // Past the cutoff, remaining versions are within retention.
    }
  }
  if (!it->status().ok()) return;  // Skip GC on I/O error.

  // Delete all versions except the most recent one before cutoff.
  for (const auto& key : keys_to_delete) {
    batch->Delete(key);
  }
}

void PersistentStorage::SetVersionRetentionPeriod(
    absl::Duration version_retention_period) {
  absl::MutexLock lock(&version_retention_period_mu_);
  version_retention_period_ = version_retention_period;
}

void PersistentStorage::CleanUpDeletedTables(absl::Time timestamp) {
  absl::MutexLock lock(&mu_);
  absl::MutexLock version_retention_period_lock(&version_retention_period_mu_);
  absl::Time expiration_time = timestamp - version_retention_period_;

  for (auto it = dropped_tables_.begin();
       it != dropped_tables_.upper_bound(expiration_time);) {
    // Delete all entries with the table prefix.
    std::string table_prefix = MakeTablePrefix(it->second);
    leveldb::WriteBatch batch;

    std::unique_ptr<leveldb::Iterator> db_it(
        db_->NewIterator(leveldb::ReadOptions()));
    for (db_it->Seek(table_prefix); db_it->Valid(); db_it->Next()) {
      if (!db_it->key().starts_with(table_prefix)) break;
      batch.Delete(db_it->key());
    }
    if (!db_it->status().ok()) break;  // Stop cleanup on I/O error.
    write_queue_.Submit(std::move(batch));

    it = dropped_tables_.erase(it);
  }
}

void PersistentStorage::CleanUpDeletedColumns(absl::Time timestamp) {
  absl::MutexLock lock(&mu_);
  absl::MutexLock version_retention_period_lock(&version_retention_period_mu_);
  absl::Time expiration_time = timestamp - version_retention_period_;

  for (auto it = dropped_columns_.begin();
       it != dropped_columns_.upper_bound(expiration_time);) {
    auto [table_id, column_id] = it->second;

    // Scan all rows of the table and delete entries for this column.
    std::string table_prefix = MakeTablePrefix(table_id);
    leveldb::WriteBatch batch;

    std::unique_ptr<leveldb::Iterator> db_it(
        db_->NewIterator(leveldb::ReadOptions()));
    for (db_it->Seek(table_prefix); db_it->Valid(); db_it->Next()) {
      leveldb::Slice ldb_key = db_it->key();
      if (!ldb_key.starts_with(table_prefix)) break;

      // Check if this entry belongs to the dropped column.
      std::string found_col = ExtractColumnIdFromLevelDBKey(ldb_key);
      if (found_col == column_id) {
        batch.Delete(ldb_key);
      }
    }
    if (!db_it->status().ok()) break;  // Stop cleanup on I/O error.
    write_queue_.Submit(std::move(batch));

    it = dropped_columns_.erase(it);
  }
}

void PersistentStorage::MarkDroppedTable(absl::Time timestamp,
                                         TableID dropped_table_id) {
  absl::MutexLock lock(&mu_);
  dropped_tables_[timestamp] = dropped_table_id;
}

void PersistentStorage::MarkDroppedColumn(absl::Time timestamp,
                                          TableID dropped_table_id,
                                          ColumnID dropped_column_id) {
  absl::MutexLock lock(&mu_);
  dropped_columns_[timestamp] =
      std::make_pair(dropped_table_id, dropped_column_id);
}

}  // namespace backend
}  // namespace emulator
}  // namespace spanner
}  // namespace google

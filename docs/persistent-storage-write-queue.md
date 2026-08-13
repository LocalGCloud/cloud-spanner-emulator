# Persistent Storage — Implementation

This document describes how persistence is implemented in the Cloud Spanner Emulator: the storage stack, on-disk format, multi-version concurrency control, the WriteQueue serialization model, snapshot isolation, and garbage collection.

---

## Architecture Overview

Persistence is split into two layers:

| Layer | Concern | Backing Store | Location |
|-------|---------|---------------|----------|
| **Metadata** | Instances, databases, DDL, ID counters | JSON file | `frontend/persistence/metadata_store.{h,cc}` |
| **Data** | Row data, multi-version columns | LevelDB (LSM-tree) | `backend/storage/persistent_storage.{h,cc}` |

### Metadata Store

`MetadataStore` persists emulator metadata to `{data_dir}/metadata.json`. Writes are **atomic** — the file is written to a temporary path and then renamed into place, preventing corruption on crash. The file is read once on startup; every metadata mutation triggers a full re-write. Thread safety is provided by `absl::Mutex`.

### Data Store

`PersistentStorage` implements the abstract `Storage` interface on top of **LevelDB** (Google's log-structured merge-tree key-value store). It provides full multi-version concurrency control — every write is stamped with a commit timestamp, and historical versions are preserved for time-travel reads.

---

## LevelDB Key Format

Keys are **length-prefixed** — each variable-length component is preceded by a 4-byte big-endian length. This eliminates ambiguity from embedded `\x00` bytes without requiring reserved separator characters.

```
┌─────────────────┬──────────┬───────────────────┬────────────┬─────────────────┬──────────┬──────────────┐
│ table_id_len:4  │ table_id │ encoded_key_len:4 │ encod'd_key│ column_id_len:4  │ column_id│ timestamp:8  │
│   (big-endian)  │          │   (big-endian)    │            │   (big-endian)   │          │ (big-endian) │
└─────────────────┴──────────┴───────────────────┴────────────┴─────────────────┴──────────┴──────────────┘
```

This layout enables **prefix scanning** at any granularity:

| Prefix | What it scans |
|--------|---------------|
| `{table}` | All rows of a table |
| `{table}{key}` | All columns+versions of a single row |
| `{table}{key}{column}` | All versions of a single cell |

### Timestamp Encoding

Timestamps are encoded as **8-byte big-endian int64 microseconds** with the sign bit flipped (`micros ^ (1ULL << 63)`), converting signed int64 to unsigned comparison order. This matches Cloud Spanner's commit timestamp resolution (microsecond).

> **Note:** `InMemoryStorage` preserves full nanosecond precision. `PersistentStorage` truncates to microseconds — two writes within the same microsecond overwrite each other.

### Encoded Key Format (`key_codec.h`)

The `encoded_key` component is produced by `EncodeKey()` in `backend/storage/key_codec.h`. It transforms a `Key` into a byte string that preserves the sort order defined by `Key::Compare()`:

| Type | Encoding |
|------|----------|
| NULL | `0x00` (nulls-first) or `0xFE` (nulls-last) |
| BOOL | `0x01` (false) or `0x02` (true) |
| INT64 | `0x03` + 8 bytes big-endian, sign bit flipped |
| DOUBLE | `0x04` + 8 bytes IEEE 754, sign bit flipped for negatives |
| STRING | `0x05` + byte-stuffed UTF-8 + `0x00 0x00` terminator |
| BYTES | `0x06` + byte-stuffed bytes + `0x00 0x00` terminator |
| TIMESTAMP | `0x07` + 8 bytes seconds + 4 bytes nanos (big-endian) |
| DATE | `0x08` + 4 bytes big-endian, sign bit flipped |
| NUMERIC | `0x09` + serialized numeric + `0x00` terminator |
| Infinity | `0xFF` |

For **descending** columns, all bytes are bitwise-inverted so that sort order is reversed.

For prefix-limit keys (used in range scans), `EncodeKeyForPrefixLimit()` appends `0xFF` bytes to make the encoded form sort after any key sharing the same prefix.

---

## Multi-Version Concurrency Control

`PersistentStorage` implements an **append-only MVCC** model:

- Every write is stamped with a commit timestamp and stored as a new LevelDB entry
- Old versions are **never overwritten** — the store accumulates all historical values
- Reads specify a target timestamp and see the **latest value ≤ timestamp**
- Deletes write `_exists=false` at the delete timestamp; data at earlier timestamps remains accessible

### Hidden Columns

Two internal columns are stored alongside user data:

| Column | Purpose |
|--------|---------|
| `_exists` | Boolean; `true` = row exists at this timestamp, `false` = row was deleted |
| `__key_data__` | Serialized `Key` metadata (column count, per-column: is_descending, nulls_last, value) — used to reconstruct `Key` objects during `Read()` |

### Lookup

```
Lookup(timestamp, table, key, column_ids) → values:
  1. Encode the key
  2. Check _exists at ≤ timestamp
  3. If NOT_FOUND → return error
  4. For each requested column:
       seek to {table}{key}{column}{timestamp}
       walk backwards to find latest ≤ timestamp version
```

### Read — Single-Pass Scan

Rather than issuing one LevelDB seek per cell, `Read()` does a **single forward scan** over the key range, collecting the best version for each `(encoded_key, column_id)` into an in-memory map. This is efficient because LevelDB key order groups all versions of a cell contiguously.

```
Read(timestamp, table, key_range, column_ids) → iterator:
  1. Acquire LevelDB snapshot   ← point-in-time anchor
  2. Create LevelDB iterator with snapshot
  3. Forward scan from {table}{start_key} to {table}{limit_key}:
       for each LevelDB entry:
         parse {encoded_key, column_id, entry_ts}
         if entry_ts ≤ timestamp AND column is needed:
           keep if it's the latest seen for this (key, column)
  4. Build result rows:
       filter by _exists = true
       extract column values in requested order
       reconstruct Key from __key_data__
  5. Return SnapshotOwningIterator(rows, db_, snapshot)
```

### Key Reconstruction

During `Read()`, keys are reconstructed from the `__key_data__` column. The serialized format is:

```
num_columns:4 (little-endian int32)
per column:
  is_descending:1   (0 or 1)
  is_nulls_last:1   (0 or 1)
  value_len:4       (little-endian int32)
  encoded_value     (variable length)
```

This avoids needing to round-trip through `EncodeKey`/`DecodeKey` — the raw encoded bytes are stored and decoded using the same `value_codec.h` pipeline.

---

## WriteQueue — Race-Free Serialization

### Problem

When multiple gRPC handler threads concurrently called `Write()` or `Read()` on the same `leveldb::DB*` handle, a race condition existed: although LevelDB internally serializes disk writes, the emulator issued sequences of write-read operations without snapshot isolation.

```
Thread A: Write(row_1, col_a = "x")          Thread B: Write(row_1, col_b = "y")
          │                                              │
          ├─ db_->Write(batch{a}) ───────────────────────┤
          │                                              ├─ db_->Write(batch{b})
          │                                              │
          ▼                                              ▼
                    LevelDB: writes are interleaved
                    Read may see {col_a = "x"} but not {col_b = "y"}
                    → partially-committed row
```

**Manifestations:**
- Reads observing partially-committed writes from concurrent transactions
- Inconsistent query results
- Corrupted SST files after restart
- Data loss under concurrent workloads

### Solution: Two Complementary Mechanisms

| Mechanism | Purpose | Implementation |
|-----------|---------|----------------|
| **WriteQueue** | Serialize all writes through one worker thread | `std::thread` + `std::condition_variable` + `std::queue` |
| **Snapshot Reads** | Point-in-time consistency for MVCC reads | `leveldb::Snapshot*` API |

```
                    ┌──────────────┐
Thread 1: Write() ──│              │
Thread 2: Write() ──│  WriteQueue  │──→ Worker Thread ──→ leveldb::DB::Write()
Thread 3: Delete() ─│  (mutex+     │         │
                    │   condvar)   │    ┌────┴────┐
                    └──────────────┘    │ Snapshot │
Thread 4: Read() ──────────────────────│ (point-  │──→ Iterator
                                       │ in-time) │
                                       └──────────┘
```

### Class Interface

```cpp
class WriteQueue {
 public:
  explicit WriteQueue(leveldb::DB* db);
  ~WriteQueue();

  // Submits a batch to the worker thread. Blocks until committed.
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
```

### Submit() — Blocking Enqueue

```
Submit(batch):
  1. Acquire mu_
  2. If shutdown_ → return IOError immediately
  3. Push batch onto queue_
  4. Notify worker thread (cv_.notify_one)
  5. Block on cv_.wait until results_ is non-empty or shutdown_
  6. If shutdown_ and no results → return IOError
  7. Pop result from results_, return it
```

`Submit()` blocks the calling thread until its specific batch has been processed and committed to LevelDB. This preserves the synchronous API contract of `Storage::Write()`.

### WorkerLoop() — Serial Processing

```
WorkerLoop():
  1. Acquire mu_
  2. Loop:
     a. cv_.wait until queue_ is non-empty or shutdown_
     b. If shutdown_ AND queue_ empty → break (exit)
     c. While queue_ not empty:
        - Pop batch from queue_
        - Release mu_              ← allow new submissions to queue
        - Call db_->Write(WriteOptions(), &batch)  ← the ONLY place this happens
        - Re-acquire mu_
        - Push Status onto results_
        - cv_.notify_all           ← wake waiting submitters
```

**Key properties:**
- Worker processes exactly one batch at a time while NOT holding `mu_`, allowing concurrent submissions to queue up
- After each batch commits, all waiting submitters are woken; only the one whose result is ready will pop it
- On shutdown, worker drains all remaining batches before exiting

### Shutdown() — Graceful Drain

```
Shutdown():
  1. Acquire mu_, set shutdown_ = true
  2. cv_.notify_one  → wake the worker (not submitters)
  3. Release mu_
  4. worker_.join()  → wait for worker to finish draining
  5. Re-acquire mu_, cv_.notify_all → safety net for any stragglers
```

**Guarantees:**
- New submissions after `shutdown_` is set return `IOError` immediately
- Batches queued before shutdown are all processed before the worker exits
- Submitters waiting on those batches receive their results (not errors)
- Worker thread is joined before `WriteQueue` destructor returns, ensuring all writes are flushed before `db_` is destroyed

### Concurrency Correctness

**Multiple concurrent submitters:**

```
Thread A: push A  ──┐                    Worker:
Thread B: push B  ──┤ queue: [A, B]  →    pop A → Write(A) → push result_A → notify_all
Thread C: push C  ──┘                     pop B → Write(B) → push result_B → notify_all
                                          pop C → Write(C) → push result_C → notify_all
```

Each submitter blocks on `cv_.wait()` with predicate `!results_.empty() || shutdown_`. When the worker notifies after each batch, exactly one submitter wakes up, acquires `mu_`, pops its result, and returns. Others re-check the predicate, see `results_.empty()`, and go back to sleep.

**Shutdown with pending submitters:**

```
Thread A: push A → wait                   Worker:
Thread B: Shutdown:                        shutdown_=true, notified
  set shutdown_=true                       queue has [A] → process A → push result_A → notify_all
  notify worker                            queue empty, shutdown_=true → break
  join worker                              exit
  notify_all (safety)
                                         Thread A: wakes, sees result_A → pops it → returns OK
```

---

## SnapshotOwningIterator — Snapshot Lifecycle Management

A wrapper class that owns a `leveldb::Snapshot*` and releases it on destruction:

```cpp
class SnapshotOwningIterator : public StorageIterator {
  FixedRowStorageIterator inner_;    // Delegates all iteration
  leveldb::DB* db_;
  const leveldb::Snapshot* snapshot_;

  ~SnapshotOwningIterator() {
    if (db_ && snapshot_) {
      db_->ReleaseSnapshot(snapshot_);  // ← prevents SST file leakage
    }
  }
};
```

**Why a wrapper and not direct modification of `FixedRowStorageIterator`?**
- Avoids coupling the general-purpose `in_memory_iterator` library to LevelDB
- No BUILD dependency changes needed
- Snapshot ownership is specific to `PersistentStorage`, not a general iterator concern

**Guarantee**: All rows returned by a single `Read()` call reflect the database state at the moment `GetSnapshot()` was called. Writes committed after that moment are invisible.

---

## Locking Model

| Method | Lock | Rationale |
|--------|------|-----------|
| `Write()` | None | Batch building reads are thread-safe; writes serialized by WriteQueue |
| `Delete()` | None | Same as Write() |
| `Read()` | None | Snapshot isolation provides consistency |
| `Lookup()` | None | Timestamp-keyed reads don't need external sync |
| `CleanUpDeletedTables()` | `MutexLock(mu_)` | Protects `dropped_tables_` map |
| `CleanUpDeletedColumns()` | `MutexLock(mu_)` | Protects `dropped_columns_` map |
| `MarkDroppedTable()` | `MutexLock(mu_)` | Protects `dropped_tables_` map |
| `MarkDroppedColumn()` | `MutexLock(mu_)` | Protects `dropped_columns_` map |
| `SetVersionRetentionPeriod()` | `MutexLock(version_retention_period_mu_)` | Protects the duration field |

**Key insight**: `mu_` is scoped to protecting only the `dropped_tables_` and `dropped_columns_` maps. LevelDB's own thread safety + WriteQueue serialization + snapshot isolation handle all data consistency. `version_retention_period_mu_` is a separate mutex to avoid deadlock with `mu_` in cleanup methods.

---

## Garbage Collection (Version Retention)

Old MVCC versions are pruned via `RemoveExpiredVersions()`.

### Trigger

- Called at the end of every `Write()` and `Delete()` as a **best-effort** pass — errors are silently ignored
- Also triggered by `CleanUpDeletedTables()` and `CleanUpDeletedColumns()` for dropped schema objects

### Retention Window

Configurable via `SetVersionRetentionPeriod()` — default is **1 hour**.

### Algorithm

```
RemoveExpiredVersions(cell_prefix, timestamp, batch):
  1. cutoff = timestamp - version_retention_period
  2. Seek to cell_prefix in LevelDB
  3. Scan forward, collecting all keys with timestamp ≤ cutoff
  4. Keep the single most recent key ≤ cutoff (needed for reads in the retention window)
  5. Delete all older keys from the batch
```

### Dropped Tables and Columns

When a table or column is dropped, it's recorded in `dropped_tables_` or `dropped_columns_` maps (keyed by drop timestamp). After the retention period expires, `CleanUpDeletedTables()` / `CleanUpDeletedColumns()` physically delete all LevelDB entries for the dropped schema object.

---

## Complete Write Path

```
gRPC handler → PersistentStorage::Write(timestamp, table, key, columns, values):
  │
  ├─ 1. Build WriteBatch (in memory, thread-safe)
  │     ├─ Check if row exists (LevelDB read)
  │     ├─ If new row → Put _exists=true
  │     ├─ Put __key_data__ metadata (for key reconstruction during Read)
  │     └─ Put all column values
  │
  ├─ 2. write_queue_.Submit(main_batch)        ← blocks until committed
  │     └─ Worker: db_->Write(main_batch)      ← ONLY place db_->Write is called
  │
  ├─ 3. Build GC WriteBatch
  │     └─ RemoveExpiredVersions for _exists, __key_data__, and each column
  │
  └─ 4. write_queue_.Submit(gc_batch)           ← best-effort, non-blocking for caller
        └─ Worker: db_->Write(gc_batch)
```

## Complete Read Path

```
gRPC handler → PersistentStorage::Read(timestamp, table, key_range, columns) → iterator:
  │
  ├─ 1. Acquire LevelDB snapshot  ← point-in-time anchor
  │     snapshot = db_->GetSnapshot()
  │
  ├─ 2. Single-pass scan with snapshot
  │     for {table}{start_key} … {table}{limit_key}:
  │       parse {encoded_key, column_id, entry_ts}
  │       if entry_ts ≤ timestamp AND column is needed:
  │         collect best (latest ≤ timestamp) value per (key, column)
  │
  ├─ 3. Build result rows
  │     ├─ Filter by _exists = true
  │     ├─ Extract column values in requested order
  │     └─ Reconstruct Key from __key_data__
  │
  └─ 4. Return SnapshotOwningIterator(rows, db_, snapshot)
        └─ Caller iterates through buffered rows
        └─ When iterator destroyed → db_->ReleaseSnapshot(snapshot)
```

## Complete Delete Path

```
gRPC handler → PersistentStorage::Delete(timestamp, table, key_range):
  │
  ├─ 1. Collect keys in range (LevelDB scan)
  │
  ├─ 2. Build WriteBatch
  │     ├─ For each existing row:
  │     │     Put _exists=false at delete timestamp
  │     │     Scan row for all column IDs
  │     │     Put invalid (null) values for each column at delete timestamp
  │
  ├─ 3. write_queue_.Submit(main_batch)
  │
  └─ 4. write_queue_.Submit(gc_batch)  ← best-effort GC
```

---

## Edge Cases Handled

| Scenario | Behavior |
|----------|----------|
| `Submit()` called after `Shutdown()` | Returns `IOError("WriteQueue is shutting down")` immediately |
| `Shutdown()` during active `Submit()` wait | Worker drains remaining queue; submitter gets result (not error) |
| Worker thread hits LevelDB error | Error returned to submitter; worker continues processing next batch |
| Multiple threads submit same key | Both batches queued; processed sequentially; second overwrites first (correct MVCC behavior) |
| Read during concurrent write | Snapshot acquired at Read() start; all writes committed after are invisible |
| Iterator destroyed late | Snapshot released on iterator destruction; prevents LevelDB SST file accumulation |
| `PersistentStorage` destructor | `write_queue_.Shutdown()` called before `db_` destroyed; ensures all writes flushed |
| Nested directories for data path | `std::filesystem::create_directories()` creates all intermediate dirs (mkdir -p equivalent) |
| Two writes in same microsecond | Second overwrites first (LevelDB Put semantics within same key) |
| Read on empty range (start ≥ limit) | Returns empty iterator immediately |
| I/O error during scan | `CheckIteratorStatus()` returns the error after the scan completes |
| I/O error during GC | Silently skipped — GC is best-effort |

---

## Performance Characteristics

| Metric | Characteristic |
|--------|---------------|
| Write latency | Single-threaded serialization adds ~microseconds of queuing in dev environments |
| Read latency | Unchanged; snapshot acquisition is O(1) reference-count bump |
| Read memory | Single-pass scan materializes result set in memory (O(result size)) |
| Write memory | WriteQueue uses `std::queue`; typically 0-1 entries in dev workloads |
| Write throughput | Bounded by single LevelDB write throughput (~10K writes/sec); sufficient for dev emulator |
| Storage overhead | MVCC accumulates versions; GC prunes beyond retention period |
| LevelDB write buffer | 64 MB |
| LevelDB max open files | 1000 |

---

## Dependencies

- **External**: LevelDB (already in the dependency graph)
- **Standard library**: `std::thread`, `std::mutex`, `std::condition_variable`, `std::queue`, `std::filesystem` (C++17)
- **Internal**: `key_codec.h` (key encoding), `value_codec.h` (value encoding), `in_memory_iterator.h` (result buffering)

---

## Key Files

| File | Role |
|------|------|
| `backend/storage/storage.h` | Abstract `Storage` interface (MVCC contract) |
| `backend/storage/persistent_storage.h` | LevelDB-backed implementation + `WriteQueue` inner class |
| `backend/storage/persistent_storage.cc` | Full implementation (~970 lines) |
| `backend/storage/key_codec.h` | `Key` → sort-preserving byte encoding |
| `backend/storage/value_codec.h` | `Value` ↔ byte serialization |
| `backend/storage/in_memory_iterator.h` | `FixedRowStorageIterator` for buffered results |
| `backend/storage/persistent_storage_test.cc` | Tests (~680 lines) |
| `frontend/persistence/metadata_store.{h,cc}` | JSON metadata persistence |
| `docs/persistent-storage-write-queue.md` | This document |

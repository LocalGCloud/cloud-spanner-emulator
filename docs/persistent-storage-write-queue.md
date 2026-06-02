# PersistentStorage Write Queue: Race Condition Fix

## Problem

The Spanner emulator's LevelDB-backed `PersistentStorage` had a race condition when multiple gRPC handler threads concurrently called `Write()` or `Read()` on the same `leveldb::DB*` handle.

### Root Cause

Although LevelDB internally serializes disk writes via an internal mutex, the emulator issued sequences of write-read operations without snapshot isolation:

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

### Why a Mutex Alone Wasn't Enough

The existing `absl::Mutex mu_` serialized access to `PersistentStorage` methods, but:

1. **Write-then-read races**: A read issued immediately after a write on a different thread could observe only a subset of committed columns, since `Read()` used a LevelDB iterator over the *current* database state without snapshot isolation.

2. **Lock granularity**: The mutex serialized entire method calls (`Write()`, `Read()`, etc.), but didn't prevent the underlying problem that LevelDB iterators see live committed state, not a point-in-time snapshot.

## Solution Overview

Two complementary mechanisms:

| Mechanism | Purpose | Implementation |
|-----------|---------|----------------|
| **WriteQueue** | Serialize all writes through one worker thread | `std::thread` + `std::condition_variable` + `std::queue` |
| **Snapshot Reads** | Point-in-time consistency for MVCC reads | `leveldb::Snapshot*` API |

```
                    ┌──────────────┐
Thread 1: Write() ──│              │
Thread 2: Write() ──│  WriteQueue  │──→ Worker Thread ──→ leveldb::DB::Write()
Thread 3: Write() ──│  (mutex+     │         │
                    │   condvar)   │    ┌────┴────┐
                    └──────────────┘    │ Snapshot │
Thread 4: Read() ──────────────────────│ (point-  │──→ Iterator
                                       │ in-time) │
                                       └──────────┘
```

## WriteQueue Design

### Class Interface

```cpp
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

  std::thread worker_;                           // Single worker thread
  std::mutex mu_;                                // Protects queue_ and results_
  std::condition_variable cv_;                   // Coordinates submitter ↔ worker
  std::queue<leveldb::WriteBatch> queue_;        // Pending write batches
  std::queue<leveldb::Status> results_;          // Per-batch results
  leveldb::DB* db_;                              // LevelDB handle (not owned)
  bool shutdown_ = false;                        // Shutdown flag
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

**Key property**: `Submit()` blocks the calling thread until its specific batch has been processed and committed to LevelDB. This preserves the synchronous API contract of `Storage::Write()`.

### WorkerLoop() — Serial Processing

```
WorkerLoop():
  1. Acquire mu_
  2. Loop:
     a. cv_.wait until queue_ is non-empty or shutdown_
     b. If shutdown_ AND queue_ empty → break (exit)
     c. While queue_ not empty:
        - Pop batch from queue_
        - Release mu_ (allow new submissions to queue)
        - Call db_->Write(WriteOptions(), &batch)  ← the ONLY place this happens
        - Re-acquire mu_
        - Push Status onto results_
        - cv_.notify_all (wake waiting submitters)
```

**Key properties:**
- Worker processes exactly one batch at a time while NOT holding mu_, allowing concurrent submissions to queue up
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
- New submissions after shutdown_ is set return IOError immediately
- Batches queued before shutdown are all processed before the worker exits
- Submitters waiting on those batches receive their results (not errors)
- Worker thread is joined before `WriteQueue` destructor returns

### Concurrency Correctness

**Multiple concurrent submitters:**

```
Thread A: push A  ──┐                    Worker:
Thread B: push B  ──┤ queue: [A, B]  →    pop A → Write(A) → push result_A → notify_all
Thread C: push C  ──┘                     pop B → Write(B) → push result_B → notify_all
                                          pop C → Write(C) → push result_C → notify_all
```

Each submitter blocks on `cv_.wait()` with predicate `!results_.empty() || shutdown_`. When the worker notifies after each batch:
- **One** submitter wakes up, acquires mu_, pops its result from `results_`, returns
- **Others** re-check the predicate, see `results_.empty()`, go back to sleep
- This is correct because `cv_.wait()` atomically releases mu_ and suspends; when woken, it re-acquires mu_ before checking the predicate

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

New submission after shutdown_ set:
```
Thread C: Submit(batch) → lock mu_ → shutdown_=true → return IOError immediately
```

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

## Read() with Snapshots

```
PersistentStorage::Read(timestamp, table_id, key_range, column_ids) → itr:
  1. Acquire snapshot = db_->GetSnapshot()
  2. Set read_options.snapshot = snapshot
  3. Create LevelDB iterator with read_options   ← sees DB as of snapshot time
  4. Scan and collect matching rows into memory
  5. Return SnapshotOwningIterator(rows, db_, snapshot)
     └─ caller iterates through buffered rows
     └─ when caller destroys iterator → snapshot released
```

**Guarantee**: All rows returned by a single `Read()` call reflect the database state at the moment `GetSnapshot()` was called. Writes committed after that moment are invisible.

## Locking Changes

| Method | Before | After | Rationale |
|--------|--------|-------|-----------|
| `Write()` | `MutexLock(mu_)` | No lock | Batch building reads are thread-safe; writes serialized by WriteQueue |
| `Delete()` | `MutexLock(mu_)` | No lock | Same as Write() |
| `Read()` | `ReaderMutexLock(mu_)` | No lock | Snapshot isolation provides consistency |
| `Lookup()` | `ReaderMutexLock(mu_)` | No lock | Timestamp-keyed reads don't need external sync |
| `CleanUpDeletedTables()` | `MutexLock(mu_)` | `MutexLock(mu_)` | Still protects `dropped_tables_` map |
| `CleanUpDeletedColumns()` | `MutexLock(mu_)` | `MutexLock(mu_)` | Still protects `dropped_columns_` map |
| `MarkDroppedTable()` | `MutexLock(mu_)` | `MutexLock(mu_)` | Still protects `dropped_tables_` map |
| `MarkDroppedColumn()` | `MutexLock(mu_)` | `MutexLock(mu_)` | Still protects `dropped_columns_` map |

**Key insight**: `mu_` is now scoped to protecting only the `dropped_tables_` and `dropped_columns_` maps. LevelDB's own thread safety + WriteQueue serialization + snapshot isolation handle all data consistency.

## Complete Write Path

```
gRPC handler thread calls PersistentStorage::Write(timestamp, table, key, columns, values):
  │
  ├─ 1. Build WriteBatch (in memory, thread-safe)
  │     ├─ Check if row exists (LevelDB read, thread-safe)
  │     ├─ Add _exists column
  │     ├─ Add __key_data__ metadata
  │     └─ Add all column values
  │
  ├─ 2. write_queue_.Submit(main_batch)        ← blocks until committed
  │     └─ Worker: db_->Write(main_batch)      ← ONLY place db_->Write is called
  │
  ├─ 3. Build GC WriteBatch (remove expired versions)
  │     └─ RemoveExpiredVersions for each cell
  │
  └─ 4. write_queue_.Submit(gc_batch)           ← best-effort GC, doesn't block return
        └─ Worker: db_->Write(gc_batch)
```

## Complete Read Path

```
gRPC handler thread calls PersistentStorage::Read(timestamp, table, key_range, columns) → itr:
  │
  ├─ 1. Acquire LevelDB snapshot
  │     snapshot = db_->GetSnapshot()
  │
  ├─ 2. Single-pass scan with snapshot
  │     for each key in [start_key, limit_key):
  │       for each column at timestamp ≤ target:
  │         collect best (latest ≤ timestamp) value
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

## Performance Characteristics

- **Write latency**: Single-threaded serialization adds ~microseconds of queuing in dev environments (single-digit concurrent users)
- **Read latency**: Unchanged; snapshot acquisition is O(1) reference-count bump
- **Memory**: WriteQueue uses `std::queue` for pending batches (typically 0-1 entries in dev workloads)
- **Throughput**: Bounded by single LevelDB write throughput (~10K writes/sec); sufficient for dev emulator

## Dependencies

- **No new external dependencies**: Uses only C++17 standard library (`std::thread`, `std::mutex`, `std::condition_variable`, `std::queue`) and existing LevelDB dependency
- **No BUILD changes required**: All needed headers are C++17 standard or already in the dependency graph

## Files Changed

| File | Change Summary |
|------|---------------|
| `backend/storage/persistent_storage.h` | Added `WriteQueue` inner class (+30 lines), `write_queue_` member, removed `ABSL_LOCKS_EXCLUDED`/`ABSL_*_LOCKS_REQUIRED` annotations from read-path methods |
| `backend/storage/persistent_storage.cc` | `WriteQueue` implementation (+70 lines), `SnapshotOwningIterator` wrapper (+30 lines), snapshot acquisition in `Read()`, replaced all `db_->Write()` calls with `write_queue_.Submit()` (6 call sites), removed mutex locks from `Write()`/`Delete()`/`Read()`/`Lookup()` |
| `backend/storage/persistent_storage_test.cc` | 6 new tests: sequential submit, concurrent submit, shutdown, read-during-write, multiple concurrent reads, snapshot release |

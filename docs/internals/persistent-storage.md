# Persistent storage internals

This document describes how `--data_dir` persistence is implemented: the
metadata files, the on-disk layout and marker files, the startup sequence, the
LevelDB storage format, multi-version concurrency control (MVCC), the write
queue, checkpoints, and garbage collection.

It is for people changing the code. For user-facing behavior, see
[Persistence](../persistence.md).

## Components

| Component | Holds | Backing store | Source |
|-----------|-------|---------------|--------|
| `MetadataStore` | Instances, databases, DDL batches, ID counters, IAM policies, custom instance configs, instance partitions, and crash-recovery journals | `<data_dir>/metadata.json` | `frontend/persistence/metadata_store.{h,cc}` |
| `BackupCatalog` | Backups, backup schedules, and long-running operations | `<data_dir>/backup_catalog.json` | `frontend/persistence/backup_catalog.{h,cc}` |
| `PersistentStorage` | Row data, all versions | One LevelDB database per emulator database | `backend/storage/persistent_storage.{h,cc}` |
| Checkpoints | Backup snapshots and DDL rollback copies | LevelDB copies made by `PersistentStorage::CreateCheckpoint` | `backend/storage/persistent_storage.cc`, `backend/database/database.cc` |
| Directory protocol | Legacy migration, markers, orphan cleanup, DDL rollback | Files and folders under `<data_dir>` | `frontend/collections/database_manager.{h,cc}` |
| Startup restore | Loads and reconciles everything above | | `binaries/emulator_main.cc` (`RestoreFromMetadata`) |

`frontend/server/environment.h` creates the `MetadataStore` only when
`--data_dir` is set. The `BackupCatalog` always exists, but it writes nothing
when `--data_dir` is empty (`BackupCatalog::persistent()`).

## Atomic JSON files

Both JSON files are written by `WriteFileAtomicallyNoFollow`
(`frontend/persistence/atomic_file.cc`):

1. Write `<file>.tmp`, opened with `O_CREAT | O_EXCL | O_NOFOLLOW`.
2. `fsync` the temporary file, then `fsync` the parent directory.
3. `rename` it over `<file>`.
4. `fsync` the parent directory again.

A `DATA_LOSS` error after the rename means that the new content is visible but
the final directory sync failed. Callers treat that case as "published".

On load, if `<file>` is missing and `<file>.tmp` exists, the temporary file is
renamed into place. If both exist, the temporary file is deleted after a
successful load. Files are read with `O_NOFOLLOW`, so neither file may be a
symbolic link.

Every metadata change rewrites the whole file. `MetadataStore` and
`BackupCatalog` each guard their state with an `absl::Mutex`.

## `metadata.json` format

The current writer emits version 8. The loader accepts versions 1 to 8 and
returns `DATA_LOSS` for anything else.

```text
{
  "version": 8,
  "instances": {
    "<instance URI>": {
      "displayName", "config", "nodeCount", "processingUnits",
      "createTime", "updateTime", "labels": {...},
      "databases": {
        "<database ID>": {
          "dialect": "GOOGLE_STANDARD_SQL" | "POSTGRESQL",
          "ddlBatches": [
            {"statements": [...], "protoDescriptors": "<base64>",
             "schemaChangeTimestamp": "<RFC 3339, optional>"}
          ],
          "createTime", "enableDropProtection",
          "idCounters": {"tableId", "columnId", "changeStreamId"}
        }
      }
    }
  },
  "iamPolicies":            {"<resource>": "<base64 google.iam.v1.Policy>"},
  "instanceConfigs":        {"<config URI>": "<base64 InstanceConfig>"},
  "instancePartitions":     {"<partition URI>": "<base64 InstancePartition>"},
  "pendingOperations":      {"<operation name>": "<base64 Operation, done>"},
  "pendingBackupDeletions": ["<backup or backup schedule name>"],
  "pendingDdlOperations": {
    "<database URI>": {"operationName", "statements": [...],
                       "protoDescriptors": "<base64>",
                       "hasRollbackCheckpoint": true}
  }
}
```

What each version requires:

| Version | Requirement |
|---------|-------------|
| 1 to 3 | Fields are optional. Databases may use the legacy `ddlStatements` and `protoDescriptors` fields instead of `ddlBatches`. |
| 4 | Every instance, database, and ID counter field shown above, plus `iamPolicies`, `instanceConfigs`, `pendingOperations`, and `pendingBackupDeletions` |
| 5 | `instancePartitions`, each with `create_time` and `update_time` |
| 6 | `nodeCount`. Exactly one of `nodeCount` and `processingUnits` is positive. |
| 7 | `pendingDdlOperations` |
| 8 | `hasRollbackCheckpoint` in each pending DDL operation |

Older files also stored `sequenceId` and `namedSchemaId` counters. The loader
ignores them. Only the table, column, and change stream counters exist in
`MetadataStore::IdCounters` and `backend::Database::IdCounterValues`.

`ddlBatches` holds one entry per committed `UpdateDatabaseDdl` request (the
first entry is the `CreateDatabase` schema). An entry without
`schemaChangeTimestamp` comes from older data. See
[Schema replay](#schema-replay).

## `backup_catalog.json` format

The current writer emits version 4. The loader accepts versions 1 to 4.

```text
{
  "version": 4,
  "backups": {
    "<backup name>": {
      "proto": "<base64 Backup>", "dialect": <int>,
      "ddlBatches": [...],        // same shape as in metadata.json
      "idCounters": {"tableId", "columnId", "changeStreamId"},
      "operationName", "sourceInstanceConfig"
    }
  },
  "schedules":  {"<schedule name>": "<base64 BackupSchedule>"},
  "operations": {"<operation name>": "<base64 Operation>"}
}
```

Version 2 added `schedules` and version 3 added `operations`. Version 4
replaced the per-backup `ddlStatements` and `protoDescriptors` with
`ddlBatches`.

`operations` stores the terminal long-running operations of all persisted
admin RPCs, not only backup operations. At startup they are loaded into the
`OperationManager`.

## Directory layout

```text
<data_dir>/
  metadata.json, metadata.json.tmp
  backup_catalog.json, backup_catalog.json.tmp
  projects/<p>/instances/<i>/databases/<d>/        database root
    storage/                                       LevelDB
    .metadata-committed(.tmp)                      contents: database URI
    .delete-in-progress(.tmp)                      contents: database URI
    .restore-in-progress                           contents: database URI
    .ddl-rollback/<operation ID>/storage/          checkpoint before DDL
    .ddl-restore-staging/, .ddl-mutated-storage/   used while rolling back
  projects/<p>/instances/<i>/databases/<d>.restoring/   RestoreDatabase staging
  backups/<percent-encoded backup name>/storage/   backup snapshot
  backups/<...>.deleting/                          DeleteBackup staging
  .quarantine/<URI with / as _>-<unix micros>/     --repair_corrupted_databases
  .database-migrations/<p>/<i>/<d>/storage/        legacy migration staging
```

`Database::PersistentStorageDirectory` (`backend/database/database.cc`) maps a
database URI to `<data_dir>/<URI>/storage`. It rejects absolute paths, `.`,
`..`, and symbolic links in any component below `data_dir`.
`BackupCatalog::EncodePathComponent` keeps `[A-Za-z0-9._-]` and
percent-encodes every other byte of the backup name.

## Marker-file protocol

Every multi-step change writes its durable intent first, so that startup can
finish or undo it. Marker files hold the database URI, and startup checks that
the contents match the path.

| Operation | Order of durable steps | Startup handling |
|-----------|------------------------|------------------|
| `CreateDatabase` | Build LevelDB in the root. Save `metadata.json` with the database and a pending operation. Write `.metadata-committed`. Save the operation to the catalog. Clear the pending operation. | A root that isn't in the metadata and has no `.metadata-committed` is an interrupted create or restore, and `CleanupOrphanedRestoreDirectories` deletes it. A root that isn't in the metadata but has the marker is `DATA_LOSS` (`Persistent database root has committed data but no metadata`). |
| `RestoreDatabase` | Copy the snapshot into `<root>.restoring` with `.restore-in-progress`, then rename it to the root. Save the metadata. Write `.metadata-committed`. Save the operation. Publish. | `CompleteRecoveredRestoreDirectories` clears `.restore-in-progress` for databases in the metadata. The per-database restore also removes it. |
| `DropDatabase`, `DeleteInstance` | Write `.delete-in-progress`. Save the metadata without the database. Remove the root. | `ReconcileDeletedDatabaseDirectories`: if the database is still in the metadata, the drop didn't commit and the marker is removed. Otherwise the root is deleted. |
| `UpdateDatabaseDdl` | Checkpoint the storage to `.ddl-rollback/<op>/storage`. Save the intent in `pendingDdlOperations`. Apply the schema. Save the metadata with the new batch, counters, and pending operation, and remove the intent. Delete the checkpoint. Save the operation. Clear the pending operation. | If an intent with `hasRollbackCheckpoint` exists, `RestoreDdlRollbackCheckpoint` swaps the checkpoint back in, the database is rebuilt, the intent's statements are applied again, and the operation is recorded. Otherwise any leftover checkpoints are removed. |
| `DeleteBackup`, `DeleteBackupSchedule` | In one metadata save, remove the resource's IAM policy and add its name to `pendingBackupDeletions`. Delete it from the catalog. Remove the name from the pending set and save. | `ReconcilePendingBackupDeletions` finishes each pending deletion. |
| Backup snapshot delete | Rename the snapshot root to `.deleting`. Save the catalog. Remove the staged folder. | If the backup is still in the catalog, the `.deleting` folder is renamed back. `CleanupStaleSnapshotsLocked` removes every folder under `backups/` that the catalog doesn't reference. |
| Resource change plus its operation | Save both in one `metadata.json` write (`pendingOperations`). Copy the operation to the catalog. Clear it. | `ReconcilePendingOperations` promotes every pending operation into the catalog. This is idempotent. |

If saving metadata fails after a DDL change is already applied in LevelDB, the
database is marked `restore_required`. `DatabaseManager::GetDatabase` then
returns `Database recovery is required before serving <uri>` until a restart
replays the intent.

## Startup sequence

`RestoreFromMetadata` in `binaries/emulator_main.cc` runs before the gRPC
server starts. Any error it returns exits the process with `EXIT_FAILURE`.

1. `MetadataStore::Load`.
2. Check the instance URIs and database IDs, and collect the database URIs.
3. Check that each pending DDL intent names a known database and a canonical
   operation.
4. `MigrateLegacyStorageDirectories`: move `<data_dir>/<database ID>/storage`
   to the per-project layout through `.database-migrations/`. A database ID
   that appears in more than one instance is ambiguous and fails startup.
5. `ReconcileDeletedDatabaseDirectories`.
6. `MarkDatabaseMetadataCommitted` for each database. Failures are logged and
   not fatal; the per-database step below catches them.
7. `CleanupOrphanedRestoreDirectories`.
8. `BackupCatalog::Load`: parse the file, open every `READY` snapshot with
   LevelDB to validate it, and delete folders the catalog doesn't reference.
   A bad snapshot is fatal.
9. `ReconcilePendingBackupDeletions`, then `ReconcilePendingOperations`.
10. `CompleteRecoveredRestoreDirectories`.
11. Load catalog operations into the `OperationManager`.
12. Restore custom instance configs, then instances.
13. For each database, run an isolated restore. Parse the dialect and
    timestamps. Handle any DDL checkpoint. Check that `storage/` exists.
    `ReserveDatabase`, then `Creation::Build`, which replays the DDL batches.
    Re-apply any pending DDL intent. `Publish`, then restore drop protection and
    remove `.restore-in-progress`.
    - On failure, log the error. If `--repair_corrupted_databases` is set,
      `QuarantineCorruptedDatabase` renames the database root into
      `.quarantine/` (`DatabaseManager::QuarantineDatabaseDirectory`) and
      removes the metadata entry and IAM policies. Otherwise
      `DatabaseManager::MarkDatabaseUnavailable` records the reason.
14. Restore instance partitions.
15. Restore IAM policies. `Environment::ValidateIamResource` must find each
    resource.

Only step 13 is isolated per database. Every other failure is fatal.

Known interactions, found by reading the code and not reproduced:

- Step 15 calls `DatabaseManager::GetDatabase`, which returns
  `FAILED_PRECONDITION` for a database marked unavailable in step 13. An IAM
  policy on such a database therefore fails the whole startup.
- `DatabaseManager` never clears `unavailable_databases_`. `DropDatabase` on an
  unavailable database removes its metadata and root, but the name stays in
  that map, and `GetDatabase` keeps rejecting it until a restart.

## Schema replay

`Creation::Build` rebuilds each database by applying its DDL batches in order
as `SchemaChangeOperation`s with `replaying_committed_ddl = true`:

- Each batch runs at its `schemaChangeTimestamp`. If the timestamp is missing,
  the first batch uses the database's `createTime`, and later batches use a new
  commit timestamp. This keeps time-based state such as change stream creation
  times.
- `replaying_committed_ddl` skips checks that were added after statements were
  accepted. Currently these are placement checks in
  `backend/schema/updater/schema_updater.cc`.
- `CREATE DATABASE` statements are dropped from each batch before replay.
- The ID generators are seeded from `idCounters`, so replayed tables and
  columns get the same IDs that the LevelDB keys use.
- The change stream backfill checks for any persisted partition row (read at
  `InfiniteFuture`) and skips itself if one exists, so the initial backfill
  runs only once.

## LevelDB key format

Keys are length-prefixed. Each variable-length component has a 4-byte
big-endian length before it, so embedded `\x00` bytes are unambiguous:

```text
table_id_len:4 | table_id | encoded_key_len:4 | encoded_key | column_id_len:4 | column_id | timestamp:8
```

This layout allows prefix scans at three levels:

| Prefix | Scans |
|--------|-------|
| `{table}` | All rows of a table |
| `{table}{key}` | All columns and versions of one row |
| `{table}{key}{column}` | All versions of one cell |

### Timestamp encoding

Timestamps are 8-byte big-endian microseconds since the Unix epoch, with the
sign bit flipped (`micros ^ (1ULL << 63)`) so that unsigned byte order matches
time order. `InMemoryStorage` keeps nanoseconds. `PersistentStorage` truncates
to microseconds, so two writes to one cell within the same microsecond
overwrite each other.

### Encoded key format

`EncodeKey()` in `backend/storage/key_codec.h` turns a `Key` into bytes that
sort in the same order as `Key::Compare()`:

| Type | Encoding |
|------|----------|
| NULL | `0x00` (nulls first) or `0xFE` (nulls last) |
| BOOL | `0x01` (false) or `0x02` (true) |
| INT64 | `0x03` + 8 bytes big-endian, sign bit flipped |
| DOUBLE | `0x04` + 8 bytes IEEE 754, sign bit flipped for negatives |
| STRING | `0x05` + byte-stuffed UTF-8 + `0x00 0x00` terminator |
| BYTES | `0x06` + byte-stuffed bytes + `0x00 0x00` terminator |
| TIMESTAMP | `0x07` + 8 bytes seconds + 4 bytes nanos (big-endian) |
| DATE | `0x08` + 4 bytes big-endian, sign bit flipped |
| NUMERIC | `0x09` + serialized numeric + `0x00` terminator |
| Infinity | `0xFF` |

For descending columns, every byte is inverted to reverse the order.
`EncodeKeyForPrefixLimit()` appends `0xFF` bytes so the limit key sorts after
every key that shares its prefix.

## MVCC

`PersistentStorage` is append-only:

- Every write adds new LevelDB entries stamped with the commit timestamp.
- Reads see the latest version at or before the read timestamp.
- A delete writes `_exists = false` and an invalid value for each column at the
  delete timestamp. Earlier versions stay readable until garbage collection.

### Hidden columns

| Column | Purpose |
|--------|---------|
| `_exists` | `true` if the row exists at this timestamp, `false` after a delete |
| `__key_data__` | The serialized `Key`, used to rebuild keys in `Read()` |

`__key_data__` is a little-endian `int32` column count followed by, for each
column, `is_descending:1`, `is_nulls_last:1`, a little-endian `int32` value
length, and the value encoded with `value_codec.h`.

### Lookup

```text
Lookup(timestamp, table, key, column_ids):
  1. Encode the key.
  2. Check _exists at or before timestamp. NOT_FOUND if absent or false.
  3. For each column, find the latest version at or before timestamp.
```

### Read

`Read()` does one forward scan over the key range and keeps the best version of
each `(encoded_key, column_id)` in memory. LevelDB's key order keeps all
versions of a cell together, so one scan is enough.

```text
Read(timestamp, table, key_range, column_ids):
  1. Take a LevelDB snapshot.
  2. Scan {table}{start_key} to {table}{limit_key} with that snapshot.
     For each entry at or before timestamp, keep the latest per (key, column).
  3. Keep rows whose _exists is true, order the columns, and rebuild each Key
     from __key_data__.
  4. Return a SnapshotOwningIterator(rows, db_, snapshot).
```

All rows from one `Read()` reflect the database at the moment the snapshot was
taken. The result is held in memory, so memory use grows with result size.

`SnapshotOwningIterator` wraps a `FixedRowStorageIterator` and calls
`db_->ReleaseSnapshot()` in its destructor. It lives in `PersistentStorage`
rather than in `in_memory_iterator`, which keeps that library free of LevelDB.

## Write queue

### Why it exists

Before the queue, handler threads called `db_->Write()` directly, and
write-then-read sequences ran without snapshot isolation. That allowed reads to
see partly written rows. The queue sends every row write, delete, and GC batch
through one worker thread, and `Read()` uses LevelDB snapshots.

### Interface

```cpp
class WriteQueue {  // private to PersistentStorage
 public:
  explicit WriteQueue(leveldb::DB* db);
  ~WriteQueue();
  leveldb::Status Submit(leveldb::WriteBatch batch);  // blocks
  void Shutdown();
 private:
  void WorkerLoop();
  std::thread worker_;
  std::mutex mu_;
  std::condition_variable cv_;       // shared by the worker and submitters
  std::queue<leveldb::WriteBatch> queue_;
  std::queue<leveldb::Status> results_;  // one FIFO shared by all submitters
  leveldb::DB* db_;
  bool shutdown_ = false;
};
```

### Behavior

```text
Submit(batch):
  lock mu_
  if shutdown_: return IOError("WriteQueue is shutting down")
  push batch; notify_one
  wait until !results_.empty() || shutdown_
  if shutdown_ and results_ is empty: return IOError("WriteQueue shut down")
  pop and return results_.front()

WorkerLoop():
  lock mu_
  loop:
    wait until !queue_.empty() || shutdown_
    if shutdown_ and queue_ is empty: exit
    while queue_ is not empty:
      pop batch; unlock
      db_->Write(leveldb::WriteOptions(), &batch)   // no sync
      lock; push status to results_; notify_all

Shutdown():
  set shutdown_; notify_one; join the worker; notify_all
```

The worker is the only code that calls `db_->Write()` on a live database. It
drops `mu_` while writing, so other threads can queue batches.

### Result matching

`results_` is one FIFO shared by every submitter. Results aren't tied to the
batch that produced them. When the worker pushes a result and calls
`notify_all`, whichever waiting submitter gets `mu_` first pops it. As a
result:

- A submitter can take the status of an earlier batch and return before its
  own batch is written. The caller can then return before its data is visible.
- A LevelDB error can reach a different caller than the one whose batch
  failed.
- After `Shutdown()` sets `shutdown_`, a waiting submitter whose result isn't
  in the queue yet returns `IOError("WriteQueue shut down")`, even if the
  worker still writes its batch.

In practice these cases need two threads submitting to the same database at
once. The database's transaction lock allows one read-write transaction or
schema change at a time, and `Shutdown()` runs only from the destructor, so the
window is small. It isn't closed. Giving each submitter its own result slot,
such as a promise per batch, would close it.

## Sequence counters

Sequence positions live in memory in `Sequence::SequenceLastValues`, keyed by
a sequence ID that is a random UUID and changes every time the DDL is
replayed. To survive restarts, `SequenceStateStore`
(`backend/storage/sequence_state_store.cc`) keeps one row per sequence in the
database's own storage, in the reserved table `_emulator_sequence_state`
(generated table IDs always contain `:`, so it can't collide), keyed by the
sequence's name with an `INT64` column `next_counter`.

- `Sequence::GetNextSequenceValue` loads `next_counter` on a sequence's first
  use in the process, and saves a counter 1,000 values ahead whenever the
  in-memory counter passes the saved one (`SequenceSavedCounters`). Every
  value handed out therefore came from a counter below the saved one.
- `GET_INTERNAL_SEQUENCE_STATE` returns the saved counter until the sequence
  is used in the process.
- The schema updater forgets the saved counter (writes `NULL`) on live
  `CREATE SEQUENCE`, `DROP SEQUENCE`, and `ALTER SEQUENCE` that sets the start
  counter. Replayed DDL (`replaying_committed_ddl`) leaves it alone, so the
  last incarnation's counter survives a replay of create, drop and recreate.
- The row is written outside the transaction, like the in-memory counter, and
  with the same durability as other row writes. Backups copy it with the rest
  of the LevelDB directory.

Tests: `backend/storage/sequence_state_store_test.cc`,
`QueryEngineTest.SequenceContinuesFromSavedCounterAfterRestart` and
`SequenceSchemaUpdaterTest.SavedCounterFollowsLiveDdlOnly`.

## Checkpoints

`PersistentStorage::CreateCheckpoint(output_dir)` makes a point-in-time copy
of the whole LevelDB database, all versions included:

1. Fail if `output_dir` exists. Create its parent.
2. Open a new LevelDB at `<output_dir>.tmp-<steady clock>-<sequence>`.
3. Iterate the source under a snapshot and copy every entry, in batches of up
   to 4 MB written with `sync = true`.
4. Rename the temporary folder to `output_dir`. On failure, the temporary
   folder is removed.

`Database::CreateBackupCheckpoint` wraps it and returns the capture time. It
backs these operations:

- `CreateBackup` writes `backups/<name>/storage`. `CopyBackup` and
  `RestoreDatabase` copy snapshots with `CopySnapshot` in
  `frontend/handlers/backups.cc`.
- Every `UpdateDatabaseDdl` with `--data_dir` checkpoints to
  `.ddl-rollback/<op>/storage` before the schema change, while holding the
  schema-change lock. DDL cost therefore grows with database size.

`BackupCatalog::AcquireSnapshot` returns a `SnapshotLease` that holds the
catalog mutex while a caller copies a snapshot. This prevents the snapshot
from being deleted during the copy, and it serializes the LevelDB opens used
for validation.

## Locking

| Method | Lock | Notes |
|--------|------|-------|
| `Write()`, `Delete()` | None | Batches are built with plain reads. Writes go through the queue. |
| `Read()` | None | The snapshot gives consistency. |
| `Lookup()` | None | Timestamped reads. |
| `CleanUpDeletedTables()`, `CleanUpDeletedColumns()` | `mu_` and `version_retention_period_mu_` | Protect the dropped-object maps and the retention period. |
| `MarkDroppedTable()`, `MarkDroppedColumn()` | `mu_` | Protects the dropped-object maps. |
| `SetVersionRetentionPeriod()`, `RemoveExpiredVersions()` | `version_retention_period_mu_` | Declared `ABSL_ACQUIRED_AFTER(mu_)`. |

Nothing locks `data_dir` as a whole. LevelDB's own `LOCK` file stops two
processes from opening the same database. `metadata.json` and
`backup_catalog.json` have no lock.

## Garbage collection

`RemoveExpiredVersions(cell_prefix, timestamp, batch)` finds the cutoff
(`timestamp - version_retention_period`). It keeps the newest version at or
before the cutoff and deletes older ones. The retention period comes from the
database's `version_retention_period` option and defaults to 1 hour.

- `Write()` and `Delete()` build a GC batch for the cells they touched and
  submit it after the main batch. The call blocks like any other `Submit()`.
  The returned status is ignored, so GC is best effort. Cells that are never
  written again keep their old versions.
- `MarkDroppedTable()` and `MarkDroppedColumn()` record drops in the in-memory
  `dropped_tables_` and `dropped_columns_` maps, keyed by drop time. Schema
  replay at startup records them again.
- `CleanUpDeletedTables()` runs on every schema change and on every read-only
  transaction read. `CleanUpDeletedColumns()` runs on every schema change. Each
  deletes all entries of objects dropped before the cutoff.

## Write, read, and delete paths

```text
Write(timestamp, table, key, columns, values):
  1. Build a WriteBatch: _exists = true if the row is new, __key_data__, and
     each column value.
  2. write_queue_.Submit(batch). Return any error.
  3. Build a GC batch for _exists, __key_data__, and each written column.
  4. write_queue_.Submit(gc_batch). Blocks. Status ignored.

Delete(timestamp, table, key_range):
  1. Collect the encoded keys in the range.
  2. For each row that exists: _exists = false, and an invalid value for every
     column the row has.
  3. Submit the batch, then a GC batch as in Write().
```

Each `Write()` or `Delete()` call is one row or range. A commit calls them once
per buffered write operation (`backend/transaction/flush.cc`), index rows
included. A multi-row commit is therefore several LevelDB writes and isn't
atomic across a process crash.

## Edge cases

| Scenario | Behavior |
|----------|----------|
| `Submit()` after `Shutdown()` | Returns `IOError("WriteQueue is shutting down")` |
| `Shutdown()` while submitters wait | The worker drains the queue. A waiter can still get `IOError` (see [Result matching](#result-matching)). |
| LevelDB write error | The status goes to whichever submitter pops it. The worker keeps going. |
| Read during a write | The snapshot is taken at the start of `Read()`. Later writes are invisible. |
| Iterator released late | The snapshot is held until the iterator is destroyed. |
| `PersistentStorage` destroyed | `write_queue_.Shutdown()` runs in the destructor. |
| Missing parent folders | `Create()` calls `std::filesystem::create_directories()` first. Without it, LevelDB fails with a `LOCK` error. |
| Two writes to one cell in one microsecond | The second replaces the first. |
| Empty range (start >= limit) | Returns an empty iterator. |
| I/O error during a scan | `CheckIteratorStatus()` returns it after the scan. |
| I/O error during GC | Ignored. |

## LevelDB options

| Option | Value |
|--------|-------|
| `write_buffer_size` | 64 MB |
| `max_open_files` | 1000 |
| `create_if_missing` | true |
| Row writes | `leveldb::WriteOptions()` with `sync = false` |
| Checkpoint writes | `sync = true` |

With `sync = false`, a write survives a process crash but can be lost if the
operating system crashes.

## Source files

| File | Role |
|------|------|
| `backend/storage/storage.h` | Abstract `Storage` interface |
| `backend/storage/persistent_storage.{h,cc}` | LevelDB implementation, `WriteQueue`, `CreateCheckpoint` |
| `backend/storage/key_codec.{h,cc}` | Order-preserving key encoding |
| `backend/storage/value_codec.{h,cc}` | Value encoding |
| `backend/storage/in_memory_iterator.h` | `FixedRowStorageIterator` for buffered results |
| `backend/storage/persistent_storage_test.cc` | Storage tests |
| `backend/database/database.{h,cc}` | `PersistentStorageDirectory`, `CreateBackupCheckpoint`, `UpdateSchemaWithRollbackCheckpoint` |
| `backend/transaction/flush.cc` | Writes a commit's buffered operations to storage |
| `frontend/persistence/atomic_file.{h,cc}` | Atomic, no-follow file writes |
| `frontend/persistence/metadata_store.{h,cc}` | `metadata.json` |
| `frontend/persistence/backup_catalog.{h,cc}` | `backup_catalog.json` and snapshot folders |
| `frontend/persistence/schema_change_batch.h` | `PersistedSchemaChangeBatch` |
| `frontend/persistence/*_test.cc` | Metadata and catalog tests |
| `frontend/collections/database_manager.{h,cc}` | Markers, legacy migration, orphan cleanup, DDL rollback, unavailable databases |
| `frontend/handlers/databases.cc`, `backups.cc` | Admin RPCs that persist state |
| `binaries/emulator_main.cc` | `RestoreFromMetadata`, `QuarantineCorruptedDatabase` |
| `common/config.{h,cc}` | `--data_dir`, `--repair_corrupted_databases` |

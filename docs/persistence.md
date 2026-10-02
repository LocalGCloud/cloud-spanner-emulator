# Persistence

By default the emulator keeps everything in memory and loses it when the
process exits. Set `--data_dir` to a directory and the emulator stores its
state there and loads it again at the next start.

This guide covers how to turn persistence on, what is saved, how the directory
is laid out, how startup and recovery work, and the known limitations. For the
storage internals, see [Persistent storage internals](internals/persistent-storage.md).

## Enabling persistence

Use an absolute path. The directory is created on first write if it doesn't
exist.

### Docker

The image has no entrypoint. Its default command is
`./gateway_main --hostname 0.0.0.0`, and anything you put after the image name
replaces that command. Repeat the command when you add flags:

```shell
docker run -p 9010:9010 -p 9020:9020 \
  -v /path/to/data:/data \
  agentcloud/localcloud-spanner-emulator \
  ./gateway_main --hostname 0.0.0.0 --data_dir=/data
```

`docker run ... agentcloud/localcloud-spanner-emulator --data_dir=/data`
doesn't work: Docker tries to run `--data_dir=/data` as the program.

Named Docker volumes (e.g. `-v spanner-vol:/data`) or host directory mounts
survive container restarts (`docker restart`) and recreation. This was qualified
in `tests/image_verification_test.py` across both GoogleSQL and PostgreSQL
dialects: schema objects, database roles, DEFINER views, database options, TTL
policies, sequences, unique indexes, split points, `version_time` backups, and
`SPANNER_SYS` statistics all survive container restarts.

### Binaries

`gateway_main` serves REST on port 9020 and starts `emulator_main` as a child
process for gRPC on port 9010. It passes `--data_dir` through:

```shell
gateway_main --data_dir=/path/to/data
# or, from the source root:
bazel run binaries/gateway_main -- --data_dir=/path/to/data
```

`emulator_main` serves gRPC only. Its default address is `localhost:10007`:

```shell
emulator_main --host_port=localhost:9010 --data_dir=/path/to/data
```

### Flags

| Flag | `emulator_main` | `gateway_main` and Docker | Effect |
|------|-----------------|---------------------------|--------|
| `--data_dir` | Yes | Yes, passed to `emulator_main` | Directory for persistent state. Empty (the default) means in-memory mode. |
| `--repair_corrupted_databases` | Yes | Yes, passed to `emulator_main` | Quarantines databases that fail to restore. See [Quarantine](#quarantine). |

## What persists

| State | What is saved | Stored in |
|-------|---------------|-----------|
| Rows | Committed row versions still present after pruning, with their commit timestamps (microsecond precision) | Per-database LevelDB directory |
| Instances | Name, display name, config, node count or processing units, labels, create and update times | `metadata.json` |
| Custom instance configs | The full config | `metadata.json` |
| Instance partitions | The full partition | `metadata.json` |
| Databases | Dialect, create time, drop protection, and every committed DDL batch (including roles, grants, database options, and row deletion policies) with its commit timestamp and proto descriptors | `metadata.json` |
| ID counters | Table, column, and change stream ID counters per database | `metadata.json` |
| Sequence counters | Each sequence's counter, including `IDENTITY` columns' sequences, saved up to 1,000 values ahead | The database's LevelDB directory (so backups carry it too) |
| Split points | Split points added with `AddSplitPoints`, with their expire times | The database's LevelDB directory |
| `SPANNER_SYS` statistics | Query, read, transaction, lock, and table and column operation statistics for every interval still in its retention period, table size samples, and each table's last row deletion policy sweep. Saved when a minute interval ends and when `emulator_main` gets `SIGTERM` or `SIGINT`; a crash or `SIGKILL` loses the current minute. Dropping the database deletes them. | `spanner_sys_statistics.json` in the database's folder |
| IAM policies | Policies on instances, databases, instance configs, instance partitions, backups, and backup schedules | `metadata.json` |
| Long-running operations | Operations from creating or updating instances, instance configs, instance partitions, and databases; database DDL; moving instances; and creating, copying, and restoring backups | `backup_catalog.json` |
| Backups | Backup metadata (including `version_time`, encryption information, and incremental chain fields), plus a full copy of the database's LevelDB data | `backup_catalog.json` and `backups/` |
| Backup schedules | The full schedule | `backup_catalog.json` |

UUID keys and values persist with `--data_dir`; a native gRPC process-restart
test covers PostgreSQL UUID rows and typed reads after restart.

At startup the emulator rebuilds each database's schema by replaying its DDL
batches in order, each at its original commit timestamp. The first batch (the
statements sent with `CreateDatabase`) runs at the database's create time,
which is saved as `createTime` and returned by `GetDatabase`. This keeps
time-based schema state such as change stream creation times.

IAM policies are stored and returned, but the emulator doesn't enforce them.

### What doesn't persist

- Sessions. Clients need new sessions after a restart.
- Open transactions. Anything not committed is lost.
- In-flight change stream queries. Start them again after a restart.
- Executing queries and partitioned DMLs, which `SPANNER_SYS` shows while
  they run.

The version retention period limits historical reads, but older row versions
may still be on disk. See [Space usage](#space-usage).

## On-disk layout

Treat the directory as opaque. The layout can change between versions, and
startup deletes some entries it doesn't recognize.

```text
<data_dir>/
  .lock                          held by the running emulator; holds its PID
  metadata.json                  instances, databases, DDL, IAM, configs, partitions
  backup_catalog.json            backups, backup schedules, operations
  projects/<project>/instances/<instance>/databases/<database>/
    storage/                     the database's LevelDB data
    spanner_sys_statistics.json  the database's SPANNER_SYS statistics
    .metadata-committed          marker: the database is recorded in metadata.json
    .restore-in-progress         marker: a RestoreDatabase hasn't finished
    .delete-in-progress          marker: a DropDatabase hasn't finished
    .ddl-rollback/<op>/storage/  copy of the data taken before a DDL change
  backups/<encoded backup name>/storage/   a backup's full LevelDB copy
  .quarantine/<database>-<micros>/         a database folder moved aside by --repair_corrupted_databases,
                                           or by dropping an unavailable database
  .database-migrations/          staging area for the legacy layout migration
```

Temporary names ending in `.tmp`, `.restoring`, or `.deleting` can appear
while an operation runs.

Rules:

- **Stop the emulator before copying or backing up the directory.** The JSON
  files and the LevelDB directories are separate files, and a copy taken while
  the emulator runs may not be consistent.
- **Only one process may use a directory.** `emulator_main` takes an
  exclusive `flock` on `<data_dir>/.lock` before it reads anything, and a
  second emulator on the same directory exits at startup with
  `--data_dir <dir> is in use by another emulator process (see the PID in
  <dir>/.lock)`. The operating system releases the lock when the process
  exits, even after a crash, so a stale `.lock` file is harmless. The lock
  doesn't stop other programs from writing to the directory.
  `gateway_main` stops its `emulator_main` when it gets `SIGINT` or
  `SIGTERM`; if you kill `gateway_main` with `SIGKILL`, stop `emulator_main`
  yourself.
- **To reset, delete the whole directory.** Deleting only `metadata.json`
  leaves database directories with no metadata, and startup then fails with
  `Persistent database root has committed data but no metadata`.
- **Don't edit files by hand, and don't add symbolic links inside the
  directory.** Startup rejects symbolic links in the directory tree. The
  directory itself may be a symbolic link or a mount.
- **Don't store your own files under `backups/` or
  `projects/.../databases/`.** Startup removes folders under `backups/` that
  aren't in the backup catalog. It also removes database folders that have no
  `.metadata-committed` marker and no metadata entry, because it treats them
  as unfinished creates or restores.

## Durability

- `metadata.json` and `backup_catalog.json` are replaced atomically. Each is
  written to a `.tmp` file, synced to disk, and renamed into place, and then
  the directory is synced.
- Row data is written to LevelDB without a sync. Committed rows survive a
  killed process, such as `docker stop` or `SIGKILL`. The most recent writes
  can be lost if the operating system crashes or the machine loses power.
- Backups and DDL rollback copies are written with a sync.
- Each commit is written with one LevelDB write: its rows, index entries and
  change stream records reach disk together or not at all, so a crash never
  leaves part of a transaction behind. (Before 2026-09-24 each row was a
  separate write.)

## Backups

Backups are full local copies. They need `--data_dir`:

- Without `--data_dir`, `CreateBackup` fails with `FAILED_PRECONDITION`
  (`Native backups require emulator --data_dir persistent storage`), and
  `RestoreDatabase` fails with `Native restore requires persistent metadata
  storage`. Backup schedule metadata RPCs still work, but the schedules aren't
  saved and cannot create backups without `--data_dir`.
- `CreateBackup` copies the database's LevelDB data. Without `version_time`
  it captures the database when the request runs, including old row versions
  still inside the retention period. The backup is `READY`, and its operation
  is done, when the call returns.
- The backup must be in the same instance as its source database.
- `version_time` can be any time from the database's `earliest_version_time`
  (bounded by `version_retention_period`) to now. The backup then keeps only
  row versions at or before that time and the schema as of that time, and
  `GetBackup` reports it as `version_time`.
- `encryption_config` accepts the Google-default types, and `encryption_info`
  reports Google default encryption. Customer-managed keys fail with
  `UNIMPLEMENTED`: local data is plaintext.
- `expire_time` is required for manual backups and must be between 6 hours
  and 366 days after the backup's create time. Expired backups are deleted by backup metadata
  RPCs and the periodic schedule worker.
- `UpdateBackup` can change only `expire_time`.
- `CopyBackup` makes another full copy. The source must be `READY`.
- `RestoreDatabase` copies the snapshot into a new database. The target must
  be in the same project as the backup, and the target instance must use the
  same instance config as the backup's source instance. Roles and grants are
  restored; row deletion policies are dropped, as in production.
- Backup schedules run at supported 12-hour, daily, weekly, or monthly UTC
  times. Each due run persists its backup, completed operation, and next
  cursor together; the worker resumes after restart. Incremental schedules
  link their backups into a chain (`incremental_backup_chain_id`,
  `oldest_version_time`), but each backup is a full copy. A database can
  have at most 4 schedules.
- `DropDatabase` fails with `Database still has backup schedules` until the
  database's schedules are deleted.
- `DeleteBackup` removes the backup's data from `backups/`.

## Startup and recovery

With `--data_dir`, `emulator_main` loads and checks all persisted state before
it opens its gRPC port. `gateway_main` opens the REST port only after the gRPC
server answers. A large directory can take a while to load: the emulator
replays each database's DDL and opens each backup to check it.

If startup fails, `emulator_main` logs
`Failed to restore persisted state: <error>` and exits with status 1.
`gateway_main` then exits with the same status, which stops the container.

### Interrupted operations

The emulator records enough state to finish or undo an operation that a crash
interrupted:

| Interrupted operation | What happens at the next startup |
|-----------------------|----------------------------------|
| `CreateDatabase` or `RestoreDatabase` before its metadata was saved | Its partial directory is removed. The database doesn't exist. |
| `RestoreDatabase` after its metadata was saved | The restore is completed. |
| `DropDatabase` | Completed if the metadata entry was already removed, otherwise undone. |
| `UpdateDatabaseDdl` | The data is rolled back to the copy taken before the change, and the DDL statements are applied again. The operation completes during startup. |
| `DeleteBackup` or `DeleteBackupSchedule` | Completed, if the deletion was recorded before the crash. Otherwise nothing changed. |

Every `UpdateDatabaseDdl` first copies the database's full LevelDB data to
`.ddl-rollback/`, and removes the copy when the change is committed. A DDL
change therefore takes time and temporary disk space in proportion to the
database's size.

If saving metadata fails after a DDL change is applied, the database rejects
requests with `Database recovery is required before serving <database>` until
the emulator restarts and recovers it.

### Unavailable databases

A database that fails to restore doesn't stop the emulator. The emulator logs
`Failed to restore database <database>; it will be unavailable for this run,
...` at `ERROR` level and starts everything else. For that run the database is
`UNAVAILABLE`:

- `ListDatabases` and `GetDatabase` return it with only its name and state
  `CREATING`. `Database.State` has no failed value, and `CREATING` already
  allows `FAILED_PRECONDITION` on operations.
- Most operations on it fail with `FAILED_PRECONDITION`: `Database <database>
  is unavailable: it failed to restore from persisted metadata (reason:
  <reason>). ...` These include creating sessions (so reads, writes, and
  queries), DDL, and backups.
- Its data and metadata stay on disk so you can inspect them. It is retried at
  every startup.
- It keeps its IAM policies. `GetIamPolicy`, `SetIamPolicy` and
  `TestIamPermissions` work on it as on any other database.
- Its name stays taken: `CreateDatabase` and `RestoreDatabase` with that name
  fail with `ALREADY_EXISTS`.
- `DropDatabase` removes it at once. Its folder is moved to
  `<data_dir>/.quarantine/` (as with [Quarantine](#quarantine)) rather than
  deleted, and the emulator logs `Dropped unavailable database <database>;
  its data was moved to <path>` at `WARNING` level. Drop protection saved in
  `metadata.json` still applies. The name can then be reused right away.

Common reasons are a missing or unreadable `storage/` directory
(`Persisted database storage is missing or unreadable for <database>`) and a
failed schema replay or data check (`Failed to restore database <database>:
<reason>`).

### IAM policies at startup

IAM policies are restored after every instance, database, instance partition,
custom instance config, backup and backup schedule:

- A policy on an unavailable database is restored like any other.
- A policy whose resource no longer exists (for example a hand-edited
  `metadata.json`, or a resource removed while the emulator was stopped) is
  dropped. The emulator logs `Dropping the persisted IAM policy for
  <resource> because the resource no longer exists` at `WARNING` level,
  starts normally, and removes the policy from `metadata.json` at the next
  save.
- A policy whose resource name is malformed stops the emulator (see
  [Startup errors](#startup-errors-that-stop-the-emulator)).

### Quarantine

`--repair_corrupted_databases` removes each database that fails to restore:

1. Its whole folder (`storage/` and its marker files) is moved to
   `<data_dir>/.quarantine/<database URI with / replaced by _>-<unix micros>`
   in one rename.
2. Its `metadata.json` entry and its IAM policies are removed.
3. It doesn't appear in the API for that run or later ones.

The emulator logs `Quarantined corrupted database <database>` at `WARNING`
level. Quarantined data is never deleted automatically. Both `gateway_main`
and `emulator_main` accept the flag. It only needs to be set for one start;
later starts don't need it:

```shell
docker run --rm -p 9010:9010 -p 9020:9020 -v /path/to/data:/data \
  agentcloud/localcloud-spanner-emulator \
  ./gateway_main --hostname 0.0.0.0 --data_dir=/data \
  --repair_corrupted_databases
```

If the emulator stops after moving the folder but before saving
`metadata.json`, the next start lists the database as unavailable (its storage
is missing), and another start with the flag removes it.

### Startup errors that stop the emulator

Only failures inside a single database's restore are isolated. These stop the
whole emulator:

| Cause | Error message starts with |
|-------|---------------------------|
| Malformed or unreadable `metadata.json` | `Malformed metadata file`, `Incomplete version 4` |
| `metadata.json` written by a newer version | `Unsupported metadata version` |
| Malformed `backup_catalog.json` | `Invalid backup catalog`, `Backup catalog` |
| `backup_catalog.json` written by a newer version | `Unsupported backup catalog version` |
| A `READY` backup whose data is missing or can't be opened | `Backup snapshot is missing, unsafe, or unreadable`, `Backup snapshot is unreadable` |
| A database folder with a `.metadata-committed` marker but no metadata entry | `Persistent database root has committed data but no metadata` |
| A symbolic link inside the directory tree | `Persisted resource hierarchy contains a symbolic link`, `Persistent database path contains a symbolic link` |
| Old-layout data that matches databases in more than one instance | `Legacy database storage ... is ambiguous` |
| A recorded DDL change for a database that isn't in the metadata | `Pending DDL operation references missing database` |
| An instance, custom instance config, or instance partition that can't be restored | `Failed to restore instance`, `Failed to restore instance config`, `Failed to restore instance partition`, `Persisted instance partition is not READY` |
| An IAM policy whose resource name is malformed | `Persisted IAM policy references an invalid or missing resource` |
| An operation that can't be restored | `Failed to restore operation` |

## Upgrades and compatibility

- The current version writes `metadata.json` version 8 and
  `backup_catalog.json` version 4. It reads `metadata.json` versions 1 to 8 and
  `backup_catalog.json` versions 1 to 4.
- Older files are rewritten in the current format at the next save. After
  that, older emulator builds can't read the directory: they fail with
  `Unsupported metadata version`. Downgrades aren't supported. Copy the
  directory, with the emulator stopped, before upgrading if you might need to
  go back.
- Older builds of this fork stored each database at
  `<data_dir>/<database>/storage`. Startup moves these to
  `projects/<project>/instances/<instance>/databases/<database>/storage`
  automatically. If two instances have a database with the same ID, the move
  is ambiguous and startup fails.
- Older metadata files also saved sequence and named schema ID counters. These
  are ignored.
- DDL saved before per-batch timestamps existed is replayed at the database's
  create time (first batch) or at the restart time (later batches). A change
  stream created in a later batch gets the restart time as its creation time.
- Builds before 2026-09-24 saved every database's `createTime`, and its first
  DDL batch's timestamp, as `1970-01-01T00:00:00+00:00`. Those databases keep
  reporting 1970, and their change streams from `CreateDatabase` have a 1970
  creation time. Nothing repairs this; recreate the database (or the data
  directory) if you need the real time.
- Replayed DDL skips some checks that were added after it was first accepted
  (currently the geo-partitioning placement checks), so databases created by
  older builds keep loading.
- Builds before the 2026-09-28 range-scan fix could skip rows in range reads
  and range deletes, so unique index checks could miss existing values. The
  on-disk format didn't change, and directories written by those builds load
  as before, but nothing removes duplicate unique index keys they may already
  hold. A database whose restore fails on them is
  [unavailable](#unavailable-databases) until you drop and recreate it.

## Space usage

- Read timestamps at least `version_retention_period` old (default 1 hour)
  are rejected. After a write or delete, the emulator prunes
  expired versions of touched cells but keeps the newest version at or before
  the cutoff. Untouched cells can retain older versions on disk. Deleted rows
  leave a deletion record.
- Data of a dropped table is removed at the first read-only transaction read
  or schema change after the retention period has passed. Data of a dropped
  column is removed at the first schema change after that.
- Each backup is a full copy of its database. Backups are removed by
  `DeleteBackup` or, once expired, by backup metadata calls and the schedule
  worker.
- Each DDL change needs temporary space for a full copy of its database.
- `.quarantine/` is never cleaned up.

## Known limitations

- **A restart can skip sequence values.** A sequence saves its counter up to
  1,000 values ahead, so after a restart it continues from the saved counter
  and never repeats a value, but it can skip up to 1,000 counter values.
  `GET_INTERNAL_SEQUENCE_STATE` shows the jump. Cloud Spanner sequences can
  also skip values. Databases persisted before this was added (2026-09-24)
  have no saved counter; their sequences start over once, as before.
- **No sync on row writes.** An OS crash or power loss can lose the most
  recent commits, though never part of one. See [Durability](#durability).
- **Backups are full copies,** including incremental schedule backups.
  Customer-managed encryption isn't supported. Expired backups are removed by
  backup metadata calls and the schedule worker; due schedules run with a
  persisted cursor.
- **`SPANNER_SYS` statistics can lose the current minute.** They're saved
  when a minute interval ends and on `SIGTERM` or `SIGINT`, so a crash or
  `SIGKILL` loses the statistics recorded since the last save. Backups don't
  carry them.
- **Downgrades aren't supported** once a newer build has saved the directory.

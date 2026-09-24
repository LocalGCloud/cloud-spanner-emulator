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
  jaysen2apache/spanner-emulator-extended \
  ./gateway_main --hostname 0.0.0.0 --data_dir=/data
```

`docker run ... jaysen2apache/spanner-emulator-extended --data_dir=/data`
doesn't work: Docker tries to run `--data_dir=/data` as the program.

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
| Rows | Every committed row, with each version's commit timestamp (microsecond precision) | Per-database LevelDB directory |
| Instances | Name, display name, config, node count or processing units, labels, create and update times | `metadata.json` |
| Custom instance configs | The full config | `metadata.json` |
| Instance partitions | The full partition | `metadata.json` |
| Databases | Dialect, create time, drop protection, and every committed DDL batch with its commit timestamp and proto descriptors | `metadata.json` |
| ID counters | Table, column, and change stream ID counters per database | `metadata.json` |
| Sequence counters | Each sequence's counter, including `IDENTITY` columns' sequences, saved up to 1,000 values ahead | The database's LevelDB directory (so backups carry it too) |
| IAM policies | Policies on instances, databases, instance configs, instance partitions, backups, and backup schedules | `metadata.json` |
| Long-running operations | Operations from creating or updating instances, instance configs, instance partitions, and databases; database DDL; moving instances; and creating, copying, and restoring backups | `backup_catalog.json` |
| Backups | Backup metadata, plus a full copy of the database's LevelDB data | `backup_catalog.json` and `backups/` |
| Backup schedules | The full schedule | `backup_catalog.json` |

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
- Row versions older than the database's version retention period (default
  1 hour). See [Space usage](#space-usage).

## On-disk layout

Treat the directory as opaque. The layout can change between versions, and
startup deletes some entries it doesn't recognize.

```text
<data_dir>/
  metadata.json                  instances, databases, DDL, IAM, configs, partitions
  backup_catalog.json            backups, backup schedules, operations
  projects/<project>/instances/<instance>/databases/<database>/
    storage/                     the database's LevelDB data
    .metadata-committed          marker: the database is recorded in metadata.json
    .restore-in-progress         marker: a RestoreDatabase hasn't finished
    .delete-in-progress          marker: a DropDatabase hasn't finished
    .ddl-rollback/<op>/storage/  copy of the data taken before a DDL change
  backups/<encoded backup name>/storage/   a backup's full LevelDB copy
  .quarantine/<database>-<micros>/         data moved aside by --repair_corrupted_databases
  .database-migrations/          staging area for the legacy layout migration
```

Temporary names ending in `.tmp`, `.restoring`, or `.deleting` can appear
while an operation runs.

Rules:

- **Stop the emulator before copying or backing up the directory.** The JSON
  files and the LevelDB directories are separate files, and a copy taken while
  the emulator runs may not be consistent.
- **Only one process may use a directory.** Nothing locks the whole directory.
  A second process can't open databases the first has open, so it marks them
  `UNAVAILABLE`, and both processes write `metadata.json`. Make sure the
  previous `emulator_main` has exited before you start a new one.
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
- Each row write of a commit, index entries included, is a separate LevelDB
  write. If the process dies partway through a commit, only part of the
  transaction may be on disk. (From reading the code in
  `backend/transaction/flush.cc`, not reproduced.)

## Backups

Backups are full local copies. They need `--data_dir`:

- Without `--data_dir`, `CreateBackup` fails with `FAILED_PRECONDITION`
  (`Native backups require emulator --data_dir persistent storage`), and
  `RestoreDatabase` fails with `Native restore requires persistent metadata
  storage`. Backup schedules still work, but they aren't saved.
- `CreateBackup` copies all of the database's LevelDB data, including old row
  versions still inside the retention period. It captures the database when
  the request runs. The backup is `READY`, and its operation is done, when the
  call returns.
- The backup must be in the same instance as its source database.
- `version_time` isn't supported. Requests that set it fail with
  `Historical backup version_time is not supported by the emulator`.
- `expire_time` is required and must be between 6 hours and 366 days after the
  create time. The emulator only checks it: expired backups are never deleted.
- `UpdateBackup` can change only `expire_time`.
- `CopyBackup` makes another full copy. The source must be `READY`.
- `RestoreDatabase` copies the snapshot into a new database. The target must
  be in the same project as the backup, and the target instance must use the
  same instance config as the backup's source instance.
- Backup schedules are stored and returned but never run. No backups are
  created automatically.
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

Common reasons are a missing or unreadable `storage/` directory
(`Persisted database storage is missing or unreadable for <database>`) and a
failed schema replay or data check (`Failed to restore database <database>:
<reason>`).

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
  jaysen2apache/spanner-emulator-extended \
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
| An IAM policy whose resource is missing or unavailable | `Persisted IAM policy references an invalid or missing resource` |
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

## Space usage

- The emulator keeps old row versions for the database's
  `version_retention_period` (default 1 hour). Old versions of a row are pruned
  when that row is written again. Deleted rows leave a deletion record.
- Data of a dropped table is removed at the first read or schema change after
  the retention period has passed. Data of a dropped column is removed at the
  first schema change after that.
- Each backup is a full copy of its database. Backups are removed only by
  `DeleteBackup`.
- Each DDL change needs temporary space for a full copy of its database.
- `.quarantine/` is never cleaned up.

## Known limitations

- **A restart can skip sequence values.** A sequence saves its counter up to
  1,000 values ahead, so after a restart it continues from the saved counter
  and never repeats a value, but it can skip up to 1,000 counter values.
  `GET_INTERNAL_SEQUENCE_STATE` shows the jump. Cloud Spanner sequences can
  also skip values. Databases persisted before this was added (2026-09-24)
  have no saved counter; their sequences start over once, as before.
- **IAM policy on an unavailable database stops startup.** Restoring an IAM
  policy checks that its resource exists. An `UNAVAILABLE` database fails that
  check, so a database-level policy on it turns an isolated failure into
  `Persisted IAM policy references an invalid or missing resource`, and the
  emulator exits. (From reading the code, not reproduced.)
- **`DropDatabase` on an unavailable database.** It deletes the database's
  data and metadata permanently, with no quarantine copy. The database stays
  listed as `CREATING` until the next restart. A database created with the
  same name during that run can't be used until a restart. (From reading the
  code, not reproduced.)
- **No directory lock.** Two processes on one directory can corrupt
  `metadata.json` and make databases unavailable.
- **No sync on row writes, and commits span several LevelDB writes.** An OS
  crash can lose recent commits, and a crash mid-commit can leave a partial
  transaction. See [Durability](#durability).
- **Backups are full copies,** `version_time` isn't supported, expired backups
  are kept, and backup schedules never run.
- **Downgrades aren't supported** once a newer build has saved the directory.

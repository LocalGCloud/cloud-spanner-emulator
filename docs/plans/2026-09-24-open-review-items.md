# Task brief: fix the open items from the 2026-09-24 review

## Status

All seven tasks were fixed on 2026-09-24, one commit each on
`jay-spanner-extended` (not pushed). Every one was reproduced with a failing
test before the fix; details are in each commit message and in the
[changelog](../CHANGELOG.md).

| Task | Commit | How it was reproduced |
|------|--------|-----------------------|
| 1. Database create time | `1a76ccc4` | End to end (`INTERNAL` after a restart) and `databases_test` |
| 2. Restart after quarantine, gateway flag | `ae77daa6` | End to end (`DATA_LOSS` on the next start) and `database_manager_test` |
| 3. IAM policy on an unavailable database | `4a8a3e7a` | `policies_test` (`DATA_LOSS` from the restore loop) |
| 4. Dropping an unavailable database | `5956388f` | `databases_test` and `database_manager_test` |
| 5. Gateway signal handling | `5444353a` | End to end (`emulator_main` left running) and `gateway_test` |
| 6. Write-queue results | `e1681742` | `persistent_storage_test` (concurrent writers) |
| 7. Atomic commits | `835d8b87` | `flush_test`, and end to end with `SIGKILL` (5 of 12 crashes left a partial commit) |

Maintainer decisions: existing `metadata.json` files with a 1970 create time
aren't repaired (no backward compatibility for now); a persisted IAM policy
whose resource is missing is dropped with a warning; dropping an unavailable
database moves it to `.quarantine/`.

Found along the way and fixed afterwards: a REST field mask in the URL wasn't
converted from camelCase, so `UpdateDatabase`, `UpdateBackup` and
`UpdateBackupSchedule` rejected `?updateMask=enableDropProtection`-style
masks.

You are working in a fork of Google's Cloud Spanner Emulator (C++ with Bazel,
plus a Go REST gateway). A documentation review on 2026-09-24 found the bugs
and gaps below. Fix them one at a time, in the order given, with a test for
each and the docs updated in the same commit. Several were found by reading
the code and haven't been reproduced: reproduce each one with a failing test
before you change code, and if you can't reproduce it, say so and correct the
docs instead of "fixing" it.

## Context

- **Repository:** `/Users/jsenjaliya/src/AI/local_cloud_dependencies/cloud-spanner-emulator`,
  branch `jay-spanner-extended` (remote `origin` =
  `LocalGCloud/cloud-spanner-emulator`). `master` tracks upstream. **Don't
  push**; the maintainer pushes and publishes.
- **Who depends on it:** LocalCloud bundles the Docker image
  (`jaysen2apache/spanner-emulator-extended`, pinned by commit and digest) and
  runs it with `--data_dir`, so persistence and restart behavior matter most.
- **Where behavior is documented** (keep these accurate):
  - `docs/capabilities.md`: what works.
  - `docs/known-gaps.md`: what doesn't, including a "Known bugs" table. Each
    row is marked "Reproduced" or "From code".
  - `docs/persistence.md`: the `--data_dir` guide, including "Known
    limitations".
  - `docs/change-streams.md`: the change stream guide.
  - `docs/configuration.md`: every flag, and which ones `gateway_main`
    forwards.
  - `docs/feature-coverage.yaml`: per-feature status. Edit it with a script:
    it's JSON, and `json.load` / `json.dumps(indent=2) + "\n"` round-trips
    exactly. Then run `python3 tools/feature_coverage.py validate`,
    `generate` and `check`.
  - `docs/CHANGELOG.md`: add a dated entry for each fix.
  - `docs/internals/*.md`: implementation notes that cite source paths.

### Building and testing (native macOS, warm cache)

Use native Bazel with a persistent disk cache. Docker builds start cold and
take hours. Keep this exact environment, so cache hits stay stable:

```shell
PATH=/opt/homebrew/opt/gnu-sed/libexec/gnubin:/opt/homebrew/bin:/usr/bin:/bin:/usr/sbin:/sbin \
PROTOC=/opt/homebrew/opt/protobuf/bin/protoc CC=/usr/bin/clang CXX=/usr/bin/clang++ \
bazel --host_jvm_args=-Xmx6g test -c opt --strip=always \
  --macos_sdk_version=$(xcrun --sdk macosx --show-sdk-version) \
  --macos_minimum_os=12.0 --host_macos_minimum_os=12.0 \
  --spawn_strategy=standalone --jobs=8 '--local_resources=memory=HOST_RAM*.70' \
  --disk_cache=$HOME/.cache/bazel-disk/cloud-spanner-emulator \
  --test_output=errors <targets>
```

- Use `build` instead of `test` for `//binaries:emulator_main` and
  `//binaries:gateway_main`; outputs land in `bazel-bin/binaries/`.
- Conformance tests are one sharded target:
  `//tests/conformance/endpoints:emulator_conformance_test`. Narrow it with
  `--test_filter='ChangeStream*:*Sequence*'` and similar.
- Relevant unit targets:
  - `//frontend/collections:database_manager_test`
  - `//frontend/handlers:databases_test`
  - `//frontend/handlers:policies_test`
  - `//backend/database:database_test`
  - `//backend/storage:persistent_storage_test`
  - `//frontend/persistence:metadata_store_test`
  - `//backend/transaction:read_write_transaction_test`
- `bazel query 'rdeps(//..., X)'` fails fetching `libpfm`. Scope it instead:
  `bazel query --keep_going 'rdeps(//backend/... + //frontend/... + //tests/..., X)'`.
- Known pre-existing failures: `third_party/spanner_pg`
  `serializer_deserializer_test` and `parser_test` don't compile. Ignore them.

### End-to-end checks

Run the real binaries with a data directory and talk REST (port 9020 by
default):

```shell
bazel-bin/binaries/gateway_main_/gateway_main --grpc_binary=bazel-bin/binaries/emulator_main \
  --grpc_port=19010 --http_port=19020 --data_dir=/tmp/spanner-e2e
```

Stop it (kill both processes), start it again on the same directory to test a
restart, and check the behavior over REST. Useful calls:

- `POST /v1/projects/p/instances` with
  `{"instanceId":"inst","instance":{"config":"projects/p/instanceConfigs/emulator-config","displayName":"inst","nodeCount":1}}`.
- `POST /v1/projects/p/instances/inst/databases` with `createStatement` and
  `extraStatements`.
- `PATCH /v1/.../databases/db/ddl` with `statements`.
- `POST /v1/.../databases/db/sessions`, then `POST /v1/<session>:executeSql`.

### Shell quirks and conventions

- The shell wraps commands with `rtk`, which rewrites `git diff`, `grep` and
  `git log` output. Use `rtk proxy git diff ...` / `rtk proxy grep ...` when
  you need raw output. `rm` is interactive; use `/bin/rm -f`.
- Commit each task separately, with a detailed message covering the problem,
  root cause, fix, tests and docs. **Never add `Co-Authored-By` trailers.**
- Match the surrounding code's style and comment density. New files use the
  repo's header (`Copyright 2026 Google LLC`, Apache 2.0) and repo-relative
  include paths.
- Before writing any doc claim, verify it against the code or a running
  emulator.

## Tasks, in priority order

### 1. Database create time isn't recorded (reproduced)

**Symptom**
- After a restart with `--data_dir`, change streams created in the
  `CreateDatabase` request (its `extraStatements`) get a creation time of
  1970-01-01.
- A change stream query whose `start_timestamp` is before the stream's real
  creation time then fails with `INTERNAL` (a `RET_CHECK ...
  !IsQueryResultEmpty` failure) instead of `OUT_OF_RANGE`.
- `GetDatabase` returns no `create_time` at all.
- Change streams created later with `UpdateDatabaseDdl` are fine.

**Evidence**
- `frontend/entities/database.cc:28-34`: `Database::ToProto` sets name, state,
  dialect and drop protection, but never `create_time`.
- The persisted database `createTime` and the first DDL batch's
  `schemaChangeTimestamp` in `metadata.json` come out as
  `1970-01-01T00:00:00Z`. Trace where `metadata.json` gets the database
  create time, starting from the `CreateDatabase` handler in
  `frontend/handlers/databases.cc` and `frontend/persistence/metadata_store.cc`.
- `frontend/collections/database_manager.cc:900-924`: replay uses the
  persisted `create_time` as the initial DDL batch's timestamp. This came from
  commit `239e9178`.
- `backend/database/database.cc` (around lines 180-190): live creation uses
  `clock->Now()` as the schema change timestamp. The live timestamp and the
  persisted one therefore differ.

**Reproduce**
1. Create a database with
   `extraStatements: ["CREATE TABLE T (K INT64) PRIMARY KEY (K)", "CREATE CHANGE STREAM CS FOR T"]`.
2. Restart the emulator.
3. Query `READ_CS` with a `start_timestamp` a few seconds before the database
   was created. It returns `INTERNAL`; `OUT_OF_RANGE` is expected.
4. Look at `metadata.json` and at `GetDatabase` output.

**Expected**
- The database's real create time is kept in memory, persisted and returned
  by `GetDatabase`.
- The initial DDL batch replays at the same timestamp the live creation used,
  so change stream creation times match before and after a restart.
- An early `start_timestamp` returns `OUT_OF_RANGE`.

**Fix direction**
- Record one create timestamp at database creation.
- Use it both as the initial schema change timestamp and as the persisted
  `createTime`, so the two can't drift apart.
- Fill `create_time` in `Database::ToProto`.
- Existing `metadata.json` files already hold 1970. Decide how to treat them
  and document the decision. One option: keep 1970 for existing databases,
  since the saved state must stay replayable. Another: repair it on load.
- Also consider whether the `INTERNAL` error should become `OUT_OF_RANGE`
  independently, since it's a validation gap in its own right.

**Tests**
- A `database_manager_test` or `database_test` case: a database created with
  a change stream in the initial statements keeps its creation time across a
  simulated restart.
- A `databases_test` case: `GetDatabase` returns `create_time`.

**Docs**
- `docs/known-gaps.md`: the Known bugs row, the change streams bullets, and
  the `GetDatabase` bullet.
- `docs/change-streams.md`: "Restarts and persistence" and "Differences from
  production".
- `docs/persistence.md`: the "What persists" paragraph about replaying the
  first batch at 1970.
- `docs/capabilities.md`: the change streams paragraph.
- The changelog.

### 2. Restart after quarantine fails, and the repair flag can't be reached (from code)

**Symptom (suspected)**
- `emulator_main --repair_corrupted_databases` moves a database that failed to
  restore under `<data_dir>/.quarantine/`. It seems to move only the
  `storage/` directory and leave the `.metadata-committed` marker in the
  database folder.
- The next startup then stops with "Persistent database root has committed
  data but no metadata".

**Evidence**
- `binaries/emulator_main.cc:89-130`: `QuarantineCorruptedDatabase`.
- `frontend/collections/database_manager.cc:352-354` and `:614`: the marker
  files.
- `frontend/collections/database_manager.cc:656-662`: the fatal check.

**Also**
- `--repair_corrupted_databases` is defined only for `emulator_main`
  (`common/config.cc:51`).
- `gateway_main` (`binaries/gateway_main.go:33-75`) neither defines nor
  forwards it (`gateway/gateway.go:72-97`, the `emulatorArgs` list). The Docker
  image runs `gateway_main`, so the flag can't be used through the gateway or
  the image's default command.

**Expected**
- After a quarantine, the next start succeeds, and the database no longer
  exists.
- `gateway_main --repair_corrupted_databases` works and is forwarded to
  `emulator_main`.

**Fix direction**
- Quarantine the whole database folder, including the markers, or remove the
  leftover marker and empty folder atomically enough to survive a crash in the
  middle.
- Add the flag to `gateway_main.go` and `gateway/gateway.go` (field in
  `Options`, pass-through), the same way `--enforce_placement_dml_restrictions`
  is forwarded.

**Tests**
- A `database_manager_test` or emulator restore test: fail a restore,
  quarantine it, then restart and check that startup succeeds.
- A gateway test if a Go test harness exists; otherwise an end-to-end check.

**Docs**
- `docs/configuration.md`: move the flag into the `gateway_main` table.
- `docs/persistence.md`: the "Quarantine" section and "Known limitations".
- `docs/known-gaps.md`: the Known bugs row and the "Running the emulator"
  bullet.

### 3. An IAM policy on an unavailable database stops startup (from code)

**Symptom (suspected)**
- If a database fails to restore, it's marked `UNAVAILABLE`. If that database
  also has an IAM policy, restoring the policy fails validation and startup
  exits.
- The failure is `Persisted IAM policy references an invalid or missing
  resource`, which becomes a fatal `DATA_LOSS`.
- This defeats per-database fault isolation.

**Evidence**
- `binaries/emulator_main.cc:668-675`: the IAM restore loop calls
  `env->ValidateIamResource`.
- `frontend/server/environment.h` (around lines 86-99): `ValidateIamResource`.
- `frontend/collections/database_manager.cc` (around lines 1043-1051):
  `GetDatabase` returns `FAILED_PRECONDITION` for unavailable databases.

**Expected**
- A policy that refers to an unavailable database is restored, or skipped
  with a warning.
- Startup continues, and the other databases work.

**Fix direction**
- Treat unavailable databases as existing resources for IAM validation, or
  handle policies whose resource is unavailable separately.
- Keep policies for truly missing resources as they are now, whether fatal or
  warned. Decide which, and document it.

**Tests**
- A restore test: corrupt one database that has a policy, then check that
  startup succeeds and the other databases are served.

**Docs**
- `docs/known-gaps.md`: the Known bugs row.
- `docs/persistence.md`: "Known limitations" and the table of startup errors
  that stop the emulator.

### 4. Dropping an unavailable database (from code)

**Symptom (suspected)**
- `DropDatabase` on an `UNAVAILABLE` database deletes its data permanently,
  with no quarantine copy.
- The database stays listed as `CREATING` until restart, because
  `unavailable_databases_` is never cleared.
- Recreating a database with the same name in the same run gives an unusable
  database.

**Evidence**
- `frontend/collections/database_manager.cc:1066`: the only place
  `unavailable_databases_` is written. There's no erase.
- The `DropDatabase` path in `frontend/handlers/databases.cc`.

**Expected**
- Dropping an unavailable database removes it from listings immediately, and
  a new database with that name works.
- Decide whether to keep a quarantine copy first. That's the safer choice for
  data the operator may want to inspect. Document the decision.

**Tests**
- A `database_manager_test` / `databases_test` case: mark a database
  unavailable, drop it, check it isn't listed, then create it again and use
  it.

**Docs**
- `docs/known-gaps.md`: the Known bugs row.
- `docs/persistence.md`: "Unavailable databases" and "Known limitations".

### 5. The gateway doesn't stop the emulator on Ctrl-C (from code)

**Symptom (suspected)**
- On SIGINT, `gateway_main` calls `cmd.Process.Release()` and then
  `cmd.Process.Kill()` (`gateway/gateway.go:121-122`).
- After `Release`, `Kill` likely fails. `emulator_main` can then keep running
  with its databases open, and a quick restart finds them locked and marks
  them unavailable.
- Docker is mostly unaffected, because the container stops.

**Expected**
- `gateway_main` stops `emulator_main` on SIGINT and SIGTERM before exiting.

**Fix direction**
- Kill, or better send SIGTERM and then wait with a timeout, before
  `Release`/exit.
- Also handle `syscall.SIGTERM`, which is what `docker stop` and process
  managers send.

**Tests**
- A Go test if practical. Otherwise an end-to-end check: start the gateway,
  send SIGINT, confirm no `emulator_main` process remains, and restart on the
  same `--data_dir` successfully.

**Docs**
- `docs/known-gaps.md`: the Known bugs row.
- `docs/configuration.md`: a note on signals, if useful.

### 6. Write-queue results aren't matched to their writers (from code)

**Symptom (suspected)**
- The persistent storage write queue returns results through one shared FIFO
  (`results_`, `backend/storage/persistent_storage.cc:365-412`).
- A caller can take another batch's status, and return before its own batch
  is written.
- Impact is low today, because the transaction lock serializes most writers.

**Expected**
- Each `Write`/`Delete` call gets its own batch's result, after its batch is
  applied.

**Fix direction**
- Give each queued batch its own promise/future, or a result slot plus a
  condition variable.
- Keep the queue's ordering guarantees.
- `docs/internals/persistent-storage.md` ("Write queue", "Result matching")
  describes the current design.

**Tests**
- A `persistent_storage_test` case with concurrent writers, where one batch
  fails, checking that the error reaches the right caller and that each
  caller sees its data after returning.

**Docs**
- `docs/internals/persistent-storage.md`.
- `docs/known-gaps.md`: the Known bugs row.

### 7. Commits aren't atomic on disk (from code, larger)

**Symptom (suspected)**
- A commit's row and index writes reach LevelDB as separate writes.
- If the process crashes partway through, part of a transaction, or an index
  entry without its row, can be on disk.
- On the next start, that can make the database fail its restore checks (for
  example unique-index verification) and become unavailable.

**Evidence**
- `backend/transaction/read_write_transaction.cc` (`Commit` →
  `FlushWriteOpsToStorage`).
- `backend/storage/persistent_storage.cc` (the `Write` path).

**Expected**
- A commit becomes durable all-or-nothing.

**Fix direction**
- Let storage accept one `leveldb::WriteBatch` per commit, for example a
  batched write API on `Storage`, implemented by `PersistentStorage` and
  `InMemoryStorage`, used by the transaction's flush.
- Keep MVCC timestamps and the write queue's serialization intact.
- Keep `sync=false`, as today; atomicity is the goal, not fsync durability.
- This task is bigger than the others. Propose the design in
  `docs/internals/persistent-storage.md` first, then implement it.

**Tests**
- Inject a failure partway through a multi-row commit and check that
  storage has none of it.

**Docs**
- `docs/internals/persistent-storage.md`.
- `docs/persistence.md` ("Durability").
- `docs/known-gaps.md`.

### Not in scope unless the maintainer asks

- **Upstream sync.** The fork hasn't merged upstream's Copybara imports
  `3cdfbf3d` (2026-09-03) and `ae118797` (2026-09-14), which include queue
  schema objects and `ALTER DATABASE ... score_version`. Merging is a separate
  task with its own conflict resolution.
- **Docker `ENTRYPOINT`.** The image has only `CMD ["./gateway_main",
  "--hostname", "0.0.0.0"]`. Switching to an `ENTRYPOINT` would change how
  callers such as LocalCloud pass commands, so it's a product decision.
- **GitHub issues.** Issues are disabled on `LocalGCloud/cloud-spanner-emulator`.

## Definition of done, for each task

1. A test that fails before the fix and passes after it. For a "from code"
   item you can't reproduce, write down the evidence instead and update the
   docs to say it isn't a bug.
2. The relevant unit tests and conformance filters pass on native macOS.
3. For persistence and restart items, an end-to-end check with `--data_dir`
   and a restart.
4. The docs listed for the task are updated. The Known bugs row is removed, or
   changed to describe what's left. `feature-coverage.yaml` is updated where
   statuses or notes change, and the `tools/feature_coverage.py` checks pass.
5. A `docs/CHANGELOG.md` entry.
6. One commit per task with a detailed message and no `Co-Authored-By`
   trailer. Don't push.
7. A short final report: for each task, whether it was reproduced, the fix,
   the tests, and anything left open.

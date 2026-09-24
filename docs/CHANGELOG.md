# Changelog

Changes in this fork (`jay-spanner-extended`), newest first. Upstream emulator
releases are merged separately; the last one merged is the 2026-08-03 import.
Upstream's 2026-09-03 and 2026-09-14 imports aren't merged yet.

## [2026-09-24] Database Create Times Are Recorded

### Fixed
- With `--data_dir`, change streams created in the `CreateDatabase` request
  got a creation time of 1970-01-01 after a restart, and `GetDatabase` never
  returned `create_time`. `Database::ToProto` didn't set `create_time`, so the
  `CreateDatabase` handler saved the proto default (1970) as the database's
  `createTime` and as its first DDL batch's timestamp, which startup replays.
  A `start_timestamp` before the stream's real creation then passed
  validation, found no partition, and failed with `INTERNAL`
  (`RET_CHECK ... !IsQueryResultEmpty`). `ToProto` now returns the database's
  create time, the same time its initial schema uses, so `GetDatabase` and
  `ListDatabases` report it, `metadata.json` saves it, and change stream
  creation times match before and after a restart. `RestoreDatabase` now also
  uses the restore time for the restored database's first batch.
- The change stream initial query returns `OUT_OF_RANGE` instead of
  `INTERNAL` when no partition covers `start_timestamp`.
- Data directories written before this change keep their 1970 create times;
  nothing repairs them. Recreate those databases if you need the real time.
- Tests: `PersistentDatabaseDdlTest.CreateTimeIsReportedPersistedAndReplayedForInitialStatements`
  in `frontend/handlers:databases_test` (fails without the fix: no
  `create_time`). Checked end to end over REST with a restart: the previous
  build returned `INTERNAL` for an early `start_timestamp` after the restart;
  this build returns `OUT_OF_RANGE`, and `createTime` matches before and
  after. A data directory from the previous build returns `OUT_OF_RANGE` too.

## [2026-09-24] Sequences Continue After Restarts

### Fixed
- With `--data_dir`, a sequence started over after a restart and returned
  values it had already returned, so inserts into tables keyed by
  `GET_NEXT_SEQUENCE_VALUE` or an `IDENTITY` column failed with
  `ALREADY_EXISTS`. Sequence positions were kept only in memory, keyed by a
  random ID that changes when the DDL is replayed. Each database now keeps its
  sequences' counters in its own storage (`backend/storage/sequence_state_store.cc`),
  saved up to 1,000 values ahead, so a sequence continues after a restart or a
  backup restore and a restart can skip up to 1,000 counter values. Live
  `CREATE`, `DROP` and start-counter `ALTER` statements reset the saved
  counter; replayed DDL doesn't. Databases persisted before this change start
  over once, as before, because nothing was saved for them.
- Tests: `backend/storage:sequence_state_store_test`, a restart simulation in
  `backend/query:query_engine_test`, and the live-versus-replay rules in
  `backend/schema/updater:schema_updater_test`. Checked end to end over REST
  in both dialects: inserts after a restart, into a restored backup, and after
  a second restart all succeed with distinct keys (the previous build failed
  every insert after a restart).

## [2026-09-24] Documentation Review

### Changed
- Docs reorganized around [capabilities](capabilities.md) and
  [known gaps](known-gaps.md), with new guides for
  [configuration](configuration.md), [persistence](persistence.md) and
  [placements](placements.md), and a rewritten
  [change streams guide](change-streams.md). Design notes moved to
  `docs/internals/`, dated plans to `docs/plans/`. The README is now a short
  landing page.
- `docs/feature-coverage.yaml` re-audited: specific notes, corrected statuses
  and evidence, and new records for fork features.
- `docker-publish.yml` log messages now state the cache limits the workflow
  actually uses (5 GiB), not the pre-2026-08-26 ones.

### Corrections to earlier docs
- Only the `table_id`, `column_id` and `change_stream_id` counters persist. The
  2026-04-17 entry and the README also listed `sequence_id` and
  `named_schema_id`.
- `TOKENIZE_FULLTEXT` accepts `remove_diacritics` but ignores it (2026-05-08
  entry said it enables diacritic-insensitive indexing).
- Passing flags to the Docker image requires the full command
  (`./gateway_main --hostname 0.0.0.0 --data_dir=/data`); the old examples
  appended flags only and failed to start.
- `--repair_corrupted_databases` is an `emulator_main` flag; `gateway_main`
  doesn't accept or forward it.
- Change stream creation times survive restarts only for streams created by
  `UpdateDatabaseDdl`. Streams created in the `CreateDatabase` request get a
  1970 creation time after a restart (known bug; the 2026-08-16 entry and the
  README said creation times always survive).
- Sequence and `IDENTITY` positions didn't persist, so sequences reused values
  after a restart (fixed in the entry above).
- The change stream docs described queries, limits and internals that don't
  match the code (for example, a `NULL` partition token returns only child
  partition records, heartbeats allow 100–300000 ms, and start times can be at
  most 10 minutes ahead).

## [2026-09-23] REST Error Codes, Large JSONB Numbers on macOS, Test Fix

### Fixed
- **REST errors keep their status code**: constraint violations, duplicate
  keys, partitioned DML errors, and query errors such as `Table not found`
  used to reach REST clients as HTTP 500, code 13,
  `failed to marshal error message`. `ToRpcStatus` in
  `frontend/common/status.cc` now forwards only standard `google.rpc` error
  details. It drops internal markers (`google.spanner.ConstraintError`,
  `googlesql.ErrorMessageModeForPayload`) that the gateway can't encode.
  REST clients now get the real code and message, such as HTTP 409
  `ALREADY_EXISTS`. Tests: `frontend/common/status_test.cc`.
- **JSONB numbers above about 1e308 on Apple silicon**: native macOS builds
  rejected them with `number overflow` because `long double` is 8 bytes
  there. `jsonb_value.cc` now caps an overflowing conversion at the largest
  finite value, since the parser uses only the number's text. The 4,932-digit
  limit applies on every platform, and larger numbers get the same
  `whole component of NUMERIC ... too large` error everywhere. Tests: new
  `jsonb_parse_test` cases. `PGFunctionsTest.ToJsonB` passes on macOS.
- **`ChangeStreamQueryValidatorTest.ValidateStartTimestampTooOldBeforeRetentionNonValid`**:
  the `CreateSchemaFromDDL` test helper now sets `schema_change_timestamp`.
  Change streams take their creation time from it, and it defaulted to 1970.
  Tests only; the database create and update paths already set it.

### Changed
- **Branch renamed**: `jay-33-persistence` is now `jay-spanner-extended`.
  `docker-publish.yml` publishes on a manual dispatch against
  `refs/heads/jay-spanner-extended` (or a release tag). A dispatch against the
  old name no longer publishes. README references were updated too.

### Documentation
- `README.md`: new "REST Gateway: Accurate Error Responses" and "PostgreSQL
  JSONB: Large Numbers on Every Platform" sections. Change stream creation
  times added to the Data Persistence list.
- `docs/change-streams.md`: creation times listed under What Persists.
- `docs/building.md` (new): how to build natively on macOS, with `build.sh`,
  and in CI; what each cache holds; what triggers a full rebuild; and how to
  check a slow local Docker build. `README.md`'s build sections now link to
  it, and their out-of-date toolchain versions (Ubuntu 18.04, Bazel 5.4.0,
  GCC 8.4/12) and "built on every push" claims are corrected.
- `docs/feature-coverage.yaml`/`.md`: `clients.rest_gateway` is now `tested`,
  `change_streams.read` notes cover creation-time validation, and a new
  `postgresql.jsonb` entry.

## [2026-09-23] Geo-Partitioning Placements

### Added
- Placements and placement keys in both dialects: `CREATE`/`ALTER`/`DROP
  PLACEMENT` with `instance_partition`, `default_leader` and
  `read_lease_regions`; one `NOT NULL STRING` placement key column per table;
  the `default` placement; `per_placement_routing_metadata`;
  `INFORMATION_SCHEMA` views; replayable `GetDatabaseDdl` output; and
  production's placement DML limits, on by default
  (`--enforce_placement_dml_restrictions`). See [placements.md](placements.md).

### Fixed
- Three `ALTER COLUMN ... PLACEMENT KEY` forms crashed the emulator (inherited
  from upstream); they now return errors.
- Conformance tests failed to compile on native macOS because
  `std::int64_t` is `long long` there (`076e77b8`).

## [2026-09-10] Change Stream Partitions Across Restarts

### Fixed
- Restarting a persisted database replayed the change stream's one-time
  partition backfill at a new timestamp and duplicated partitions. Replay now
  uses the database's creation time for the initial DDL batch, and the
  backfill treats any persisted partition as proof it already ran. Tests:
  `frontend/collections/database_manager_test.cc`.

### Added
- `candidate_only` input on the publish workflow: publishes only the
  immutable `<sha>` tag and leaves `latest` alone.
- Images carry `org.opencontainers.image.revision` with the source commit.

## [2026-08-26] UNAVAILABLE Database State for Restore Failures

Closes the "Known gaps" item from the 2026-08-18 entry below: a database
that fails to restore was previously left entirely absent from the catalog
for that run, indistinguishable from a database that never existed.
Implements the remaining scope of `openspec/changes/fix-unique-index-restore-isolation/`
section 3.

### Added
- **`DatabaseManager::MarkDatabaseUnavailable()` / `UnavailableReason()` /
  `ListUnavailableDatabases()`**: a new URI-sorted registry (separate from
  `database_map_`, since a database that failed to restore has no working
  `backend::Database` to construct) tracking failed-to-restore databases and
  their reason. See `frontend/collections/database_manager.{h,cc}`.
- **`RestoreFromMetadata()` marks failures `UNAVAILABLE`**: the per-database
  restore-failure branch in `binaries/emulator_main.cc` now calls
  `MarkDatabaseUnavailable()` with the restore error's message, unless the
  database was successfully quarantined (`--repair_corrupted_databases`), in
  which case it is removed entirely rather than left `UNAVAILABLE`.
- **`DatabaseManager::GetDatabase()` rejects unavailable databases**: returns
  `FAILED_PRECONDITION` naming the database and the restore failure reason.
  Since this is the single chokepoint used by session creation
  (`frontend::GetSession`) and `UpdateDatabaseDdl`, this uniformly blocks
  reads, writes, and DDL against an unavailable database without a
  per-handler change.
- **`ListDatabases`/`GetDatabase` (admin RPCs) surface `UNAVAILABLE`
  databases**: reported as `State::CREATING` — Cloud Spanner's
  `Database.State` enum has no dedicated failure value, and `CREATING`'s own
  documented contract already permits `FAILED_PRECONDITION` on operations
  against it, so this stays wire-compatible with the real API instead of
  inventing a new enum value. `ListDatabases` merge-walks the two
  URI-sorted sources (restored databases, unavailable databases) to keep
  pagination ordering intact. See `frontend/handlers/databases.cc`.
- **Tests**: `frontend/collections/database_manager_test.cc` covers
  marking/querying unavailable databases, `GetDatabase()` rejection with the
  reason in the error, isolation from unrelated databases, and
  instance-scoped listing.
- **`README.md`**: documents the `UNAVAILABLE` state and
  `--repair_corrupted_databases` under Data Persistence.

### Known gaps (not implemented in this pass)
- No regression test reproducing the original unique-index TOCTOU (tasks
  1.1–1.3 of the openspec change) and no stress test for concurrent
  colliding inserts + restart (task 2.2) — both require a running emulator
  binary and were out of scope for a build-free pass.
- No end-to-end verification (Docker build, localcloud's "Add Row"
  generator against a corrupted database) — task 6.2/6.3.
- Verification for the separate LevelDB write-queue race fix
  (`openspec/changes/fix-spanner-leveldb-race/tasks.md` sections 4.7, 5) is
  still outstanding and unrelated to this entry.

## [2026-08-26] Docker Publishing and CI Cache

### Changed
- Publishing is manual only. A `workflow_dispatch` on the working branch
  publishes `<sha>` and `latest`; other branches only build. A version tag
  `x.y.z` publishes `<sha>`, `latest` and `x.y.z` (no `v`-prefixed tag).
- The dispatch input `target` is `linux` (Docker images) or `arm` (also the
  native macOS arm64 archive, the default).
- The CI Bazel cache budget was raised (archives and expanded caches up to
  5 GiB, 8 GiB combined), and a twice-weekly scheduled run on `master` keeps
  the caches from being evicted.

## [2026-08-25] Feature Coverage Inventory

### Added
- `docs/feature-coverage.yaml`, the rendered `docs/feature-coverage.md`, and
  `tools/feature_coverage.py` to validate and render them. The
  `feature-coverage.yml` workflow fails when the rendered matrix is out of date
  or a registered RPC has no record.
- `TABLESAMPLE ... REPEATABLE` is accepted (upstream rejects it), with tests
  for BERNOULLI and RESERVOIR sampling.
- Partitioned DML on PostgreSQL databases is validated with the PostgreSQL
  analyzer, so parameterized statements work.
- Tests for `ISOYEAR` and `ISOWEEK` in `DATE_TRUNC`, `TIMESTAMP_TRUNC` and
  `DATE_DIFF`.

## [2026-08-18] Unique Index Restore-Time Corruption and Restore Fault Isolation

Implements `openspec/changes/fix-unique-index-restore-isolation/`. Fixes the
incident where localcloud's console "Add Row" generator produced a duplicate
key in a unique secondary index (`EmployeesByEmail`) that was accepted at
write time, only surfaced as `DATA_LOSS` on the next restart, and then took
down the whole emulator process (and every other instance/database) because
`RestoreFromMetadata()` propagated a single database's restore failure
straight to `main()`.

### Investigation

Reviewed `backend/locking/manager.cc` (`LockManager::EnqueueLock`'s
wound-wait), `backend/transaction/read_write_transaction.cc` (`Write()` /
`Commit()`), and `frontend/handlers/transactions.cc`'s `Commit` RPC handler.
Confirmed that a plain mutations-only `Commit` RPC calls `txn->Write(mutation)`
and `txn->Commit()` as two separate top-level calls in the same handler; each
only holds `ReadWriteTransaction::mu_` for its own duration
(`GuardedCall`), so there is a real (if narrow) window between them where
`LockManager`'s wound-wait can hand the whole-database lock to a different,
concurrently-arriving transaction. `UniqueIndexVerifier::Verify()`
(`backend/actions/unique_index.cc`) only runs once, inside `Write()` --
`Commit()` never re-checks uniqueness before flushing to `PersistentStorage`,
so nothing re-validates that the verified-at-Write()-time state still holds
by the time mutations are about to become durable. This is a textbook
time-of-check-to-time-of-use gap, and it holds regardless of the exact
interleaving that lets a transaction reach `Commit()` after the world moved
on underneath it (lock hand-off, or any future change to
`PersistentStorage`'s read/write ordering, e.g. the separate
`fix-spanner-leveldb-race` change).

### Fixed
- **Unique index TOCTOU at commit**: Added `ReadWriteTransaction::ReverifyBufferedWriteOps()`, called in `Commit()` immediately before `FlushWriteOpsToStorage()` (and before reserving a commit timestamp), which re-runs all verifiers -- in practice only `UniqueIndexVerifier`, the only `Verifier` subclass in the codebase -- against every mutation buffered so far in the transaction. Because this runs inside the same continuously-held `mu_` critical section as the flush, it is the last point before mutations become durable and closes the gap described above: a duplicate unique-index key can no longer reach `PersistentStorage`, regardless of what interleaving got the transaction to `Commit()`. A failure here is handled identically to a `Write()`-time verification failure (same `UniqueIndexConstraintViolation` error, same `GuardedCall` reset/cleanup path). See `backend/transaction/read_write_transaction.{h,cc}`.
- **Per-database restore fault isolation**: `RestoreFromMetadata()` (`binaries/emulator_main.cc`) no longer aborts the whole process when one database fails to restore (for example, discovering a unique-index violation left over from before the fix above, or any other restore error). The per-database restore body is now an isolated lambda; a failure is logged with the database URI and reason, and the loop continues with the next database. `main()` only fails startup for errors that aren't scoped to a single database (the metadata catalog itself being unreadable stays fatal, since no database identities are known at all in that case).
- **Second-crash fix**: The earlier `MarkDatabaseMetadataCommitted` pass (over all persisted database URIs, before the main per-database restore loop) no longer treats a single database's metadata/storage mismatch as fatal either -- previously, manually removing a corrupted database's on-disk directory to work around the first crash tripped a *second*, different `DATA_LOSS` ("Persistent database root is unavailable for metadata commit") that still took down the whole process. That error message now also names the specific database and, when the root is simply missing, says so explicitly instead of a generic "unavailable" message. See `frontend/collections/database_manager.cc`.
- **`--repair_corrupted_databases` startup flag**: When a database fails to restore and this flag is set, its on-disk LevelDB directory is moved aside under `<data_dir>/.quarantine/` and its `metadata.json` entry is removed (directory rename first, then `MetadataStore::Save()`'s already-atomic temp-file-plus-rename), so it stops blocking future startups. Without the flag, a database that fails to restore is simply left in place (and unavailable for this run) so an operator can inspect it before deciding to discard it. See `common/config.{h,cc}`, `binaries/emulator_main.cc`.

### Known gaps (not implemented in this pass)
- A database that fails to restore is *not* currently listed by `DatabaseAdmin.ListDatabases`/`GetDatabase` with an explicit unavailable state (it is simply absent from the catalog for that run, the same as if it were quarantined). Surfacing it as visible-but-unavailable, as originally scoped in `specs/restore-fault-isolation/spec.md`, would need a `state` field threaded through `DatabaseManager`/`frontend::Database` and is left as follow-up work.
- No automated regression/stress tests were added for either fix (per `tasks.md` sections 1-6) -- this pass was code review and fixes only, with test execution and building explicitly out of scope for this change.

## [2026-08-16] Backups, Admin API Extensions and Durable Change Streams

### Added
- Backups: `CreateBackup`, `CopyBackup`, `GetBackup`, `ListBackups`,
  `UpdateBackup`, `DeleteBackup` and `RestoreDatabase`. They require
  `--data_dir`; each backup is a full copy of the database.
- Backup schedules: create, get, list, update and delete. Schedules are stored
  but never run.
- `ListBackupOperations` and `ListDatabaseOperations`; `UpdateDatabase` with
  `enable_drop_protection`, enforced by `DropDatabase`; `ListDatabaseRoles`
  (always empty); `AddSplitPoints` (accepted, no effect).
- Custom instance configs (create, update, delete, list operations),
  `MoveInstance` (metadata only), `FetchCacheUpdate` (no updates), and IAM
  policy storage (`GetIamPolicy`/`SetIamPolicy`; not enforced).
- With `--data_dir`: IAM policies, custom instance configs, instance
  partitions and long-running operations persist; each DDL batch is recorded
  with its commit timestamp; an interrupted `UpdateDatabaseDdl` is finished or
  rolled back at the next startup. Each database gets its own directory, and
  the older layout is migrated automatically.
- Change stream creation times survive restarts and backup restores, and the
  initial partition backfill runs only once.

### Fixed
- Merging change stream partitions crashed with more than two tokens
  (`6cecbd4a`).

## [2026-08-15] Native macOS Build

### Added
- CI builds a native macOS arm64 archive (`spanner-emulator-macos-arm64`) on
  release tags and on `target=arm` dispatches.

### Fixed
- Docker builds in CI work without Docker Hub credentials (no cache export).

## [2026-06-01] Persistent Storage Write Queue and ARRAY Values

### Fixed
- Writes to LevelDB go through a single queue, so concurrent writers can't
  interleave. See [internals/persistent-storage.md](internals/persistent-storage.md).
- `ARRAY` column values persist and reload correctly.
- GoogleSQL build fix for GCC 12 `constexpr` errors; see
  [plans/2026-06-01-zetasql-constexpr-fix.md](plans/2026-06-01-zetasql-constexpr-fix.md).

## [2026-05-08] OPTIMIZER_VERSION Hint and Full-Text Search Fix

### Added
- **OPTIMIZER_VERSION Statement Hint**: Production queries using `@{OPTIMIZER_VERSION=latest}` no longer fail with "invalid hint". Added `optimizer_version` to the hint whitelist in `query_validator.cc`. Accepts STRING and INT64 values, silently ignored (emulator has no optimizer versioning).

### Fixed
- **TOKENIZE_FULLTEXT `remove_diacritics`**: Added missing `remove_diacritics` boolean parameter to `TOKENIZE_FULLTEXT` function signature in the search function catalog. Enables diacritic-insensitive full-text indexing.
- **GCC 12 `optional` Include**: Fixed missing `<optional>` include in `conversion_finder.cc` for GCC 12 compatibility.

## [2026-05-07] GoogleSQL Upgrade Analysis

### Added
- **Upgrade Analysis Document**: Documented analysis of upgrading from GoogleSQL 2025.09.1 to 2026.01.1, identifying 3 major blockers (Bzlmod migration, namespace rename, patch rebase). Recommendation: don't upgrade unless needed. See `docs/plans/2026-05-07-googlesql-upgrade-analysis.md`.
- **Feature Gap Analysis**: Analyzed 9 features from the full-text search emulation proposal. Found 8 of 9 already supported in current ZetaSQL 2025.09.1 base (TOKENLIST, TOKENIZE_NGRAMS, TOKENIZE_FULLTEXT, SEARCH_NGRAMS, SCORE_NGRAMS, SOUNDEX, SAFE_DIVIDE, NORMALIZE, FORCE_INDEX hint, HIDDEN columns, named arguments, Unicode regex, SEARCH INDEX DDL, generated columns).

## [2026-05-05] Build Optimization and Stability Improvements

### Added
- **Persistent Build Cache**: Implemented BuildKit cache mounts (`--mount=type=cache`) in `Dockerfile.ubuntu` for Bazel's disk and repository caches. This reduces subsequent build times from hours to ~2 minutes by persisting compilation artifacts across Docker runs.
- **Resource Management**: Added explicit `BAZEL_JOBS=4` and `BAZEL_RAM=50%` defaults in `build.sh` to ensure build stability and prevent host system resource exhaustion.
- **APT Retries**: Added `Acquire::Retries "3"` configuration to Dockerfiles to improve reliability of package installations in transient network conditions.

### Changed
- **Toolchain Upgrade**: Upgraded the compiler to **GCC 12** in `Dockerfile.ubuntu` and `Dockerfile.base`. This provides better stability and support for modern C++ features required by the latest ZetaSQL and dependencies.
- **ZetaSQL Build Optimizations**: Configured `-O0` optimization level in `.bazelrc` for heavy ZetaSQL rewriter and visitor files.
    - *Reasoning*: These specific files (e.g., `resolved_ast_rewrite_visitor.cc`, `order_by_and_limit_in_aggregate_rewriter.cc`) are known to cause extreme RAM usage and build hangs/OOMs at higher optimization levels.
- **Offline Build Portability**: Updated `fetch_workspace_deps.sh` to use script-relative paths instead of hardcoded absolute paths, enabling the offline pre-fetch process to work in any environment.

### Fixed
- **`NoDestructor` Initialization Ambiguity**: Fixed compilation failures in `conversion_finder.cc`, `spangres_function_filter.cc`, and `pg_jsonb_conversion_functions_test.cc` caused by GCC 12's stricter constructor resolution.
    - *Reasoning*: Explicitly naming the type (e.g., `ConversionMap({...})`) in the `zetasql_base::NoDestructor` constructor resolves an ambiguity between the variadic constructor and the move constructor when using brace-enclosed initializer lists.
- **Docker License Extraction**: Corrected the path for `licenses.txt.gz` in `Dockerfile.ubuntu` and ensured the generation script runs from the workspace root to correctly locate external dependencies.

## [2026-04-17] Data Persistence and Multi-Arch Docker

### Added
- **LevelDB Data Persistence**: Full persistent storage backend using LevelDB. Multi-version cell storage with microsecond-precision timestamps, sort-order-preserving key encoding, per-table prefix scanning, and thread-safe access. Activated via `--data_dir=/path` flag. Default (empty) = in-memory mode.
- **Metadata Persistence**: Atomic JSON-based persistence for instances (display_name, config, processing_units, labels, create_time) and databases (dialect, DDL statements). Write-tmp-then-rename pattern for crash safety.
- **ID Generator Persistence**: `Seed()` and `GetIdCounterValues()` methods on UniqueIdGenerator. Persists table_id, column_id, change_stream_id, sequence_id, and named_schema_id counters. Prevents ID collisions with existing LevelDB data after restart.
- **Automatic Recovery on Startup**: `RestoreFromMetadata()` in `emulator_main.cc` reconstructs instances, databases, DDL, dialect, and seeds ID generators from persisted `metadata.json`.
- **Database Operation Persistence**: CreateDatabase, UpdateDatabaseDdl, and DropDatabase operations automatically persist metadata and clean up LevelDB directories.
- **Multi-Arch Docker CI/CD**: GitHub Actions workflow publishing `jaysen2apache/spanner-emulator-extended` to Docker Hub with multi-arch manifests (linux/amd64 + linux/arm64).

### Fixed
- **ID Generator Move Assignment**: Fixed build failure caused by deleted move assignment operator for `UniqueIdGenerator` in `ids.h`.
- **Mutex Pattern in Const Accessors**: Fixed mutex usage in const accessor methods in `ids.h` to match existing codebase patterns.
- **Data Read Exclusion Bug**: Fixed row exclusion logic in `persistent_storage.cc` with added test coverage.
- **CRC32C GC Improvements**: Fixed CRC32C garbage collection in persistent storage.

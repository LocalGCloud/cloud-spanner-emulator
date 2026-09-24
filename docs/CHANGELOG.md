# Changelog

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

### Documentation
- `README.md`: new "REST Gateway: Accurate Error Responses" and "PostgreSQL
  JSONB: Large Numbers on Every Platform" sections. Change stream creation
  times added to the Data Persistence list.
- `docs/change-streams.md`: creation times listed under What Persists.
- `docs/feature-coverage.yaml`/`.md`: `clients.rest_gateway` is now `tested`,
  `change_streams.read` notes cover creation-time validation, and a new
  `postgresql.jsonb` entry.

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

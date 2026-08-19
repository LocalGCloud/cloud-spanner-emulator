# Changelog

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

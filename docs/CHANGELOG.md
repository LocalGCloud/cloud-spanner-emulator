# Changelog

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

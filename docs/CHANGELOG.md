# Changelog

Changes in this fork (`jay-spanner-extended`), newest first. Upstream emulator
releases are merged separately; the last one merged is the 2026-08-03 import.
Upstream's 2026-09-03 and 2026-09-14 imports aren't merged yet.

## [2026-09-28] Docker Packaging & Image Test Qualification

Full qualification of the packaged Docker image `spanner-emulator-extended:local`
across client SDKs, persistence volume restarts, directory locking, flag
forwarding, and feature spot checks, plus build caching optimizations. Evidence:
`tests/image_verification_test.py` and `tests/client_matrix/` passed 100%.

### Added
- Automated Docker image qualification suite `tests/image_verification_test.py`:
  - Persistent volume restart test covering GoogleSQL and PostgreSQL databases:
    database roles and privilege enforcement (`creator_role`), DEFINER views,
    database options (`versionRetentionPeriod = 2h`), TTL policies (`ADD ROW DELETION POLICY`
    and `TTL INTERVAL`), sequences (`BIT_REVERSED_POSITIVE`), unique secondary
    indexes, point-in-time backup restore (`version_time`), split points
    (`AddSplitPoints`), and `SPANNER_SYS` query statistics across container restarts.
  - Data directory lock test: verifies that a second container mounting an active
    `--data_dir` volume immediately exits with code 1 and lock acquisition error.
  - Flag forwarding verification: confirms `--row_deletion_policy_sweep_interval_seconds=5`
    (expired rows swept within 5s) and `--spanner_sys_expose_open_interval=true`
    (open-interval statistics visible in `QUERY_STATS_TOP_MINUTE`).
  - Feature spot checks: verifies `REPEATABLE_READ` snapshot conflict aborts (409),
    wound-wait lock wait and commit, `PLAN` and `PROFILE` query plans (16-node trees
    with execution statistics), GQL `CALL PageRank(...)` scores, full-text Unicode
    search, and PostgreSQL ScaNN index creation with `spanner.approx_cosine_distance`.
- Build caching enhancements in `build.sh`:
  - Build context transfer reduced by 99.2% (from 324 MB to 2.65 MB) via `.dockerignore`.
  - Offline repository cache auto-skips redundant host `bazel fetch` when
    `bazel-distdir/content_addressable` is already populated.
  - Local image build time cut to 410s (~6.8 minutes) via BuildKit cache mounts and
    Bazel disk cache hits.

### Changed
- Client matrix (`tests/client_matrix/`) qualified 100% against the running
  container: REST (22/22 with Google JSON error envelopes for 400, 403, 404, 409),
  Go (16/16), Node.js (16/16), Python (16/16), Java (16/16), JDBC (18/18),
  PGAdapter (13/13), and psql (100%).
- Updated feature coverage inventory (`docs/feature-coverage.yaml`,
  `docs/feature-coverage.md`) marking `clients.docker` and `emulator.packaging` as
  fully tested.

## [2026-09-28] Remaining Limitations

Three batches after the developer-usability closure. Evidence: the combined
native suite (fixed-environment Bazel) passed 164 of 164 targets on
`bd6a9646`. The client SDK matrix and the `--data_dir` restart probe were not
re-run for these batches, and the packaged Docker/LocalCloud image was not
qualified. See the
[usability worksheet](plans/2026-09-27-usability-audit-worksheet.md#remaining-limitations-round-2026-09-28).

### Added
- Graph algorithms are computed in memory instead of zero-argument stand-ins:
  `PageRank` (including personalized), `BetweennessCentrality`,
  `ClosenessCentrality`, `WeaklyConnectedComponents`, `ModularityClustering`,
  `CorrelationClustering`, `LabelPropagation`, `CliqueFinding`, Jaccard,
  cosine, common-neighbors and total-neighbors similarity, and `ShortestPath`
  (path and cost). `GRAPH g CALL Algo(...) YIELD ...` and `CALL PER ()` over a
  `FULL UNION ALL` input take the documented named arguments, and the
  documented `PageRank` example returns the documented results. `EXPORT DATA`
  with `format = 'CLOUD_SPANNER'` writes results back to Spanner tables
  (update or upsert; rows that violate constraints are skipped); CSV, Parquet
  and Avro validate their options and then discard the rows.
  `ELEMENT_DEFINITION_NAME` is allowed.
- Resume tokens for `ExecuteStreamingSql` and `StreamingRead` at row
  boundaries (responses hold whole rows up to 1 MB; a larger row is still
  split). A request resent with a token continues after that row in the same
  transaction or at the original read timestamp. Mismatched or garbled tokens,
  or a `begin` selector, return `INVALID_ARGUMENT`; results that differ on
  re-execution (for example `RAND()`) return `FAILED_PRECONDITION`. DML has no
  tokens.
- Change stream resume tokens (partition token, commit timestamp and record
  index) replace the placeholders; a resumed query skips the records already
  returned.
- Wound-wait lock waits: an older transaction wounds a younger holder, and a
  younger requester waits up to the new `--lock_wait_timeout_ms` (default
  10000; `0` restores abort-at-once, forwarded by `gateway_main`) and then
  aborts. `--abort_current_transaction_probability` now sets how often a
  younger transaction aborts an idle older holder instead of waiting. Schema
  changes never wait. `SPANNER_SYS.LOCK_STATS` `LOCK_WAIT_SECONDS` and
  `TOTAL_LOCK_WAIT_SECONDS` are measured.
- `SPANNER_SYS` statistics persist per database with `--data_dir`
  (`spanner_sys_statistics.json`), saved when each minute interval ends and on
  `SIGTERM`/`SIGINT`; a crash loses at most the current minute, and dropping
  the database deletes the file.
- `SPANNER_SYS` tables `ROW_DELETION_POLICIES`, `USER_SPLIT_POINTS`,
  `TABLE_SIZES_STATS_1HOUR` (a logical estimate sampled every 5 minutes and
  averaged per hour; HDD bytes 0) and `ACTIVE_PARTITIONED_DMLS` (one partition,
  progress 0 until done).
- `AddSplitPoints` validates and stores split points, persisted with
  `--data_dir` and shown in `USER_SPLIT_POINTS` until they expire. They still
  don't change how data is stored.
- `--data_dir` lock: `emulator_main` holds `<data_dir>/.lock` with its PID, and
  a second process on the same directory exits at startup with a clear error.
- Fine-grained access control: `SQL SECURITY INVOKER` view bodies are checked
  at analysis, so `PLAN` mode is checked too; sequences used by `DEFAULT` or
  generated columns need `SELECT` or `UPDATE` on the sequence; `pg_catalog`
  rows are filtered by role like `INFORMATION_SCHEMA`.
- PostgreSQL `GetDatabaseDdl` prints `CREATE SEARCH INDEX` (with `INCLUDE`,
  `PARTITION BY`, `ORDER BY`, `WHERE` and `WITH`) and
  `CREATE INDEX ... USING scann (...) WITH (...)` instead of plain
  `CREATE INDEX`.
- `CommitRequest.max_commit_delay` is validated to 0–500 ms
  (`INVALID_ARGUMENT` otherwise); commits are still immediate.
- `google.longrunning` `ListOperations` applies the shared AIP-160 filter;
  unknown fields are rejected.
- `OPTIMIZER_VERSION` and `OPTIMIZER_STATISTICS_PACKAGE` hints are accepted on
  DML (`OPTIMIZER_VERSION` used to fail there), and
  `OPTIMIZER_STATISTICS_PACKAGE` on queries; package names aren't validated.
- Search: the `SCORE_NGRAMS` `array_aggregator` argument (`flatten`,
  `max_element`; GoogleSQL only); `SCORE` `version` (no local effect) and
  `token_category_weights` options, while `bigram_weight` and `idf_weight`
  stay `UNIMPLEMENTED`; PostgreSQL `SCORE` options take effect; Unicode terms
  in RQUERY; HTML content skips `script` and `style` and decodes numeric and
  the 253 HTML 4 named entities; `short_tokens_only_for_anchors` has its
  documented effect; `TOKENLIST_CONCAT` of mixed `remove_diacritics` settings
  is diacritic-insensitive; French text follows the documented rules; CJK text
  is split where the script changes (there are no CJK dictionaries, so pure
  Chinese text isn't segmented); `DEBUG_TOKENLIST` matches both documented
  examples (`TOKENLIST`s stored earlier lack the boundary and hashtag
  markers); PostgreSQL `spanner.tokenize_fulltext` accepts `remove_diacritics`
  and `spanner.tokenize_substring` exposes `support_relative_search`.
- Change stream records older than the retention period are deleted on each
  partition churner cycle.

### Fixed
- Data correctness: with `--data_dir`, range reads and range deletes could
  skip rows, because the on-disk key order is length-then-content while range
  scans assumed content order. Unique index checks then missed existing
  values, so duplicates could commit and later break the restore; this was
  the root cause of the earlier unique-index incident, now closed. Fixed in
  `backend/storage/persistent_storage.cc` without a format change; a
  concurrent stress test failed about half its runs before the fix and passed
  30 of 30 after.
- PostgreSQL `spanner.tokenize_substring` passed its arguments in the wrong
  order and crashed with `relative_search_types`.
- `MUTABLE_KEY_RANGE` change streams re-sent partition start (`move_in`)
  records on every scan.

### Known issues (pre-existing, not fixed)
- Graph `RETURN ... ORDER BY` is ignored.
- A change stream scan can skip a commit at the exact scan-boundary
  microsecond.
- No 10-second idle-transaction abort, lock waits ignore the RPC deadline, and
  `READ_STATS` `AVG_LOCKING_DELAY_SECONDS` is still 0.

### Docs
- `docs/feature-coverage.yaml`: `graph.algorithms` is now supported (138
  supported, 8 accepted-no-op, 9 not-applicable); `database.split_points` and
  `googlesql.hints` stay accepted-no-op because their documented effects are
  physical or performance-only. Notes and evidence updated for the records
  above; the `emulator.persistence` note no longer credits the restart probe
  with checks it didn't run.
- Known gaps, capabilities, configuration, persistence, change stream,
  README and testing docs updated for the changes above.

## [2026-09-28] Developer-Usability Closure

Closes the plan in [docs/plans/2026-09-27-usability-closure-plan.md](plans/2026-09-27-usability-closure-plan.md).
Evidence (full native suite, public-endpoint client matrix, doc-example
conformance, and a `--data_dir` restart probe) is in the
[usability worksheet](plans/2026-09-27-usability-audit-worksheet.md). The
packaged Docker/LocalCloud image was not re-qualified.

### Added
- Database roles and fine-grained access control in both dialects: `GRANT` and
  `REVOKE` on tables (with column lists), views, change streams and their
  read functions, sequences, models and schemas, and role membership, persisted, printed by
  `GetDatabaseDdl`, and carried by backups. Sessions with a `creator_role` are
  restricted on queries, DML, reads, mutations, and change stream reads;
  `SQL SECURITY DEFINER` views (now also in PostgreSQL) need only `SELECT` on
  the view. `ListDatabaseRoles` includes the system roles.
- `INFORMATION_SCHEMA` `ROLES`, `ROLE_GRANTEES`, the privilege and
  `ROLE_*_GRANTS` views, `ROUTINES`, `ROUTINE_OPTIONS`, `PARAMETERS`, and
  `TABLE_SYNONYMS` (PostgreSQL `enabled_roles` and `applicable_roles`), with
  documented role filtering.
- `SPANNER_SYS` query, read, transaction, and lock statistics, table and
  column operation statistics, `OLDEST_ACTIVE_QUERIES`, and
  `ACTIVE_QUERIES_SUMMARY`, measured per database in both dialects. New flag
  `--spanner_sys_expose_open_interval`, forwarded by `gateway_main`.
- Query plans for `PLAN`, `PROFILE`, `WITH_STATS`, and `WITH_PLAN_AND_STATS`,
  built from the query, with per-scan `PROFILE` statistics and plans on
  streaming responses.
- `REPEATABLE_READ` isolation with snapshot reads and commit-time write-write
  validation.
- Row deletion policies (TTL) delete expired rows in system transactions
  tagged `RowDeletionPolicy`, honoring `exclude_ttl_deletes`. New flag
  `--row_deletion_policy_sweep_interval_seconds` (default 60), forwarded by
  `gateway_main`.
- `CreateBackup` `version_time`; Google-default backup encryption types with
  `encryption_info`; incremental backup schedules as metadata chains over full
  copies.
- `MUTABLE_KEY_RANGE` change streams emit `MOVE` partition events.
- AIP-160 list filters for `ListInstances`, `ListSessions`, `ListBackups`, and
  the Spanner operation lists.
- `CommitStats.mutation_count` with `return_commit_stats`.
- `ALTER SEARCH INDEX` column changes in both dialects; PostgreSQL vector
  indexes (`USING ScaNN`) and `spanner.approx_*` functions.
- Functions: `ZSTD_COMPRESS`, `ZSTD_DECOMPRESS_TO_BYTES`,
  `ZSTD_DECOMPRESS_TO_STRING`, `SPLIT_SUBSTR`,
  `LCASE`/`UCASE`/`ADDDATE`/`SUBDATE`, `DEBUG_TOKENLIST`; PostgreSQL
  `generate_series`, `spanner.split_substr`, `make_interval`,
  `pg.ilike`/`pg.not_ilike`, `!~~`, `NOT LIKE/ILIKE ... ESCAPE`, and
  `array_agg(uuid)`.
- `tests/client_matrix`: public-endpoint smoke programs for Python, Go, Node,
  Java, JDBC, PGAdapter/pgJDBC/psql, and REST.

### Fixed
- REST errors now use Google's `{"error": {"code", "message", "status",
  "details"}}` envelope with the HTTP status code.
- DML `UPDATE` writes only the key, `SET`, and commit-timestamp/`ON UPDATE`
  columns, so change stream records match `Update` mutations. (The docs had
  blamed DML `INSERT`.)
- `LOCK_SCANNED_RANGES=exclusive` takes exclusive locks; in a two-key deadlock
  exactly one transaction aborts; a future-timestamp read past the request
  deadline fails with `DEADLINE_EXCEEDED`.
- `InternalUpdateGraphOperation` with an OK status or 100% progress completes
  the operation.
- Search regressions that crashed or returned wrong rows: `SEARCH` over a SQL
  NULL token list returns NULL, an empty `TOKENLIST_CONCAT` matches nothing
  instead of failing a `RET_CHECK`, and search functions over numeric, boolean,
  or exact-match token columns are rejected at analysis time. `SNIPPET` uses
  the documented JSON layout.
- `TO_BASE32` and `FROM_BASE32` were registered but failed at execution (the
  docs had called them working); they now run.
- PostgreSQL: `jsonb || jsonb` no longer puts the right array first;
  `->`/`->>` with a negative index return NULL, as Spanner documents;
  `regexp_replace` replaces only the first match, not every match;
  `pg_proc.proname` is no longer schema-qualified for functions in named
  schemas;
  `information_schema.locality_group_options` has `locality_group_name` and
  shows option values instead of their type.
- ML and remote-UDF requests no longer reuse a constant `requestId`; each call
  carries a distinct one (tests updated).
- A data race in the per-schema action registry: change stream churner threads
  writing alongside user transactions could insert into its table map during
  lookups. Lookups no longer modify the map.
- `ListBackups` filter precedence now follows AIP-160 (OR binds tighter than
  AND); `AND` had bound tighter.

### Docs
- `docs/feature-coverage.yaml`: every former partial, unsupported, and unknown
  record re-evaluated (now 137 supported, 9 accepted-no-op, 9 not-applicable);
  new not-applicable records `security.iam_enforcement`, `ops.rate_quotas`, and
  `vector.ann_recall_latency`.
- Added the usability audit worksheet with post-implementation results, and
  corrected stale claims (DML `INSERT` blamed for change stream `NULL`s,
  `TO_BASE32`, PostgreSQL `APPROX_*`, graph algorithms, the 80,000-cell
  limit, `SPANNER_SYS`, and backup `version_time`).
- Marked the independent audit prompt and the partial-feature support plan as
  superseded, and noted the usability definition and new records in the
  coverage design record.

## [2026-09-27] Additional Partial-Feature Slices

### Improved
- `GetDatabase` now reports a moving `earliest_version_time`, and
  `INFORMATION_SCHEMA.DATABASE_OPTIONS` projects stored regional options.
  Focused native readback and metadata-replay tests pass; real process restart
  and packaged qualification remain open.
- A direct native REST regression now preserves a 2,052-byte invalid-mask
  error within a 4,096-byte shared cap. The focused gateway and status tests
  pass; packaged REST and PostgreSQL SQLSTATE over HTTP remain open.
- Full backup schedules now run with `--data_dir`, persist each due backup and
  next cursor together, and recover across process restart. Cron validation
  covers 12-hour, daily, weekly, and monthly UTC runs; incremental schedules
  and encryption remain unavailable.
- Key/range locks allow disjoint active writes. The full lock/transaction
  targets and 20 SELECT FOR UPDATE conformance cases passed; broad-scan and
  direct-DML conflict coverage remains open.
- SQL NULL tokenizer inputs return SQL NULL, and PostgreSQL floating SUM/AVG
  empty/all-NULL and NUMERIC/JSONB/OID ARRAY_AGG cases pass focused tests.
- Configured local ML predictions passed public gRPC tests in both dialects.
  GoogleSQL remote UDF calls now propagate provider errorMessage and send a
  distinct requestId for each call. Packaged LocalCloud remains unqualified.
- `gateway_main` now forwards `--remote_functions_host_port` to the native
  emulator. The native gateway test and binary build pass; packaged LocalCloud
  behavior has not been qualified.
- `CREATE ROLE` and `DROP ROLE` now persist in both dialects, and
  `ListDatabaseRoles` lists and paginates stored roles. Durable schema replay
  has a separate native test. Role grants, privilege views, and session
  authorization remain open.
- Custom instance configs now rotate opaque etags and reject stale updates or
  deletes. Their operation list validates parent-bound page tokens and applies
  simple filters. A native `--data_dir` restart probe verified persisted etags;
  full Cloud filter semantics and physical placement remain open.
- Commit and BatchWrite reject more than 80,000 distinct explicit write cells
  with `INVALID_ARGUMENT`; generated, index, delete, and DML effects are not
  counted yet.
- `SEARCH` applies `language_tag` casing to WORDS and WORDS_PHRASE queries.
  PostgreSQL change-stream `value_capture_type = NULL` resets an explicit
  value; native PostgreSQL `RESET` is covered by focused updater tests.
- PostgreSQL errors preserve standard rich-status details and the capped
  message. `SetIamPolicy` checks supplied etags and rotates them on success.
  IAM permissions are still not evaluated or enforced.

## [2026-09-27] Database Drop Protection and Backup Metadata

### Fixed
- `UpdateDatabase` returns typed completion metadata and its documented drop
  protection now prevents `DeleteInstance`, including when a persisted
  database is unavailable. Focused handler tests and a direct gRPC restart,
  invalid-mask, disable, and delete probe passed. `database.update` is
  supported in the native emulator.
- `ListBackups` applies filters, newest-first ordering, and bounded opaque
  pagination. Backup metadata calls prune expired backups through the existing
  deletion path; update and copy enforce expiry bounds. The full backup
  handler target and a direct gRPC update/copy/filter/two-restart expiry probe
  passed. `backups.metadata` is supported natively, with opportunistic
  expiration cleanup. Packaged LocalCloud qualification remains open.

## [2026-09-27] Read After Failed DML

### Fixed
- A constraint-tagged DML error now leaves the active transaction readable,
  including earlier buffered writes. Further DML and commit replay the saved
  error; a rejected commit invalidates the transaction. The formerly disabled
  conformance case passed in both dialects, and 32 surrounding transaction
  error cases passed. Batch DML and backend Write failures remain unqualified,
  so `transactions.transaction_errors` remains partial.

## [2026-09-27] Unicode Search Tokenization Slice

### Improved
- Full-text, substring, and n-gram tokenization now use ICU word boundaries,
  Unicode code points, and optional diacritic removal. Basic HTML text and
  token categories are represented in token lists and local scoring. Five
  focused tokenizer/category test targets and a filtered GoogleSQL SQL
  conformance case passed. Full HTML5/language/rquery/NULL behavior remains
  partial; packaged LocalCloud qualification is open.

## [2026-09-27] UpdateInstance Operation Metadata

### Fixed
- `UpdateInstance` now returns the declared `UpdateInstanceMetadata` in its
  completed operation, with the updated instance and start/end timestamps.
  Focused handler and persistence tests passed, followed by a direct gRPC
  create/update/get test across an `emulator_main --data_dir` restart.
  Display name, labels, and capacity fields persist; capacity remains metadata
  only. The native `instance.update` row is supported, while packaged
  LocalCloud qualification remains open.

## [2026-09-27] Exact Vector Distance Conformance

### Verified
- Native conformance tests now cover exact cosine, Euclidean, and dot-product
  functions for FLOAT32 and FLOAT64 vectors in both dialects, including
  PostgreSQL's INT64 dot product, stored-vector queries, and representative
  NULL, invalid-vector, NaN, infinity, and overflow cases. The existing
  GoogleSQL evaluator and PostgreSQL mapping needed no code change.
- `vector.distance_functions` is supported in the native emulator. Approximate
  vector indexing and packaged LocalCloud qualification remain open.

## [2026-09-27] UUID Public-API and Persistence Support

### Added
- Conformance tests for UUID keys and values through mutations, keyed reads,
  typed SQL results and casts in both dialects, GoogleSQL `NEW_UUID()` and
  `GENERATE_UUID()`, and PostgreSQL `gen_random_uuid()` with default-key
  `INSERT ... RETURNING`. The focused conformance run passed five cases; the
  GoogleSQL parameter of the PostgreSQL-only default case skipped as intended.
  Converter value and read test targets also passed.
- PostgreSQL change-stream conformance now checks UUID type metadata and key
  and value JSON for INSERT and UPDATE. A direct gRPC test starts
  `emulator_main` twice with one disposable `--data_dir` and confirms typed
  PostgreSQL UUID reads after restart. UUID is supported in the native emulator;
  packaged LocalCloud image qualification remains separate.

### Fixed
- Persistent storage now encodes UUID values and UUID array elements and uses
  a distinct, ordered encoding for UUID keys. Before this fix, committing a
  UUID row with `--data_dir` aborted in the value codec. Codec, database
  close/reopen, and complete storage/database test targets pass.
- Conformance's macOS link failed because two test helper libraries defined
  `GetRunfilesDir`. The shared test utility now owns its sole definition.

## [2026-09-24] REST Field Masks in the URL Accept camelCase

### Fixed
- Over REST, a field mask passed in the URL wasn't converted from its JSON
  form, so `PATCH .../databases/db?updateMask=enableDropProtection` failed
  with `Unsupported database update field: enableDropProtection`, and
  `UpdateBackup` (`?updateMask=expireTime`) and `UpdateBackupSchedule`
  (`?updateMask=retentionDuration`) failed the same way. Cloud Spanner's REST
  API documents the camelCase form. grpc-gateway copies query-string mask
  paths as-is, while the handlers compare proto field names; masks in a
  JSON body (such as `UpdateInstance`'s `fieldMask`) were already converted
  by protojson.
- `gateway_main` now fills requests with its own query parser, which runs
  grpc-gateway's default parser and then converts each field mask path on
  the request from lowerCamelCase to proto field names (`expireTime` to
  `expire_time`, `encryptionConfig.kmsKeyName` to
  `encryption_config.kms_key_name`). snake_case paths are unchanged, so both
  forms work. gRPC clients weren't affected.
- Tests: `TestQueryParserConvertsFieldMaskPathsToProtoNames` in
  `//gateway:gateway_test` (fails with the default parser). Checked end to
  end over REST: all three RPCs rejected the camelCase mask with the
  previous build and accept it now, and snake_case and body masks still
  work.

## [2026-09-24] Commits Are Atomic on Disk

### Fixed
- With `--data_dir`, a commit's rows, index entries and change stream records
  were written to LevelDB one row at a time, so a crash or a write error
  partway through a commit could leave part of the transaction on disk, for
  example an index entry without its row, which could then fail the
  database's restore checks. A commit is now one LevelDB write batch, and
  LevelDB applies a batch all or nothing, also across a crash.
- New `Storage::ApplyRowOps` applies a group of row writes and point deletes
  at one timestamp. `PersistentStorage` builds one `WriteBatch` for the group
  with the same encoding as `Write()` and `Delete()`, submits it through the
  write queue, then prunes expired versions as before. `InMemoryStorage`
  keeps applying the ops one by one. `FlushWriteOpsToStorage` makes one
  `ApplyRowOps` call per commit, which resolves upstream's TODO in
  `backend/transaction/flush.h`.
- Writes still use `sync = false`: an OS crash or power loss can lose the
  most recent commits, but never part of one. The design is in
  [internals/persistent-storage.md](internals/persistent-storage.md#atomic-commits).
- Tests: `PersistentFlushTest.PersistentCommitIsAllOrNothing` in
  `backend/transaction:flush_test` (a commit across two tables where storage
  refuses one table's rows; before the fix the other table's first row was
  on disk) and three `PersistentStorageTest.ApplyRowOps_*` cases. Checked
  end to end by killing `emulator_main` with `SIGKILL` while a client
  committed 50-row transactions to an indexed table, then restarting: with
  the previous build 5 of 12 crashes left a partial commit (for example
  4,834 rows); with this build 0 of 20 did, and the table and index counts
  always matched. The same load also completed about three times as many
  commits, since a commit is now one LevelDB write instead of one per row.

## [2026-09-24] Storage Writes Get Their Own Results

### Fixed
- With `--data_dir`, the LevelDB write queue returned results through one
  FIFO shared by every writer, so a concurrent writer could return before
  its batch was written, and one writer's error could be reported to
  another. Each queued batch now carries its own result slot, and `Submit`
  returns only after that batch is written, with its own status. The worker
  and the waiting writers also use separate condition variables now. Batches
  are still written one at a time, in order.
- Tests: `PersistentStorageTest.WriteQueue_EachWriterGetsItsOwnResult`, with
  eight concurrent writers and a new test-only write hook
  (`PersistentStorage::SetWriteHookForTesting`) that fails every write to
  one table. Before the fix, one run had 55 of 64 failing writes report
  success, 20 errors reach the wrong writer, and 130 of 256 successful
  writes not yet visible when `Write` returned. It passes 10 of 10 runs now.

## [2026-09-24] Gateway Stops the Emulator on SIGINT and SIGTERM

### Fixed
- When `gateway_main` alone got `SIGINT` (for example from a process manager
  or `kill -INT`), it exited but left `emulator_main` running with its
  databases open. A restart on the same `--data_dir` then found them locked
  and listed them as unavailable (`CREATING`). The handler called
  `Process.Release()` before `Process.Kill()`, and Go refuses to signal a
  released process ("os: process already released"). Release also made the
  gateway's `cmd.Wait()` return at once, so two goroutines raced to
  `os.Exit`. `SIGTERM` wasn't handled at all, so `gateway_main` died without
  stopping `emulator_main`, and as PID 1 in Docker, `docker stop` waited out
  its grace period before killing the container.
- `gateway_main` now handles `SIGINT` and `SIGTERM` by sending `SIGTERM` to
  `emulator_main`, waiting up to 5 seconds, killing it if needed, and then
  exiting with status 0. A single goroutine handles both a signal and the
  emulator exiting on its own.
- Tests: `TestStopEmulatorStopsTheProcess` and
  `TestStopEmulatorKillsAProcessThatIgnoresSigterm` in
  `//gateway:gateway_test` (both fail with the old release-then-kill code).
  Checked end to end, signalling only the gateway process: with the
  previous build, `emulator_main` kept running after `SIGINT` and after
  `SIGTERM`, and a restart listed the database as `CREATING`; with this
  build, `emulator_main` is gone, and a restart lists it as `READY`.

## [2026-09-24] Dropping an Unavailable Database

### Fixed
- `DropDatabase` on a database that failed to restore (listed as `CREATING`)
  deleted its data with no copy, ignored its drop protection, and left it
  listed until a restart. A database created with the same name in that run
  couldn't be used. The handler assumed the database was loaded, and nothing
  ever cleared `DatabaseManager`'s unavailable mark. Now:
  - the database's folder is moved to `<data_dir>/.quarantine/` instead of
    being deleted (a maintainer decision, since nobody could inspect it
    through the API), and the emulator logs where it went;
  - drop protection saved in `metadata.json` is checked first;
  - the database disappears from `ListDatabases` and `GetDatabase` at once,
    and its name can be reused in the same run;
  - while a database is unavailable, `CreateDatabase` and `RestoreDatabase`
    with its name return `ALREADY_EXISTS`, since it's still a listed
    resource. Before, the name could be taken, which produced the unusable
    database above.
- The folder moves before the metadata save, and moves back if the save
  fails. If the emulator stops in between, the next start lists the
  database as unavailable again, and dropping it again finishes the job.
- Tests: `PersistentDatabaseDdlTest.DropUnavailableDatabaseQuarantinesItAndFreesTheName`
  and `DropUnavailableDatabaseKeepsDropProtection` in
  `frontend/handlers:databases_test`, and
  `DatabaseManagerTest.UnavailableDatabaseNameIsTakenUntilDeleted`. All fail
  before the fix. Checked end to end over REST: two corrupted databases, one
  protected. The protected one can't be dropped, and its folder stays. The
  other is quarantined, recreated under the same name, written to, and still
  there after a restart.

## [2026-09-24] IAM Policies No Longer Stop Startup

### Fixed
- With `--data_dir`, if a database failed to restore and had an IAM policy,
  startup stopped with `Persisted IAM policy references an invalid or missing
  resource`, which defeated per-database fault isolation. Restoring a policy
  checked its resource with `DatabaseManager::GetDatabase`, which rejects
  unavailable databases. An unavailable database now counts as an existing
  resource for IAM, so its policy is restored, and `GetIamPolicy`,
  `SetIamPolicy` and `TestIamPermissions` work on it.

### Changed
- A persisted IAM policy whose resource no longer exists is dropped with a
  `WARNING` (`Dropping the persisted IAM policy for <resource> ...`) instead
  of stopping the emulator, and the next metadata save removes it from disk.
  A malformed resource name still stops startup.
- The restore loop moved from `emulator_main` into
  `ServerEnv::RestoreIamPoliciesFromMetadata` so it can be tested.
- Tests: `PolicyPersistenceTest.RestoreKeepsUnavailableDatabasePoliciesAndDropsMissingOnes`
  in `frontend/handlers:policies_test` (fails before the fix with the
  `DATA_LOSS` above). Checked end to end over REST: a corrupted database with a
  policy, plus a hand-added policy for a database that doesn't exist, no
  longer stop startup; the corrupted database's policy is served and the
  orphaned one is removed from `metadata.json`.

## [2026-09-24] Quarantine No Longer Blocks the Next Start

### Fixed
- After `--repair_corrupted_databases` quarantined a database, the next start
  failed with `Persistent database root has committed data but no metadata`.
  Quarantine moved only the database's `storage/` directory and left its
  folder, with the `.metadata-committed` marker, behind, while the
  `metadata.json` entry was removed. Quarantine now moves the whole database
  folder into `<data_dir>/.quarantine/` in one rename
  (`DatabaseManager::QuarantineDatabaseDirectory`), so nothing is left behind.
  If the emulator stops between the rename and the metadata save, the next
  start lists the database as unavailable and another repair run removes it.

### Added
- `gateway_main` accepts `--repair_corrupted_databases` and passes it to
  `emulator_main`, so it works with the Docker image's usual
  `./gateway_main ...` command. Before, only `emulator_main` accepted it.
- Tests: `DatabaseManagerTest.QuarantinedDatabaseDoesNotBlockNextStartup`
  (fails before the fix with the `DATA_LOSS` above) and a new Go test,
  `//gateway:gateway_test`, for the flags the gateway forwards. Checked end to
  end over REST: a corrupted database quarantined through `gateway_main`,
  then a restart without the flag succeeds and the other database keeps
  serving; the previous build failed that restart.

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

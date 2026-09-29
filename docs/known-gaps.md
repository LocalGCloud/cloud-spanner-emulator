# Known gaps

What doesn't work in this emulator, or works differently from Cloud Spanner.
Use it with [Capabilities](capabilities.md). Per-feature status and evidence
are in the [feature coverage matrix](feature-coverage.md), whose
machine-readable form is [`feature-coverage.yaml`](feature-coverage.yaml).

Each gap says what happens and what to do about it where there's a
workaround. "(fork)" marks gaps in features this fork added; the rest are
inherited from upstream. Reviewed 2026-09-24 against `jay-spanner-extended`;
updated 2026-09-28 after the developer-usability closure and again after the
remaining-limitations batches. The evidence is recorded in the
[usability worksheet](plans/2026-09-27-usability-audit-worksheet.md): the
closure ran native unit, conformance, `--data_dir` restart and public-endpoint
SDK/driver checks; the remaining-limitations batches re-ran the combined native suite (164 of 164
targets on `bd6a9646`). On 2026-09-28, the packaged Docker image
`spanner-emulator-extended:local` was fully qualified against both the client
matrix and the 5-step image verification suite (`tests/image_verification_test.py`).

## Known bugs

The seven bugs found in the 2026-09-24 review, and a REST field-mask bug
found while fixing them, are fixed; see the [changelog](CHANGELOG.md). The
bugs found and fixed during the 2026-09-28 developer-usability closure (search
over NULL token lists, PostgreSQL JSONB operators and `regexp_replace`,
`SNIPPET` layout, `TO_BASE32`/`FROM_BASE32`, deadlock and
`LOCK_SCANNED_RANGES` locking, a change stream action-registry data race, DML
`UPDATE` change stream records, remote function request IDs, PostgreSQL
catalog views, and `ListBackups` filter precedence) are listed in the
[changelog's 2026-09-28 entry](CHANGELOG.md#2026-09-28-developer-usability-closure).

The earlier [unique-index incident](../openspec/changes/fix-unique-index-restore-isolation/tasks.md)
is fixed and closed: with `--data_dir`, range reads and range deletes could
skip rows, because range scans assumed content order while the on-disk key
order sorts a table's keys by encoded length first. Unique index checks then missed existing values, so
duplicates could commit and later break the restore. A regression test and a
concurrent stress test (which failed about half its runs before the fix and
passed 30 of 30 after) cover it; the on-disk format is unchanged. See the
[changelog's remaining-limitations entry](CHANGELOG.md#2026-09-28-remaining-limitations).

Open bugs: None currently open. The two previously documented open bugs (graph
query `RETURN ... ORDER BY` ordering ignored by GoogleSQL reference evaluator's
scrambling, and change stream microsecond boundary commit skip during interval
chopping) have been resolved and covered by regression tests.

## Transactions and concurrency

- Conflicting read-write transactions follow wound-wait, like Cloud Spanner
  (fork): a younger transaction waits up to `--lock_wait_timeout_ms` (default
  10000; `0` restores the old abort-at-once behavior) for a lock an older one
  holds and then aborts, and an older transaction aborts (wounds) a younger
  holder. Differences: a waiting request ignores its RPC deadline;
  `--abort_current_transaction_probability` (default 20%) sometimes lets a
  younger transaction abort an idle older holder instead of waiting; schema
  changes never wait (they abort the holder or themselves); and idle
  transactions aren't aborted after 10 seconds as in production. Retry
  aborted transactions.
- `REPEATABLE_READ` checks constraints (unique indexes, foreign keys) against
  the latest data rather than the snapshot, and validates write-write
  conflicts at commit. Commit history is kept only while such transactions are
  active.
- `max_commit_delay` is validated (0 to 500 ms) but doesn't delay commits.
  `CommitStats.mutation_count` follows the
  documented counting rules, but production may count some index or
  constraint effects differently.
- `--enable_fault_injection` aborts about 5% of first commit attempts.
- gRPC deadlines and cancellations are ignored, except that a read at a
  future timestamp fails with `DEADLINE_EXCEEDED` when its wait would pass
  the deadline.
- Streaming reads and queries return resume tokens only at row boundaries
  (fork); a row larger than 1 MB still spans several responses. A resumed
  stream runs the request again in the same transaction or at the same read
  timestamp and skips the rows already sent; if those rows come back
  different (a nondeterministic result, such as one using `RAND()`), it fails
  with `FAILED_PRECONDITION`. A garbled or mismatched token, or a resent
  request that begins a new transaction, fails with `INVALID_ARGUMENT`. DML
  streams carry no resume tokens.
- Reads after the tested failed DML constraint case now preserve earlier
  buffered writes, but Batch DML and backend Write failure read behavior have
  not been qualified against the full transaction error-state matrix.
- If a commit violates several constraints, the one reported may differ from
  Cloud Spanner's.

## Security and IAM

- IAM policies are stored and returned (fork) but never enforced: no
  authenticated principal reaches the emulator. `TestIamPermissions` returns
  every permission requested. `SetIamPolicy` checks supplied etags and rotates
  them on successful writes.
- Fine-grained access control (fork) is enforced only for sessions created
  with a `creator_role`; other sessions have full access. Using a role is not
  checked against the IAM `spanner.databaseRoles.use` permission.
- Privilege errors use the emulator's own messages. Graph algorithm
  permission requirements aren't enforced.
- Credentials aren't checked, and both ports serve unencrypted traffic.

## Admin API

- `ListInstances`, `ListSessions`, `ListBackups` and the database, backup,
  instance config and instance partition operation lists apply AIP-160
  filters (fork), and so does the generic `google.longrunning`
  `ListOperations`; unknown filter fields fail with `INVALID_ARGUMENT`.
  `ListDatabases` has no `filter` field.
- Long-running operations complete before the RPC returns. Cancelling does
  nothing; most operations do not report progress, while completed
  `UpdateDatabase` reports 100%. Index backfills block
  `UpdateDatabaseDdl`.
- `UpdateInstance` accepts only display name, node count, processing units and
  labels. Capacity remains metadata only.
- `INFORMATION_SCHEMA.DATABASE_OPTIONS` projects stored `default_leader`,
  `witness_location`, and `read_lease_regions`; they don't change physical
  placement.
- `encryption_config` is ignored by `CreateDatabase`. `RestoreDatabase`,
  `CreateBackup`, `CopyBackup`, and backup schedules accept the Google-default
  encryption types and report Google default encryption; customer-managed
  keys return `UNIMPLEMENTED` because local data is plaintext.
- `MoveInstance` changes metadata and returns a completed operation without a
  physical move (fork). This build's request proto lacks the currently
  documented per-database move configuration. `FetchCacheUpdate` returns no
  updates. `AddSplitPoints` validates and stores split points, which
  `SPANNER_SYS.USER_SPLIT_POINTS` shows until they expire and which persist
  with `--data_dir`, but it doesn't change how data is stored. The split key
  text in that table approximates production's format.
- Instance configs, node counts and processing units are metadata; there is
  no capacity.
- Local schema limits and the 100-databases-per-instance quota are enforced
  (`--override_max_databases_per_instance` raises the latter). Commit and
  BatchWrite reject more than 80,000 distinct explicit write cells; index,
  generated-column, delete, and DML effects are not counted. Commit-size
  limits aren't enforced, and the tables-per-database and
  indexes-per-database boundaries have no active test. Admin/API rate and
  capacity quotas are not emulated.
- `InternalUpdateGraphOperation` updates only the local operation record;
  there is no graph backfill work.

## Backups (fork)

- Backups and restores need `--data_dir`; without it they fail with
  `FAILED_PRECONDITION`.
- Each backup and copy is a full copy of the database, so disk use grows with
  each one. Incremental schedule backups are linked into chains in their
  metadata but are still full copies.
- `version_time` must be between the database's `earliest_version_time` and
  now, so the database's `version_retention_period` (default 1 hour) bounds
  how far back a backup can go.
- Expired backups become inaccessible and are deleted by backup metadata RPCs
  or the periodic schedule worker; cleanup can lag until its next pass.
- Customer-managed encryption keys return `UNIMPLEMENTED`.
- A restore must stay in the same project and use a matching instance config.
  Restores drop row deletion policies, as in production; add them again if
  needed.

## Persistence (fork)

Details and workarounds are in [Persistence](persistence.md#known-limitations).


- Row writes aren't synced to disk. Data survives a process crash but not an
  OS crash or power loss, which can lose the most recent commits (each commit
  is still all or nothing).
- The data directory lock (`<data_dir>/.lock`, fork) is an advisory `flock`:
  it stops a second emulator process, which exits at startup naming the
  holder's PID, but not other programs that write to the directory.
- `SPANNER_SYS` statistics are saved when a minute interval ends and when
  `emulator_main` gets `SIGTERM` or `SIGINT`. A crash or `SIGKILL` loses the
  statistics of the current minute. Executing queries and partitioned DMLs
  aren't saved.
- Sessions, open transactions and in-flight change stream queries don't
  persist; clients reconnect after a restart.
- A failure outside a single database (unreadable or newer `metadata.json` or
  `backup_catalog.json`, a bad backup, an instance or config restore failure)
  stops the emulator at startup.
- Downgrades aren't supported: metadata is rewritten in the newest format.
- Startup deletes unrecognized folders under `backups/` and database folders
  without a commit marker. Don't store other files in the data directory.
- Each schema change copies the whole database first, so DDL time and
  temporary disk space grow with database size.
- `.quarantine/` is never cleaned up: it keeps databases quarantined at
  startup and unavailable databases that were dropped. Old row versions are
  pruned for cells touched by a later write or delete; untouched cells may
  retain older versions on disk.

## Change streams

Details are in [Change streams](change-streams.md).

- Partitioning is simulated. Every write goes to the first active partition
  token, so extra partitions don't spread load. Tokens rotate on a timer
  (20–40 s by default), not by load.
- Change stream queries must use `ExecuteStreamingSql` in a single-use,
  strong, read-only transaction. `ExecuteSql` and `PLAN` mode are rejected.
- Queries that are open during a restart fail and must be restarted.
- Records past the retention period are deleted by the partition churner, so
  deletion lags by up to one churn pass. With
  `--enable_change_stream_churning=false` they stay on disk.
- Records differ from production in a few ways:
  `number_of_partitions_in_transaction` is always 1, and `transaction_tag` is
  propagated from user transactions and BatchWrite requests (TTL deletes carry
  `RowDeletionPolicy` with `is_system_transaction` set). `record_sequence`
  increases within a transaction, but clients should not assume mutation input
  order, which Spanner also does not guarantee.
- `MUTABLE_KEY_RANGE` `MOVE` churns keep a single child partition, and writes
  still go to one active partition.
- Setting `retention_period` to `NULL` retains its previous effective value.
  `value_capture_type = NULL` resets to the default; PostgreSQL `RESET` also
  selects the default.
- Partition rotations and retention cleanup run as transactions and can
  conflict with user transactions.
- Data directories written before 2026-09-24 saved every database's create
  time as 1970, so change streams created in `CreateDatabase` there have a
  1970 creation time after a restart, and reading them from before their real
  creation returns `OUT_OF_RANGE` with "before the first partition". Recreate
  those databases to fix it. For databases persisted before 2026-08-16
  (commit `b8bf6521`), change streams created later get the restart time as
  their creation time.

## Schema and DDL

- Row deletion policies (TTL) delete expired rows within
  `--row_deletion_policy_sweep_interval_seconds` (default 60 seconds; `0`
  disables the sweeper), much sooner than production's roughly 72 hours. A
  sweep batch runs as a system transaction and can abort a conflicting user
  transaction; retry it.
- `INFORMATION_SCHEMA` lacks `INDEX_OPTIONS`, `COLUMN_PARAMETERS` and
  `COLUMNS.IS_STORED_VOLATILE`, which exist in production but aren't
  documented.
- Locality groups are accepted with no storage effect.
- Upstream `Queue` catalog and validator schema objects are merged into this fork,
  but Cloud Spanner's DDL parser grammar does not yet expose `CREATE QUEUE` syntax.
  The `ALTER DATABASE ... SET OPTIONS (score_version = <int>)` option is supported
  in GoogleSQL.
- Foreign key backing index names generated by Cloud Spanner can be used in
  query hints. Names the emulator generates can't be used, in the emulator or
  in production.
- Geo-partitioning (fork): rows are stored locally whatever their placement,
  and routing latency, row moves and instance partition capacity aren't
  emulated. See [Placements](placements.md).

## Queries and functions

- Query plans are built from the emulator's own execution of the query.
  Operator names follow Spanner's documentation, but plan shapes, costs and
  timings don't predict production plans. `PROFILE` on DML reports only the
  root operator.
- `SPANNER_SYS` statistics (fork) are measured locally. Without `--data_dir`
  they're lost on restart. Text fingerprints differ from production, physical
  columns (memory, disk I/O) are zero, and `READ_STATS`
  `AVG_LOCKING_DELAY_SECONDS` is 0. `TABLE_SIZES_STATS_1HOUR` averages a
  logical size estimate sampled every 5 minutes (`USED_HDD_BYTES` is 0).
  `ACTIVE_PARTITIONED_DMLS` shows one partition with progress 0 until the
  statement finishes. `QUERY_PROFILES_TOP*`, `TASKS` and
  `TABLE_SIZES_STATS_PER_LOCALITY_GROUP_1HOUR` don't exist. `SPLIT_*` and
  recommendation tables are empty. Only ended intervals are shown unless
  `--spanner_sys_expose_open_interval` is set.
- SQL that GoogleSQL supports but Cloud Spanner doesn't may succeed instead of
  failing.
- Performance hints are accepted but do nothing. `OPTIMIZER_VERSION` and
  `OPTIMIZER_STATISTICS_PACKAGE` statement hints (fork) are accepted on
  queries and DML; statistics package names aren't checked.
- Queries that force a `NULL_FILTERED` index are rejected unless
  `--disable_query_null_filtered_index_check` or the matching hint is set.
- `PartitionQuery` and `PartitionRead` default to two partitions (one empty
  and one with every row) when `max_partitions <= 1`. When `max_partitions > 1`,
  `PartitionRead` slices discrete keys, key ranges, or primary key scans into up
  to `max_partitions` disjoint slices, and `PartitionQuery` returns $N$ streaming
  row-sliced tokens for multi-worker parallel execution (tested with Spark and
  Dataflow patterns). Partitioned DML runs as a single local transaction, and
  `BatchWrite` applies mutation groups one at a time.
- `TABLESAMPLE SYSTEM` isn't supported.
- Graph algorithms (fork) compute deterministic results in memory. Where
  Spanner doesn't publish an algorithm's exact method, the results are the
  emulator's own and can differ from production. `machine_category`, `zone`
  and `max_idle_time` are validated only; algorithm permission, schema and
  `RETURN`-clause rules aren't enforced; and
  `SPANNER_SYS.GRAPH_OPERATION_EXECUTION_STATUS` isn't populated. `EXPORT
  DATA` writes rows back only with `format = 'CLOUD_SPANNER'` (rows that
  violate constraints are skipped); CSV, Parquet and Avro validate their
  options and then discard the rows. A write-back inside a read-write
  transaction can conflict with that transaction's own locks; run it outside
  one.
- Without a configured remote-functions backend, `ML.PREDICT` and the `AI.*`
  functions return deterministic fake values. Configured calls use a local
  HTTP backend; Vertex AI is never called.
- `APPROX_*` and `spanner.approx_*` vector distance functions return exact
  nearest neighbors, so recall is always 100% and `num_leaves_to_search`,
  `tree_depth` and `num_leaves` have no effect. Tune recall and latency
  against Cloud Spanner.
- `SCORE` values don't match production relevance scores. `SCORE` accepts
  the `version` (no local effect) and `token_category_weights` options;
  `bigram_weight` and `idf_weight` return `UNIMPLEMENTED`.
- `DEBUG_TOKENLIST` matches the documented examples, but `TOKENLIST` values
  stored by earlier builds lack the boundary and hashtag markers it prints.
- The ICU build has no CJK dictionaries: full-text search splits Chinese,
  Japanese and Korean text only where the script changes, so pure Chinese
  text isn't segmented into words. HTML tokenization handles numeric entities and the
  253 HTML 4 named entities (not the full HTML5 table) and skips `script`
  and `style` content.
- Full-text search parameters that are accepted but ignored:
  - `enhance_query` in query functions; `language_tag` affects query casing in
    `WORDS` and `WORDS_PHRASE`, but not RQUERY parsing;
  - physical sharding from `sort_order_sharding` in `CREATE SEARCH INDEX`.
- Queries without `ORDER BY` return rows in a deliberately random order.

## PostgreSQL dialect

- Clients that speak the PostgreSQL wire protocol need
  [PGAdapter](https://github.com/GoogleCloudPlatform/pgadapter/blob/postgresql-dialect/docs/emulator.md);
  the emulator serves only the Spanner API.
- `DELETE ... USING` returns `UNIMPLEMENTED` (matches Cloud Spanner PostgreSQL
  dialect, which does not support multi-table `DELETE ... USING`; queries should
  be rewritten using `WHERE ... IN (SELECT ...)`). `WHERE CURRENT OF` returns `UNIMPLEMENTED`.
- `pg_catalog` views for objects Spanner doesn't have (for example
  `pg_matviews`, `pg_policies`) return no rows.
- `= ANY('{...}')` and `<> ALL('{...}')` array literals are supported (with
  implicit string literal coercion and explicit type casting). `= ANY` over
  `float4[]` and `float8[]` columns remains unsupported due to PostgreSQL NaN
  comparison semantics.

## Errors

- Error messages can differ from Cloud Spanner's. They aren't part of the API
  contract, so don't depend on their text.
- Error details carry only standard `google.rpc` types (fork). REST errors use
  Google's `{"error": {"code", "message", "status", "details"}}` envelope with
  the HTTP status mapped from the gRPC code (fork, 2026-09-28); messages are
  capped at 4,096 bytes.

## Operations and observability

- Audit logs, Cloud Logging and Cloud Monitoring aren't emulated.
- The packaged Docker image was fully qualified on 2026-09-28 via
  `tests/image_verification_test.py` and the client matrix (REST, Go, Node,
  Python, Java, JDBC, PGAdapter, psql). LocalCloud packaging integration
  is qualified via the same Docker image.

## Running the emulator (fork)

- The Docker image has no `ENTRYPOINT`: to pass flags, repeat the full command
  (`./gateway_main --hostname 0.0.0.0 ...`). See
  [Configuration](configuration.md#docker).
- `--abort_current_transaction_probability` and the change stream tuning
  flags (such as `--enable_change_stream_churning`) are `emulator_main`-only;
  `gateway_main` doesn't accept or forward them (it does forward
  `--lock_wait_timeout_ms`). Running `emulator_main` directly serves gRPC
  only.
- The gRPC and REST ports open only after persisted data is restored, which
  can take a while for large data directories.

## Not applicable

Production-only behavior with no meaningful local equivalent: TrueTime,
multi-region replication and consensus, automatic capacity and scaling,
availability and latency guarantees, Cloud Spanner performance, IAM permission
enforcement, admin/API rate and capacity quotas, approximate-nearest-neighbor
recall and latency tuning, customer-managed encryption, and Cloud Monitoring
and audit logs.

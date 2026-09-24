# Known gaps

What doesn't work in this emulator, or works differently from Cloud Spanner.
Use it with [Capabilities](capabilities.md). Per-feature status and evidence
are in the [feature coverage matrix](feature-coverage.md), whose
machine-readable form is [`feature-coverage.yaml`](feature-coverage.yaml).

Each gap says what happens and what to do about it where there's a
workaround. "(fork)" marks gaps in features this fork added; the rest are
inherited from upstream. Reviewed 2026-09-24 against `jay-spanner-extended`.

## Known bugs

Found during the 2026-09-24 review and not fixed yet. "Reproduced" bugs were
confirmed against a running emulator; the others come from reading the code.

| Area | Bug | Impact | Status |
|------|-----|--------|--------|
| Persistence (fork) | A commit's rows and index entries are separate LevelDB writes. | A process crash in the middle of a commit can leave part of a transaction, or an inconsistent index, on disk. | From code |
| Persistence (fork) | Results from the storage write queue aren't matched to the writer that submitted them. | Rarely, a write returns before its data is visible, or reports another write's error. The transaction lock serializes most writers. | From code |

## Transactions and concurrency

- Only one read-write transaction or schema change runs at a time. A new one
  may abort the current one (`--abort_current_transaction_probability`,
  default 20%). Wrap transactions in a retry loop, as Cloud Spanner also
  recommends.
- `SERIALIZABLE` and `REPEATABLE_READ` isolation are accepted, but everything
  runs serialized.
- `return_commit_stats` and `max_commit_delay` are ignored.
- `--enable_fault_injection` aborts about 5% of first commit attempts.
- gRPC deadlines and cancellations are ignored.
- Streaming reads and queries don't return resume tokens.
- If a commit violates several constraints, the one reported may differ from
  Cloud Spanner's.

## Security and IAM

- IAM policies are stored and returned (fork) but never enforced.
  `TestIamPermissions` returns every permission requested, and policy etags
  aren't checked.
- Fine-grained access control is parsed and ignored: `CREATE ROLE`, `GRANT`
  and `REVOKE` have no effect, and `ListDatabaseRoles` is always empty.
- Credentials aren't checked, and both ports serve unencrypted traffic.

## Admin API

- Every list RPC ignores its `filter`: instances, sessions, databases, backups,
  backup schedules and operations.
- Long-running operations complete before the RPC returns. Cancelling does
  nothing, no progress is reported, and index backfills block
  `UpdateDatabaseDdl`.
- `UpdateDatabase` accepts only `enable_drop_protection`, and `UpdateInstance`
  only display name, node count, processing units and labels.
- `GetDatabase` doesn't report `version_retention_period`,
  `earliest_version_time` or `default_leader`.
- `DeleteInstance` doesn't check its databases' drop protection.
- `encryption_config` is ignored by `CreateDatabase`, `CreateBackup` and
  `RestoreDatabase`.
- `MoveInstance` changes metadata only (fork). `FetchCacheUpdate` returns no
  updates, and `AddSplitPoints` has no effect.
- Instance configs, node counts and processing units are metadata; there is
  no capacity.
- Only the 100-databases-per-instance quota is enforced
  (`--override_max_databases_per_instance` raises it). Admin rate limits,
  mutation-count limits and commit-size limits aren't.

## Backups (fork)

- Backups and restores need `--data_dir`; without it they fail with
  `FAILED_PRECONDITION`.
- Each backup and copy is a full copy of the database, so disk use grows with
  each one.
- `version_time` isn't supported.
- `expire_time` is validated (6 hours to 366 days) but not enforced: backups
  are never deleted automatically.
- Backup schedules are stored and returned but never run.
- A restore must stay in the same project and use a matching instance config.

## Persistence (fork)

Details and workarounds are in [Persistence](persistence.md#known-limitations).


- Row writes aren't synced to disk. Data survives a process crash but not an
  OS crash or power loss.
- There's no lock on the data directory. Never point two emulator processes at
  the same directory.
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
  pruned only when a row is written again.

## Change streams

Details are in [Change streams](change-streams.md).

- Partitioning is simulated. Every write goes to the first active partition
  token, so extra partitions don't spread load. Tokens rotate on a timer
  (20–40 s by default), not by load.
- Resume tokens are placeholders. A client retry restarts the query, so
  records can repeat.
- Change stream queries must use `ExecuteStreamingSql` in a single-use,
  strong, read-only transaction. `ExecuteSql` and `PLAN` mode are rejected.
- Queries that are open during a restart fail and must be restarted.
- Change stream data isn't garbage-collected after the retention period. It's
  rejected at query time but stays on disk.
- Records differ from production in a few ways: `REPLACE` on an existing row
  emits one `INSERT` record instead of a delete and an insert, record order
  across tables in one transaction isn't kept, a DML `INSERT` writes `NULL` to
  the columns it doesn't list, `number_of_partitions_in_transaction` is always
  1, `transaction_tag` is always empty, and `is_system_transaction` is always
  false.
- `exclude_ttl_deletes` is stored but has no effect, because the emulator
  never runs TTL deletes.
- `MUTABLE_KEY_RANGE` change streams never perform the `MOVE` partition
  rotation.
- Setting an option to `NULL` doesn't reset it: GoogleSQL keeps the old value,
  and PostgreSQL ignores it (or fails, for `value_capture_type`). Use
  PostgreSQL's `RESET`, or set an explicit value.
- Partition rotations run as transactions and can abort a user transaction.
- Data directories written before 2026-09-24 saved every database's create
  time as 1970, so change streams created in `CreateDatabase` there have a
  1970 creation time after a restart, and reading them from before their real
  creation returns `OUT_OF_RANGE` with "before the first partition". Recreate
  those databases to fix it. For databases persisted before 2026-08-16
  (commit `b8bf6521`), change streams created later get the restart time as
  their creation time.

## Schema and DDL

- Row deletion policies (TTL) are accepted, but rows never expire.
- `ALTER SEARCH INDEX` isn't parsed.
- `INFORMATION_SCHEMA` lacks the role and privilege views, `ROUTINES`,
  `PARAMETERS`, `TABLE_SYNONYMS`, `INDEX_OPTIONS` and `COLUMN_PARAMETERS`.
- Locality groups are accepted with no storage effect.
- Upstream's queue schema objects and the `ALTER DATABASE ... score_version`
  option aren't in this fork yet; they arrived in upstream releases after
  2026-08-03.
- Foreign key backing index names generated by Cloud Spanner can be used in
  query hints. Names the emulator generates can't be used, in the emulator or
  in production.
- Geo-partitioning (fork): rows are stored locally whatever their placement,
  and routing latency, row moves and instance partition capacity aren't
  emulated. See [Placements](placements.md).

## Queries and functions

- `PLAN` and `PROFILE` query modes return no query plan. `PLAN` returns only
  metadata; `PROFILE` returns row counts and an execution time unrelated to
  Cloud Spanner's.
- SQL that GoogleSQL supports but Cloud Spanner doesn't may succeed instead of
  failing.
- Performance hints are accepted but do nothing. The `OPTIMIZER_VERSION` hint
  (fork) works on queries only; on DML it fails with "invalid hint".
  `OPTIMIZER_STATISTICS_PACKAGE` is rejected.
- Queries that force a `NULL_FILTERED` index are rejected unless
  `--disable_query_null_filtered_index_check` or the matching hint is set.
- `PartitionQuery` and `PartitionRead` always return two partitions: one
  empty and one with every row. Partitioned DML runs as a single local
  transaction, and `BatchWrite` applies mutation groups one at a time.
- `TABLESAMPLE SYSTEM` isn't supported.
- Graph algorithms parse and validate but always return an empty result.
- `ML.PREDICT` and the `AI.*` functions return deterministic fake values;
  Vertex AI is never called.
- `APPROX_*` vector distance functions compute exact results, and
  `num_leaves_to_search` is ignored.
- `SCORE` values don't match production relevance scores.
- `GENERATE_UUID` returns `STRING`, and the UUID functions have no tests.
- The allowlist of about 255 GoogleSQL functions hasn't been checked against
  production.
- Full-text search parameters that are accepted but ignored:
  - `language_tag` and `enhance_query`, wherever they appear;
  - `content_type` other than `text/plain` in `TOKENIZE_FULLTEXT` and
    `TOKENIZE_SUBSTRING` (not supported);
  - `token_category` and `remove_diacritics` in `TOKENIZE_FULLTEXT`;
  - `remove_diacritics` and `short_tokens_only_for_anchors` in
    `TOKENIZE_SUBSTRING`;
  - `remove_diacritics` in `TOKENIZE_NGRAMS`;
  - `options` in `SCORE`;
  - the `OPTIONS` clause of `CREATE SEARCH INDEX`.
- Queries without `ORDER BY` return rows in a deliberately random order.

## PostgreSQL dialect

- Clients that speak the PostgreSQL wire protocol need
  [PGAdapter](https://github.com/GoogleCloudPlatform/pgadapter/blob/postgresql-dialect/docs/emulator.md);
  the emulator serves only the Spanner API.
- `DELETE ... USING` and `WHERE CURRENT OF` return `UNIMPLEMENTED`.
- Several `pg_catalog` views always return no rows.

## Errors

- Error messages can differ from Cloud Spanner's. They aren't part of the API
  contract, so don't depend on their text.
- Error details carry only standard `google.rpc` types (fork). Since
  2026-09-23, REST errors keep the same status code as gRPC; before that, many
  came back as HTTP 500.

## Operations and observability

- `SPANNER_SYS` has only `SUPPORTED_OPTIMIZER_VERSIONS`; the query,
  transaction and lock statistics tables are missing.
- Audit logs, Cloud Logging and Cloud Monitoring aren't emulated.

## Running the emulator (fork)

- The Docker image has no `ENTRYPOINT`: to pass flags, repeat the full command
  (`./gateway_main --hostname 0.0.0.0 ...`). See
  [Configuration](configuration.md#docker).
- `--abort_current_transaction_probability`, `--remote_functions_host_port`
  and the change stream tuning flags (such as
  `--enable_change_stream_churning`) are `emulator_main`-only; `gateway_main`
  doesn't accept or forward them. Running `emulator_main` directly serves gRPC
  only.
- The gRPC and REST ports open only after persisted data is restored, which
  can take a while for large data directories.

## Not applicable

Production-only behavior with no meaningful local equivalent: TrueTime,
multi-region replication and consensus, automatic capacity and scaling,
availability and latency guarantees, and Cloud Spanner performance.

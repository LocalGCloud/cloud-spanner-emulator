# Capabilities

What this emulator supports, by area. "(fork)" marks what this fork added or
fixed; everything else comes from Google's emulator. Limits that matter are
noted inline, and the full list of what doesn't work is in
[Known gaps](known-gaps.md). For per-feature status with code and test
evidence, see the [feature coverage matrix](feature-coverage.md) and its
machine-readable form, [`feature-coverage.yaml`](feature-coverage.yaml).

Reviewed 2026-09-24 against `jay-spanner-extended`; updated 2026-09-28 after the
developer-usability closure ([plan](plans/2026-09-27-usability-closure-plan.md),
[worksheet](plans/2026-09-27-usability-audit-worksheet.md)) and the
remaining-limitations batches that followed it.

## At a glance

| Area | Status | More |
|------|--------|------|
| Data API: sessions, reads, SQL, DML, mutations | Supported | [Data API](#data-api) |
| Transactions, including `REPEATABLE_READ` (fork) | Supported; wound-wait lock waits (fork) | [Transactions](#transactions) |
| Schema and DDL, both dialects | Supported | [Schema and DDL](#schema-and-ddl) |
| GoogleSQL and PostgreSQL queries, query plans, graph algorithms (fork) | Supported; plans and some algorithm results are emulator-defined | [Queries](#queries-and-functions) |
| Instance and database admin | Supported; capacity is metadata only | [Admin API](#admin-api) |
| Persistence with `--data_dir` (fork) | Supported; row writes aren't synced to disk | [Persistence](persistence.md) |
| Backups (fork) | Supported with `--data_dir`, including `version_time`; every backup is a full copy | [Backups](#backups-fork) |
| Change streams | Supported; partitioning simulated | [Change streams](change-streams.md) |
| Geo-partitioning (fork) | Supported; storage is local | [Placements](placements.md) |
| Database roles and fine-grained access control (fork) | Supported; IAM itself is not enforced | [Security](#security-fork) |
| `SPANNER_SYS` statistics (fork) | Supported; measured locally, persisted with `--data_dir` | [Queries](#queries-and-functions) |
| REST gateway | Supported; Google JSON error envelope (fork) | [REST gateway](#rest-gateway) |

## Data API

- **Sessions**: create, batch create, get, list and delete, plus multiplexed
  sessions. Sessions don't survive a restart.
- **Reads**: `Read` and `StreamingRead` by key, key range or index, with
  strong and stale (exact or bounded staleness) timestamps.
- **SQL**: `ExecuteSql` and `ExecuteStreamingSql` in both dialects, with
  parameters. `PLAN`, `PROFILE`, `WITH_STATS` and `WITH_PLAN_AND_STATS` return
  a plan tree built from the query and local execution statistics (fork).
- **Resume tokens** (fork): `StreamingRead` and `ExecuteStreamingSql` send
  whole rows in responses of up to 1 MB (a larger row is split) with a resume
  token at each row boundary, and a request resent with the token continues
  after that row, in the same transaction or at the same read timestamp. A
  garbled token, a token from another request, or a resent request that
  begins a new transaction is `INVALID_ARGUMENT`, and rows that changed since
  they were sent (for example with `RAND()`) are `FAILED_PRECONDITION`. DML
  streams carry no tokens.
- **Writes**: mutations (insert, update, insert-or-update, replace, delete),
  DML including `THEN RETURN` and upsert DML, batch DML with sequence numbers,
  and `BatchWrite`.
- **Partitioned operations**: `PartitionRead`, `PartitionQuery` and
  partitioned DML. Partitioned DML works for parameterized PostgreSQL
  statements too (fork).
- **Directed reads and Data Boost**: request options are validated and
  otherwise ignored.

## Transactions

- Read-write, read-only and single-use transactions, inline begin, and commit
  timestamps (`PENDING_COMMIT_TIMESTAMP()`).
- Read-write transactions can overlap. Conflicts follow Cloud Spanner's
  wound-wait scheme (fork): a younger transaction waits up to
  `--lock_wait_timeout_ms` (default 10 seconds) for a lock that an older one
  holds, and an older transaction aborts a younger holder. `LOCK_STATS`
  report the measured wait time. A schema change can abort an active
  transaction. In a two-key deadlock exactly one transaction aborts (fork).
- `SERIALIZABLE` (default) and `REPEATABLE_READ` (fork). `REPEATABLE_READ`
  reads from a snapshot taken at the first read, takes no shared read locks,
  and aborts the commit with `ABORTED` if another transaction wrote the same
  keys (or `FOR UPDATE` ranges) since that snapshot.
- `SELECT ... FOR UPDATE` and the `LOCK_SCANNED_RANGES=exclusive` hint take
  exclusive locks (fork).
- `return_commit_stats` returns `CommitStats.mutation_count` (fork).
  `max_commit_delay` is validated (0 to 500 ms) and has no other effect
  (fork).
- A read at a future timestamp waits for it, and fails with
  `DEADLINE_EXCEEDED` at once if the wait would pass the request deadline
  (fork).
- Unique indexes are re-checked at commit (fork).
- `--enable_fault_injection` randomly aborts commits, to test retry logic.

## Schema and DDL

Both GoogleSQL and PostgreSQL DDL, through `CreateDatabase` and
`UpdateDatabaseDdl`, with `GetDatabaseDdl` and `INFORMATION_SCHEMA`
(`pg_catalog` too for PostgreSQL, partly):

- tables, `ALTER TABLE`, interleaving (`INTERLEAVE IN PARENT` and
  `INTERLEAVE IN`, with `ON DELETE`), and tables without primary keys (system-assigned hidden `_rowid` key);
- secondary indexes, including unique, `NULL_FILTERED` and `STORING` (disallows combining `NULL_FILTERED` with a `WHERE` clause);
- foreign keys, check constraints, generated columns, hidden columns and
  column defaults;
- sequences, `IDENTITY` columns and generated primary keys. With
  `--data_dir`, sequences continue after a restart or backup restore instead
  of repeating values; a restart can skip up to 1,000 counter values (fork);
- views, named schemas, synonyms and SQL user-defined functions;
- search indexes, `TOKENLIST` columns and vector indexes (validated; searches
  are exact);
- property graphs, ML models and proto bundles;
- change streams ([guide](change-streams.md));
- placements and placement keys (fork; [guide](placements.md));
- `ALTER SEARCH INDEX` column changes in both dialects, and PostgreSQL vector
  indexes (`CREATE INDEX ... USING ScaNN`) (fork). PostgreSQL `GetDatabaseDdl`
  prints search indexes as `CREATE SEARCH INDEX` and vector indexes as
  `CREATE INDEX ... USING scann`, so the output re-parses (fork);
- database options (`version_retention_period`, `default_time_zone`,
  `default_sequence_kind`, `score_version`; placement options are metadata);
- row deletion policies (TTL): a background sweeper deletes expired rows
  every `--row_deletion_policy_sweep_interval_seconds` (default 60) (fork);
- roles, `GRANT` and `REVOKE` in both dialects (fork; see
  [Security](#security-fork)).

Accepted without effect: locality groups (storage tiering is physical).

## Types

`BOOL`, `INT64`, `FLOAT32`, `FLOAT64`, `NUMERIC`, `STRING`, `BYTES`, `DATE`,
`TIMESTAMP`, `JSON`, `ARRAY`, `STRUCT` (in queries), `PROTO` and `ENUM`,
`TOKENLIST`, `UUID`, and `INTERVAL` (query-only). The PostgreSQL dialect adds its
equivalents, including `jsonb`, `numeric` and `oid`.

UUID key and value mutations, keyed reads, casts, and typed SQL results are
tested through the public API in both dialects. PostgreSQL
`DEFAULT gen_random_uuid()` and `INSERT ... RETURNING`, UUID change-stream
records, and native `--data_dir` process restart are tested. Packaged
LocalCloud behavior has not been qualified by these native tests.

`jsonb` accepts numbers up to Spanner's 4,932 integer digits on every
platform, including native macOS builds (fork).

## Queries and functions

SQL runs on the GoogleSQL reference implementation. Tested functions include:

- every Spanner-documented GoogleSQL function, including `TO_BASE32`,
  `FROM_BASE32`, the `ZSTD_*` functions, `SPLIT_SUBSTR` and the
  `LCASE`/`UCASE`/`ADDDATE`/`SUBDATE` aliases (fork). An official
  doc-example run (1,165 examples) found no emulator defect apart from the
  `DEBUG_TOKENLIST` output format, which now matches both documented examples
  (fork);
- full-text search in both dialects: `TOKENIZE_*`, `TOKEN`,
  `TOKENLIST_CONCAT`, `SEARCH`, `SEARCH_SUBSTRING`, `SEARCH_NGRAMS`,
  `SNIPPET`, `SCORE`, `SCORE_NGRAMS` and `DEBUG_TOKENLIST` (scores are local
  and deterministic, not production relevance values). Fork additions:
  Unicode terms in RQUERY, HTML 4 entities and skipped `script`/`style`
  content, `short_tokens_only_for_anchors`, diacritic-insensitive
  `TOKENLIST_CONCAT` of mixed `remove_diacritics` settings, French
  `language_tag` behavior as documented, `SCORE` `version` (no local effect)
  and `token_category_weights` options, which PostgreSQL `spanner.score` now
  applies too, the `SCORE_NGRAMS` `array_aggregator` argument (GoogleSQL),
  PostgreSQL `spanner.tokenize_fulltext` `remove_diacritics`, and PostgreSQL
  `spanner.tokenize_substring` with `relative_search_types` (which used to
  crash) and `support_relative_search`;
- `SOUNDEX`, `SAFE_DIVIDE`, `NORMALIZE`, named arguments (`=>`) and RE2
  regular expressions;
- exact cosine, Euclidean, and dot-product vector functions, and the
  approximate `APPROX_*` (GoogleSQL) and `spanner.approx_*` (PostgreSQL, fork)
  functions, which return exact nearest neighbors;
- `TABLESAMPLE` with `BERNOULLI` and `RESERVOIR` in both dialects (GoogleSQL supports `REPEATABLE`; PostgreSQL supports `BERNOULLI` and `SPANNER.RESERVOIR`);
- hints, including `FORCE_INDEX` and the `OPTIMIZER_VERSION` and
  `OPTIMIZER_STATISTICS_PACKAGE` statement hints on queries and DML (fork);
- graph queries (GQL) and graph algorithms (fork): `PageRank` (including
  personalized), `BetweennessCentrality`, `ClosenessCentrality`,
  `WeaklyConnectedComponents`, `ModularityClustering`,
  `CorrelationClustering`, `LabelPropagation`, `CliqueFinding`,
  `JaccardSimilarity`, `CosineSimilarity`, `CommonNeighborsSimilarity`,
  `TotalNeighborsSimilarity` and `ShortestPath` (path and cost), called with
  `GRAPH g CALL ... YIELD`, or with `CALL PER ()` over a preceding query such
  as a `FULL UNION ALL` of nodes and edges, with the documented arguments;
  `ELEMENT_DEFINITION_NAME` works in the calling statement. The documented
  PageRank example returns the documented results. `EXPORT DATA` with
  `format = 'CLOUD_SPANNER'` writes the results back to Spanner tables
  (`update_ignore_all` or `upsert_ignore_all`; rows that violate constraints
  are skipped);
- `ML.PREDICT` and `AI.*` functions; unconfigured calls return deterministic
  placeholders, while configured calls use the local HTTP backend;
- remote functions, which return placeholder values unless
  `--remote_functions_host_port` points at a local HTTP backend. Both
  `gateway_main` and `emulator_main` accept the flag;
- `SPANNER_SYS` statistics measured per database (fork): query, read,
  transaction and lock statistics (TOP and TOTAL for 1 minute, 10 minutes and
  1 hour), table and column operation statistics, `TABLE_SIZES_STATS_1HOUR`
  (a logical estimate), `OLDEST_ACTIVE_QUERIES`, `ACTIVE_QUERIES_SUMMARY`,
  `ACTIVE_PARTITIONED_DMLS`, `ROW_DELETION_POLICIES` and
  `USER_SPLIT_POINTS`, in both dialects. With `--data_dir` the statistics
  survive restarts. Like production, only ended intervals are shown unless
  `--spanner_sys_expose_open_interval` is set.

PostgreSQL-dialect databases accept PostgreSQL queries, DML and DDL, including
`generate_series`, `make_interval`, `ILIKE`, `!~~`, `spanner.split_substr` and
JSONB operators as documented (fork).
PostgreSQL drivers and tools connect through
[PGAdapter](https://github.com/GoogleCloudPlatform/pgadapter/blob/postgresql-dialect/docs/emulator.md).

## Admin API

- **Instances**: create, get, list, update and delete; instance configs,
  including custom configs (fork); instance partitions; `MoveInstance`
  (metadata only, fork).
- **Databases**: create, get, list, drop, `UpdateDatabaseDdl`,
  `GetDatabaseDdl`, `UpdateDatabase` with drop protection (including instance
  deletion, fork), and
  `ListDatabaseOperations` (fork).
- **Operations**: long-running operations for every admin call that returns
  one. They complete before the RPC returns.
- **Split points** (fork): `AddSplitPoints` validates requests and stores
  split points, shown in `SPANNER_SYS.USER_SPLIT_POINTS` until they expire and
  persisted with `--data_dir`. They don't change how data is stored.
- **List filters** (fork): `ListInstances`, `ListSessions`, `ListBackups`, the
  generic `ListOperations`, and the database, backup, instance config and
  instance partition operation lists apply AIP-160 filters (`OR` binds tighter
  than `AND`; unknown fields are rejected).
- **IAM**: `GetIamPolicy`, `SetIamPolicy` and `TestIamPermissions` on
  instances, databases, backups and more. Policies are stored and persisted
  (fork); supplied etags guard updates, but permissions are never enforced.
- **Quotas**: local schema limits, 100 databases per instance (raised with
  `--override_max_databases_per_instance`), and Commit and BatchWrite reject
  more than 80,000 distinct explicit write cells. Rate and capacity quotas are
  not emulated.

## Backups (fork)

Require `--data_dir`. See [Persistence](persistence.md#backups).

- `CreateBackup`, `CopyBackup`, `GetBackup`, `ListBackups`, `UpdateBackup`
  (expire time only), `DeleteBackup` and `RestoreDatabase`, with
  `ListBackupOperations`.
- `version_time` backups: any time from the database's
  `earliest_version_time` to now; the backup holds the rows and schema as of
  that time.
- Google-default encryption types are accepted and reported in
  `encryption_info`; customer-managed keys return `UNIMPLEMENTED`.
- `ListBackups` supports filtering and newest-first pagination. Expired backups
  are cleaned up by backup metadata RPCs and the schedule worker.
- Each backup is a full copy. Restores drop row deletion policies, as in
  production.
- Backup schedules can be created, read, listed, updated and deleted. With
  `--data_dir`, UTC 12-hour, daily, weekly and monthly schedules create backups
  once per due interval and persist their cursor through restart. Incremental
  schedules link their backups into chains (`incremental_backup_chain_id`,
  `oldest_version_time`), but each backup is physically full. At most 4
  schedules per database.

## Persistence (fork)

With `--data_dir`, rows, schema (including roles, grants and row deletion
policies), sequence counters, split points, instances, instance configs,
instance partitions, IAM policies, long-running operations, backups, backup
schedules and `SPANNER_SYS` statistics survive restarts. Each commit is
written to disk all or nothing. The directory is locked, so a second emulator
process on it fails at startup (fork). Schema changes replay at their original timestamps,
interrupted schema changes are finished or rolled back at startup, and a
database that fails to restore is marked unavailable instead of stopping the
emulator; it can be dropped, or quarantined with
`--repair_corrupted_databases`. See [Persistence](persistence.md), including
its known limitations.

## Change streams

Both dialects, with all `value_capture_type` settings, `FOR ALL`, table and
column tracking, retention from 1 to 7 days, `MUTABLE_KEY_RANGE` partition
mode with `MOVE` partition events (fork), transaction exclusion, and TTL
deletes tagged as system transactions that `exclude_ttl_deletes` can skip
(fork). DML `UPDATE` records only the columns it sets (fork). Queries follow the production flow of
partition tokens and child partitions, and return resume tokens (partition,
commit timestamp and record index) that a retried query resumes from (fork).
Records past the retention period are deleted (fork). With `--data_dir`,
definitions, records, partition history and creation times survive restarts,
and reads can start before a restart (fork). See [Change streams](change-streams.md)
for limits and differences from production.

## REST gateway

REST for the Spanner, Database Admin, Instance Admin and Operations APIs,
including instance partition operations (fork). Errors use Google's JSON
envelope, `{"error": {"code": <HTTP status>, "message": ..., "status":
"ALREADY_EXISTS", "details": [...]}}`, with the HTTP status mapped from the
gRPC code and standard `google.rpc` details preserved, including PostgreSQL
SQLSTATE as `ErrorInfo` (fork). Field masks accept the JSON form
(`?updateMask=enableDropProtection`) as well as proto field names (fork).

## Clients and tools

- **Client libraries** connect with `SPANNER_EMULATOR_HOST`. The conformance
  suite uses the C++ client. The public-endpoint client matrix
  ([`tests/client_matrix`](../tests/client_matrix/)) was verified 100% against
  both native builds and the packaged Docker image
  `spanner-emulator-extended:local` on 2026-09-28 with Python
  google-cloud-spanner 3.71.0, Go 1.95.1, Node 9.0.0, and Java 6.123.0.
- **gcloud** works against the REST port through
  `api_endpoint_overrides/spanner`, and is tested for instance, database, DDL,
  operation and read/write commands.
- **JDBC and PGAdapter**: the Spanner JDBC driver 2.45.0 (both dialects) and
  PGAdapter 0.55.3 with pgJDBC 42.7.13 and psql 17.5 passed the same matrix,
  including verification against the packaged Docker container.

## Security (fork)

- Database roles and fine-grained access control in both dialects: `CREATE
  ROLE`, `DROP ROLE`, `GRANT` and `REVOKE` on tables (with column lists),
  views, change streams and their read functions, sequences, models and
  schemas, plus role membership. Grants are printed by `GetDatabaseDdl`,
  persist with `--data_dir` and backups, and are revoked when an object is
  dropped.
- `ListDatabaseRoles` lists created roles and the system roles `public`,
  `spanner_info_reader` and `spanner_sys_reader`.
- A session created with `creator_role` is restricted on queries, DML, reads,
  mutations and change stream reads; `SQL SECURITY DEFINER` views need only
  `SELECT` on the view, while `SQL SECURITY INVOKER` view bodies are checked
  at analysis, including in `PLAN` mode. Sequences used by column defaults
  need their privilege. `INFORMATION_SCHEMA` role and privilege views and
  `pg_catalog` filter rows by the session's role.
- IAM is not enforced: no authenticated principal reaches the emulator.

## Builds and distribution (fork)

- Multi-arch Docker images (`linux/amd64`, `linux/arm64`) on Docker Hub, tagged
  by commit.
- Native macOS arm64 builds and archives.
- Cached local builds with `build.sh`. See [Building](building.md).
- Packaged Docker image fully qualified across client SDKs, volume persistence,
  and extended feature spot checks via `tests/image_verification_test.py`.

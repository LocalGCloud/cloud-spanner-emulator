# Capabilities

What this emulator supports, by area. "(fork)" marks what this fork added or
fixed; everything else comes from Google's emulator. Limits that matter are
noted inline, and the full list of what doesn't work is in
[Known gaps](known-gaps.md). For per-feature status with code and test
evidence, see the [feature coverage matrix](feature-coverage.md) and its
machine-readable form, [`feature-coverage.yaml`](feature-coverage.yaml).

Reviewed 2026-09-24 against `jay-spanner-extended`.

## At a glance

| Area | Status | More |
|------|--------|------|
| Data API: sessions, reads, SQL, DML, mutations | Supported | [Data API](#data-api) |
| Transactions | Supported; one read-write transaction at a time | [Transactions](#transactions) |
| Schema and DDL, both dialects | Supported | [Schema and DDL](#schema-and-ddl) |
| GoogleSQL and PostgreSQL queries | Supported, with some functions stubbed | [Queries](#queries-and-functions) |
| Instance and database admin | Supported; capacity is metadata only | [Admin API](#admin-api) |
| Persistence with `--data_dir` (fork) | Supported, with known bugs | [Persistence](persistence.md) |
| Backups (fork) | Supported with `--data_dir`; schedules don't run | [Backups](#backups-fork) |
| Change streams | Supported; partitioning simulated | [Change streams](change-streams.md) |
| Geo-partitioning (fork) | Supported; storage is local | [Placements](placements.md) |
| IAM | Policies stored (fork), not enforced | [Admin API](#admin-api) |
| REST gateway | Supported; errors keep their status (fork) | [REST gateway](#rest-gateway) |

## Data API

- **Sessions**: create, batch create, get, list and delete, plus multiplexed
  sessions. Sessions don't survive a restart.
- **Reads**: `Read` and `StreamingRead` by key, key range or index, with
  strong and stale (exact or bounded staleness) timestamps.
- **SQL**: `ExecuteSql` and `ExecuteStreamingSql` in both dialects, with
  parameters. `PLAN` and `PROFILE` modes run but return no query plan.
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
- Only one read-write transaction or schema change runs at a time; a new one
  may abort the current one. `SERIALIZABLE` and `REPEATABLE_READ` are
  accepted but run serialized.
- Unique indexes are re-checked at commit, so a duplicate can't reach storage
  through a race between write and commit (fork).
- `--enable_fault_injection` randomly aborts commits, to test retry logic.

## Schema and DDL

Both GoogleSQL and PostgreSQL DDL, through `CreateDatabase` and
`UpdateDatabaseDdl`, with `GetDatabaseDdl` and `INFORMATION_SCHEMA`
(`pg_catalog` too for PostgreSQL, partly):

- tables, `ALTER TABLE`, interleaving (`INTERLEAVE IN PARENT` and
  `INTERLEAVE IN`, with `ON DELETE`);
- secondary indexes, including unique, `NULL_FILTERED` and `STORING`;
- foreign keys, check constraints, generated columns, hidden columns and
  column defaults;
- sequences, `IDENTITY` columns and generated primary keys (positions don't
  survive a restart; see [Known gaps](known-gaps.md#known-bugs));
- views, named schemas, synonyms and SQL user-defined functions;
- search indexes, `TOKENLIST` columns and vector indexes (validated; searches
  are exact);
- property graphs, ML models and proto bundles;
- change streams ([guide](change-streams.md));
- placements and placement keys (fork; [guide](placements.md));
- database options.

Accepted without effect: row deletion policies (TTL), locality groups,
`CREATE ROLE`, `GRANT` and `REVOKE`.

## Types

`BOOL`, `INT64`, `FLOAT32`, `FLOAT64`, `NUMERIC`, `STRING`, `BYTES`, `DATE`,
`TIMESTAMP`, `JSON`, `ARRAY`, `STRUCT` (in queries), `PROTO` and `ENUM`,
`TOKENLIST`, and, partly, `UUID` and `INTERVAL` (query-only). The PostgreSQL dialect adds its
equivalents, including `jsonb`, `numeric` and `oid`.

`jsonb` accepts numbers up to Spanner's 4,932 integer digits on every
platform, including native macOS builds (fork).

## Queries and functions

SQL runs on the GoogleSQL reference implementation, so most GoogleSQL
functions work, including:

- full-text search: `TOKENIZE_FULLTEXT`, `TOKENIZE_SUBSTRING`,
  `TOKENIZE_NGRAMS`, `SEARCH`, `SEARCH_NGRAMS`, `SCORE`, `SCORE_NGRAMS`
  (several parameters are accepted but ignored);
- `SOUNDEX`, `SAFE_DIVIDE`, `NORMALIZE`, named arguments (`=>`) and RE2
  regular expressions;
- vector distance functions (`APPROX_*` compute exact results);
- `TABLESAMPLE` with `BERNOULLI` and `RESERVOIR`, including `REPEATABLE`
  (fork);
- hints, including `FORCE_INDEX` and the `OPTIMIZER_VERSION` statement hint on
  queries (fork);
- graph queries (GQL). Graph algorithms validate but return no rows;
- `ML.PREDICT` and `AI.*` functions, which return fake values;
- remote functions, which return placeholder values unless
  `--remote_functions_host_port` (`emulator_main` only) points at a local HTTP
  backend.

PostgreSQL-dialect databases accept PostgreSQL queries, DML and DDL.
PostgreSQL drivers and tools connect through
[PGAdapter](https://github.com/GoogleCloudPlatform/pgadapter/blob/postgresql-dialect/docs/emulator.md).

## Admin API

- **Instances**: create, get, list, update and delete; instance configs,
  including custom configs (fork); instance partitions; `MoveInstance`
  (metadata only, fork).
- **Databases**: create, get, list, drop, `UpdateDatabaseDdl`,
  `GetDatabaseDdl`, `UpdateDatabase` with drop protection (fork), and
  `ListDatabaseOperations` (fork).
- **Operations**: long-running operations for every admin call that returns
  one. They complete before the RPC returns.
- **IAM**: `GetIamPolicy`, `SetIamPolicy` and `TestIamPermissions` on
  instances, databases, backups and more. Policies are stored and persisted
  (fork) but never enforced.
- **Quotas**: 100 databases per instance, raised with
  `--override_max_databases_per_instance`.

## Backups (fork)

Require `--data_dir`. See [Persistence](persistence.md#backups).

- `CreateBackup`, `CopyBackup`, `GetBackup`, `ListBackups`, `UpdateBackup`
  (expire time only), `DeleteBackup` and `RestoreDatabase`, with
  `ListBackupOperations`.
- Each backup is a full copy, taken at a single point in time.
- Backup schedules can be created, read, listed, updated and deleted, but
  never run.

## Persistence (fork)

With `--data_dir`, rows (every version), schema, instances, instance configs,
instance partitions, IAM policies, long-running operations, backups and
backup schedules survive restarts. Schema changes replay at their original
timestamps, interrupted schema changes are finished or rolled back at
startup, and a database that fails to restore is marked unavailable instead
of stopping the emulator. See [Persistence](persistence.md), including its
known limitations.

## Change streams

Both dialects, with all `value_capture_type` settings, `FOR ALL`, table and
column tracking, retention from 1 to 7 days, `MUTABLE_KEY_RANGE` partition
mode, and transaction exclusion. Queries follow the production flow of
partition tokens and child partitions. With `--data_dir`, definitions,
records and partition history survive restarts, and reads can start before a
restart (fork), except for streams created in `CreateDatabase`, which lose
their creation time ([known bug](known-gaps.md#known-bugs)). See [Change streams](change-streams.md)
for limits and differences from production.

## REST gateway

REST for the Spanner, Database Admin, Instance Admin and Operations APIs,
including instance partition operations (fork). Errors keep their gRPC status
code and carry only standard `google.rpc` details (fork); before 2026-09-23
many came back as HTTP 500.

## Clients and tools

- **Client libraries** connect with `SPANNER_EMULATOR_HOST` (see the
  [README FAQ](../README.md#faq) for minimum versions). This repository tests
  the C++ client; other languages aren't tested here.
- **gcloud** works against the REST port through
  `api_endpoint_overrides/spanner`, and is tested for instance, database, DDL,
  operation and read/write commands.
- **PGAdapter** connects PostgreSQL drivers and tools. JDBC and PGAdapter
  aren't tested in this repository.

## Builds and distribution (fork)

- Multi-arch Docker images (`linux/amd64`, `linux/arm64`) on Docker Hub, tagged
  by commit.
- Native macOS arm64 builds and archives.
- Cached local builds with `build.sh`. See [Building](building.md).

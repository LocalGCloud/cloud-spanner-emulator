# Developer-usability closure plan for open Spanner features

**Date:** 2026-09-27, branch `jay-spanner-extended` (dirty tree on top of `8f82fa5e`).
**Inputs:** `docs/feature-coverage-independent-audit-prompt.md`, `docs/feature-coverage.yaml`,
the 2026-09-27 audit worksheet (`docs/plans/2026-09-27-usability-audit-worksheet.md`), and
official Spanner documentation retrieved 2026-09-27 (docs.cloud.google.com; pages marked
"Last updated 2026-09-24").

**Status:** Implemented 2026-09-28; 155 records = 137 supported / 9 accepted-no-op /
9 not-applicable. Results are in the worksheet's "Post-implementation results" section.
A follow-up remaining-limitations round the same day (native suite 164/164 on `bd6a9646`;
client matrix and restart probe not re-run; packaged image not qualified) made
`graph.algorithms` supported: 138 supported / 8 accepted-no-op / 9 not-applicable. See the
worksheet's "Remaining-limitations round (2026-09-28)" section.

## Support definition used by this plan

The maintainer's definition: **a feature is `supported` when a developer can use it locally
and get correct, documented, observable behavior through the public gRPC/REST API** (both
dialects where Spanner offers both), backed by native test evidence. Consequences:

- Physical production properties (latency, throughput, replica placement, distributed
  partitioning, capacity, Google's private relevance/ANN internals, Cloud Monitoring export)
  do not keep a row partial. Where a physical property is a distinct capability, it is a
  separate `not-applicable` record so it stays visible.
- A parser accepting syntax, a stored option with no effect, or a fake result is not support
  when the documented behavior is locally meaningful (for example GRANT, REPEATABLE_READ,
  SPANNER_SYS statistics, query plans, `version_time` backups).
- Evidence tiers stay explicit. Native focused tests, the full native suite, and a disposable
  native process restart are the tiers this plan can produce. Packaged LocalCloud image
  qualification belongs to the LocalCloud project and is recorded separately, never implied.

## Build and test discipline

- Bazel always runs through the fixed-environment wrapper (fixed `PATH`, `CC`, `CXX`,
  `PROTOC`, `-c opt`, the documented macOS flags, `--distdir`/`--repository_cache` =
  `bazel-distdir`, shared `--disk_cache`). The repository `.bazelrc` is not
  `strict_action_env`, so an agent shell's PATH otherwise changes every C++ action key and
  forces a cold GoogleSQL rebuild. One Bazel invocation at a time on the shared tree.
- Each task: failing focused test first where practical, implementation, focused green run,
  then YAML/docs update. After each batch: the affected module targets. At the end: the full
  native suite (`//tests/conformance/... //backend/... //frontend/... //gateway/... //common/...`)
  and `python3 tools/feature_coverage.py audit-rpcs|generate|check` plus
  `python3 -m unittest tools.feature_coverage_test`.
- Stateful features get a disposable `--data_dir` stop/restart probe against a built
  `emulator_main` on disposable ports.

## Batches

Order follows dependencies: SQL/function fixes and small API gaps first, then transaction
semantics, then roles (which INFORMATION_SCHEMA filtering, DEFINER views and SPANNER_SYS
access depend on), then statistics/plans, then streams/backups/TTL, then filters and docs.

### Batch A — SQL functions, catalogs, and small API gaps

| Task | Rows closed or corrected | Change | Test |
| --- | --- | --- | --- |
| A1 | `googlesql.string_functions` | Implement `TO_BASE32`/`FROM_BASE32` as emulator functions (GoogleSQL's open-source evaluator lacks them); allowlist `SPLIT_SUBSTR`; enable `FEATURE_ALIASES_FOR_STRING_AND_DATE_FUNCTIONS` for `LCASE`/`UCASE`/`ADDDATE`/`SUBDATE`; implement `ZSTD_COMPRESS`/`ZSTD_DECOMPRESS_TO_BYTES`/`ZSTD_DECOMPRESS_TO_STRING` with the bundled zstd | query_engine_test + conformance value cases |
| A2 | `googlesql.aggregate_functions`, `googlesql.date_timestamp_functions`, `types.json`, `googlesql.string_functions` | Table-driven "every Spanner-documented function evaluates to the documented value" conformance test (GoogleSQL), including JSON null vs SQL NULL, empty/NULL aggregates, Unicode/bytes/regex, ISO week/DST/time zone, interval arithmetic | conformance |
| A3 | `postgresql.queries` | Map `generate_series` to `pg.generate_array`; add `spanner.split_substr`; `!~~`; `NOT LIKE/NOT ILIKE ... ESCAPE`; `array_agg(uuid)`; verify float `sum`/`avg` empty type fix | pg_functions_test |
| A4 | `postgresql.jsonb` | Conformance cases for JSONB write functions/operators and `spanner.*_array` extractors, including documented error cases | pg_functions_test |
| A5 | `search.search_functions`, `search.tokenization`, `search.scoring` | Run `search_test.cc` in both dialects; fix PostgreSQL failures | search conformance |
| A6 | `search.search_indexes` | `ALTER SEARCH INDEX {ADD|DROP} [STORED] COLUMN` (GoogleSQL) and `{ADD|DROP} [INCLUDE] COLUMN` (PostgreSQL); reject interleaved search index without `sort_order_sharding=true` | parser, updater, conformance |
| A7 | `vector.ann_indexes` (PostgreSQL) | PostgreSQL `CREATE INDEX ... USING ScaNN` and `spanner.approx_*` functions over the same exact-scan executor | pg ann conformance |
| A8 | `postgresql.pg_catalog`, `metadata.pg_catalog` | Fix `pg_proc.proname` for UDFs in named schemas; enable `PGProc_UDFs` with documented no-content columns | pg_catalog_test |
| A9 | `clients.rest_gateway` | REST errors in Google's JSON error shape (`{"error":{"code":<http>,"message","status","details"}}`) | gateway_test |
| A10 | `transactions.lifecycle` (caveat) | `return_commit_stats` → `CommitStats.mutation_count` per the documented counting rules | transactions_test |
| A11 | `database.internal_graph_operation` | Success status completes the operation | database_extensions_test |
| A12 | `postgresql.views`, `schema.views` | PostgreSQL `SQL SECURITY DEFINER` translation (effect lands with Batch C) | ddl_test, pg_views_test |

### Batch B — transaction semantics

| Task | Rows | Change | Test |
| --- | --- | --- | --- |
| B1 | `transactions.isolation_levels` | Plumb `isolation_level`/`read_lock_mode` to `ReadWriteOptions`. REPEATABLE_READ: lazily chosen snapshot, reads at the snapshot without shared locks, own writes still visible, constraint checks still serializable, commit-time write-write validation against a per-database committed-key history → ABORTED | read_write_transaction_test + conformance (lost update aborts, write skew allowed, snapshot stable) |
| B2 | `transactions.select_for_update` | `lock_scanned_ranges=exclusive` takes exclusive locks like FOR UPDATE; broad scan vs DML conflict case | queryable_table_test, conformance |
| B3 | `transactions.concurrency_model` | Two-key deadlock (wound-wait) and separate-thread writer tests | read_write_transaction_test |
| B4 | `transactions.stale_reads` | Future exact-timestamp wait bounded by the gRPC deadline | reads/queries handler tests |

### Batch C — database roles and fine-grained access control

| Task | Rows | Change | Test |
| --- | --- | --- | --- |
| C1 | `security.database_roles`, `security.fine_grained_access` | Persist GRANT/REVOKE (table/column/view/change stream/function/sequence/model/schema privileges, role membership) in both dialects; `DROP ROLE` rules; system roles (`public`, `spanner_info_reader`, `spanner_sys_reader`); GetDatabaseDdl and replay print GRANTs; `ListDatabaseRoles` includes system roles | schema_updater tests, ddl_test, handler, restart probe |
| C2 | same | `creator_role` on CreateSession/BatchCreateSessions/multiplexed; `Role not found` PERMISSION_DENIED; enforcement for queries, DML, Read, mutations, change-stream reads; DEFINER vs INVOKER views | new two-dialect conformance suite |
| C3 | `metadata.information_schema`, `metadata.database_roles_metadata` | Add ROLES, ROLE_GRANTEES, TABLE/COLUMN/CHANGE_STREAM/ROUTINE/MODEL privilege and ROLE_*_GRANTS views, ROUTINES, PARAMETERS, ROUTINE_OPTIONS, TABLE_SYNONYMS (PostgreSQL: enabled_roles, applicable_roles, routines, parameters); documented role filtering; fix PostgreSQL `locality_group_options` CSV | information_schema conformance |

### Batch D — statistics and plans

| Task | Rows | Change | Test |
| --- | --- | --- | --- |
| D1 | `metadata.spanner_sys` | Per-database stats collector; QUERY/READ/TXN/LOCK stats TOP/TOTAL × MINUTE/10MINUTE/HOUR, OLDEST_ACTIVE_QUERIES, ACTIVE_QUERIES_SUMMARY, TABLE/COLUMN_OPERATIONS_STATS; dynamic rows at execution; fix FLOAT64/BYTES/ARRAY type map and column order; `spanner_sys_reader` visibility | spanner_sys conformance |
| D2 | `googlesql.query_modes` | Plan tree from the resolved AST (Serialize Result, Distributed Union, Scan, Filter, joins, aggregates, sort/limit, Apply Mutations); PROFILE root and scan statistics with string leaves; plans on streaming responses; stats on the last message | query_modes conformance |

### Batch E — change streams, backups, TTL

| Task | Rows | Change | Test |
| --- | --- | --- | --- |
| E1 | `change_streams.read`, `postgresql.change_streams` | DML UPDATE records only key, updated and commit-timestamp columns; re-enable the disabled record-content test | change stream conformance |
| E2 | `change_streams.key_ranges` | MUTABLE_KEY_RANGE churn emits partition_event move_in/move_out records | churner + conformance |
| E3 | `backups.create`, `backups.restore` | `version_time` backups from MVCC history; accept GOOGLE_DEFAULT/USE_DATABASE encryption and report `encryption_info`; CMEK stays rejected (`security.encryption`) | backups_test + restart probe |
| E4 | `backups.schedules` | Incremental schedules as metadata chains (`incremental_backup_chain_id`, `oldest_version_time`), default encryption types | backups_test |
| E5 | `schema.row_deletion_policy` | TTL sweeper: batched system deletes, `RowDeletionPolicy` transaction tag, `exclude_ttl_deletes`, restore drops policies | database_test, change stream exclusion |

### Batch F — admin filters

| Task | Rows | Change | Test |
| --- | --- | --- | --- |
| F1 | `instance.configs`, `ops.instance_partitions`, `operations.list`, `backups.metadata` | Shared AIP-160 filter parser (OR binds tighter than AND) for backups, operation lists (reflection over metadata/response), ListInstances, ListSessions | handler tests |

### Batch G — qualification and documentation

- Full native suite, disposable restart probes for roles/grants, backups, TTL, database options.
- Re-run the client SDK/JDBC/PGAdapter/REST matrix on the final binary.
- Update `docs/feature-coverage.yaml` (statuses, splits, notes, evidence), regenerate
  `docs/feature-coverage.md`, and reconcile `docs/capabilities.md`, `docs/known-gaps.md`,
  `docs/configuration.md`, `docs/persistence.md`, `docs/change-streams.md`, `README.md`,
  and `docs/CHANGELOG.md`.

## Record splits

| Existing record | Local successor (status) | New physical record |
| --- | --- | --- |
| `security.iam_policies` | policy management and etag CAS (supported) | `security.iam_enforcement` (not-applicable: the emulator receives no authenticated identity) |
| `ops.quotas` | local schema/mutation limits (supported) | `ops.rate_quotas` (not-applicable: admin/API rate and capacity quotas) |
| `vector.ann_indexes` | index DDL and APPROX query contract with exact results (supported) | `vector.ann_recall_latency` (not-applicable: approximate recall/latency tuning) |
| `search.scoring` | deterministic local ranking and option validation (supported) | covered by production relevance note; no new record |

Physical residuals of `instance.move`, `instance.configs`, `ops.instance_partitions`,
`writes.partitioned_dml`, and `change_streams.partition_churn` are already covered by
`ops.replication`, `ops.autoscaling`, and the notes; no new records are needed.

## Out of scope

- Customer-managed encryption (`security.encryption`), Cloud Audit Logs, Cloud Monitoring
  export, replication, autoscaling: remain `not-applicable`.
- Packaged LocalCloud image qualification: recorded as a separate tier, not claimed here.
- `graph.algorithms` computation: remains `accepted-no-op`; this plan only corrects its notes.
  (Implemented later, in the remaining-limitations round; now `supported`.)

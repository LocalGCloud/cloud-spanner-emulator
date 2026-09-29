# Independent audit prompt: Spanner emulator feature coverage

> **Superseded 2026-09-28:** all records closed; current counts 155 = 137 supported / 9 accepted-no-op / 9 not-applicable. See the [usability audit worksheet](plans/2026-09-27-usability-audit-worksheet.md) and the [closure plan](plans/2026-09-27-usability-closure-plan.md). The text below is the historical record and is not kept up to date.

**Checkpoint:** 2026-09-27 on jay-spanner-extended; the checkout contains uncommitted work.  
**Purpose:** Give a fresh reviewer the complete open-feature inventory, its current explanation, and a falsifiable gate. This is an independent evaluation request, not an implementation assignment.

## Assignment to the reviewer

You are an independent reviewer of this Cloud Spanner emulator fork and its proposed LocalCloud integration. Treat the coverage matrix and implementation-progress statements as hypotheses. Work read-only. Do not implement features, edit statuses, or promote a row while doing this audit. Produce reproducible findings for all 152 records, with individual conclusions for the 44 partial, one unsupported, and one unknown records listed below.

Start with the canonical JSON-compatible YAML at docs/feature-coverage.yaml, its generated docs/feature-coverage.md, the implementation proposal at docs/superpowers/plans/2026-09-27-spanner-partial-feature-support.md, and user-facing docs/capabilities.md, docs/known-gaps.md, docs/configuration.md, docs/persistence.md, and docs/change-streams.md. Read source, tests, and exact execution output for each material claim. A source path or test path cited in the matrix is a lead, not a passing result. The plan's original 48 partial rows and checked work items are not the current snapshot.

Refresh the contract from **official Spanner documentation**, including the links embedded in each row. Use Context7 when available; if unavailable, read the official Google Cloud pages directly and record the retrieval date. Do not substitute generic GoogleSQL, upstream PostgreSQL, BigQuery, or the published upstream emulator for this fork's actual behavior. The [upstream emulator limitations](https://docs.cloud.google.com/spanner/docs/emulator) are context, not proof of this fork's behavior. High-value official starting points are [fine-grained access control](https://docs.cloud.google.com/spanner/docs/fgac-about), [backup lifecycle](https://docs.cloud.google.com/spanner/docs/backup), [backup schedules](https://docs.cloud.google.com/spanner/docs/reference/rest/v1/projects.instances.databases.backupSchedules), [vector search](https://docs.cloud.google.com/spanner/docs/vector-search-overview), and [ANN query behavior](https://docs.cloud.google.com/spanner/docs/find-approximate-nearest-neighbors).

### Snapshot and interpretation

The canonical inventory at this checkpoint contains **152 records: 89 supported, 44 partial, 11 accepted-no-op, 6 not-applicable, 1 unsupported, and 1 unknown**. Verify these counts from the file before relying on them. Five original partial rows were promoted after native evidence: types.uuid, vector.distance_functions, instance.update, database.update, and backups.metadata. Security.database_roles moved from accepted-no-op to partial, yielding the current 44. The full inventory review date in the YAML is 2026-09-24; its 2026-09-27 update targeted earlier partial and unsupported rows. The YAML value **tested** means a test is cited; it does not certify recent execution, full contract coverage, or packaged LocalCloud qualification.

Recent native evidence to challenge: UUID and exact distance public tests; persisted role create/drop/list without grants; SQL NULL tokenizer behavior; selected PostgreSQL aggregate cases; key/range locks and focused SELECT FOR UPDATE; configured ML and GoogleSQL remote UDF provider fixtures; due full backup schedules and restart; focused database-option catalog/GetDatabase/replay tests; and an actual REST HTTP regression preserving a 2,052-byte invalid-mask message after a shared 4,096-byte cap change. The REST and database-option checkpoints did not run an immutable packaged candidate. The database-option replay test is not a live process restart. A few tests passing do not close a broad row.

Trace claims across the public gRPC and REST listener, handler, evaluator/storage, durable state, and external provider when applicable. Distinguish **native unit**, **native public endpoint**, **disposable native process restart**, **packaged Spanner image**, and **immutable LocalCloud candidate** evidence. Record dialect, ports, launch flags, image identity and test command. Reject support arguments based only on parser acceptance, proto fields, mock values, a health endpoint, metadata storage without effect, or another LocalCloud service existing.

Compare ../bigquery-emulator-on-duckdb for reusable tests or libraries only where semantics match. Its VECTOR_SEARCH path performs exact search and cannot establish Spanner ANN. Its geography/S2 and HLL/ZetaSketch libraries have no identified counterpart among these open Spanner rows. Its provider-binding pattern may help ML, but it does not speak Spanner's Remote UDF protocol. LocalCloud identity, KMS, or inference counts only if the actual published Spanner paths call them. Confirm versions, licenses, protocol boundaries and packaging before recommending reuse.

For each broad row, decide whether a split is necessary. A split must keep every remaining locally observable behavior visible; a physical Cloud-only successor may be not-applicable when justified. Do not upgrade an original row merely because its easy subset works. Classify missing evidence separately from proven absent behavior.

## Open records: current notes and exit checks

Each **Current matrix note** below is a claim to test, not a conclusion. Each **Plan exit check** is the proposed gate to critique against current Spanner docs. Official links and cited paths are leads. Check actual test result, source behavior, negative cases, persistence, and packaged tier.


### SQL types, functions, and PostgreSQL queries (6)

**Shared audit probe:** Build a versioned Spanner-only signature table. Distinguish type roundtrip from function coverage, SQL NULL from JSON null, and production Spanner PostgreSQL from general PostgreSQL.


#### types.json — JSON type and functions

- **Classification:** partial; applicability: local-development; verification label: tested.
- **Current matrix note:** JSON columns, ARRAY<JSON>, DML values, index STORING, and selected JSON functions (JSON_QUERY, JSON_VALUE, JSON_SET, JSON_REMOVE, PARSE_JSON, TO_JSON, SAFE_TO_JSON, TO_JSON_STRING) have representative tests. The JSON type itself works for local development. JSON key and index-key restrictions match Spanner; the combined row remains partial because function coverage is allowlisted and has not been compared function by function with production.
- **Plan exit check to challenge:** JSON type roundtrip and documented local JSON functions/edge cases; a split closes only when both successor records pass.
- **Cited code:** backend/datamodel/types.cc, frontend/converters/values.cc, backend/query/feature_filter/gsql_supported_functions.cc.
- **Cited tests:** frontend/converters/values_test.cc, tests/conformance/cases/query.cc, tests/conformance/cases/dml.cc, tests/conformance/cases/arrays.cc, tests/conformance/cases/index_read_write.cc.
- **Official contract leads:** [official reference 1](https://cloud.google.com/spanner/docs/data-types).


#### googlesql.aggregate_functions — Aggregate functions

- **Classification:** partial; applicability: local-development; verification label: tested.
- **Current matrix note:** Built-in functions are limited to the Spanner allowlist in gsql_supported_functions.cc (ANY_VALUE, ARRAY_AGG, ARRAY_CONCAT_AGG, AVG, BIT_AND/OR/XOR, COUNT, COUNTIF, LOGICAL_AND/OR, MAX, MIN, STDDEV, STRING_AGG, SUM, VARIANCE and others); query_validator.cc rejects any other GoogleSQL builtin. Only some aggregates have dedicated tests (query.cc StddevAndVariance; PG MIN/MAX/SUM/AVG/ARRAY_AGG in pg_functions_test.cc), and the allowlist has not been audited against the production function list.
- **Plan exit check to challenge:** Documented signatures and NULL/empty/overflow/window boundaries pass the function matrix.
- **Cited code:** backend/query/feature_filter/gsql_supported_functions.cc, backend/query/feature_filter/sql_features_view.h, backend/query/query_validator.cc.
- **Cited tests:** tests/conformance/cases/query.cc, backend/query/query_engine_test.cc, tests/conformance/cases/pg_functions_test.cc.
- **Official contract leads:** [official reference 1](https://cloud.google.com/spanner/docs/google-sql-reference).


#### googlesql.string_functions — String functions

- **Classification:** partial; applicability: local-development; verification label: tested.
- **Current matrix note:** Allowlisted string functions (CONCAT, FORMAT, LPAD/RPAD, REGEXP_CONTAINS/EXTRACT/REPLACE, SPLIT, NORMALIZE, SOUNDEX, TO_/FROM_BASE32/BASE64, CODE_POINTS_*, and others) run on the reference evaluator; FORMAT, regex, and alias functions (CHAR_LENGTH, POWER, CEILING) are tested in query.cc. Not every function is individually tested, and builtins outside gsql_supported_functions.cc are rejected.
- **Plan exit check to challenge:** Documented signatures, Unicode, bytes, regex, and bad-input cases pass.
- **Cited code:** backend/query/feature_filter/gsql_supported_functions.cc, backend/query/query_validator.cc.
- **Cited tests:** tests/conformance/cases/query.cc, backend/query/query_engine_test.cc.
- **Official contract leads:** [official reference 1](https://cloud.google.com/spanner/docs/google-sql-reference).


#### googlesql.date_timestamp_functions — Date and timestamp functions

- **Classification:** partial; applicability: local-development; verification label: tested.
- **Current matrix note:** Core date and timestamp operations are tested, including ISOYEAR and ISOWEEK in DATE_TRUNC, TIMESTAMP_TRUNC, and DATE_DIFF, and TIMESTAMP literals without a zone resolve in America/Los_Angeles unless the database sets default_time_zone. The category remains partial because the complete production function surface is not exhaustively covered.
- **Plan exit check to challenge:** Documented signatures, timezone, DST, ISO week, and boundary cases pass.
- **Cited code:** backend/query/feature_filter/sql_feature_filter.cc, backend/query/feature_filter/sql_features_view.h, backend/query/feature_filter/gsql_supported_functions.cc.
- **Cited tests:** tests/conformance/cases/query.cc, tests/conformance/cases/default_time_zone.cc.
- **Official contract leads:** [official reference 1](https://cloud.google.com/spanner/docs/google-sql-reference).


#### postgresql.queries — PostgreSQL query translation

- **Classification:** partial; applicability: local-development; verification label: tested.
- **Current matrix note:** PostgreSQL SQL is translated to a GoogleSQL resolved tree and runs on the same evaluator. Focused native conformance passes previously disabled floating SUM/AVG empty/all-NULL cases and NUMERIC, JSONB, and OID ARRAY_AGG cases in pg_functions_test.cc. The full Spanner PostgreSQL query and function surface has not been audited; PostgreSQL syntax absent from Spanner's documented DML grammar is not counted as an emulator gap.
- **Plan exit check to challenge:** Spanner PostgreSQL grammar/function matrix passes; disabled cases are resolved with evidence.
- **Cited code:** third_party/spanner_pg/transformer/forward_query.cc, third_party/spanner_pg/transformer/forward_function.cc, backend/query/query_engine.cc.
- **Cited tests:** tests/conformance/cases/pg_functions_test.cc, third_party/spanner_pg/transformer/query_test.cc, third_party/spanner_pg/transformer/unsupported_query_test.cc, backend/query/query_engine_test.cc.
- **Official contract leads:** [official reference 1](https://cloud.google.com/spanner/docs/postgresql-interface).


#### postgresql.jsonb — PostgreSQL JSONB type and functions

- **Classification:** partial; applicability: local-development; verification label: tested.
- **Current matrix note:** JSONB parsing, normalization, storage, and selected functions are covered by tests; the type itself works for local development. Numbers keep up to 4,932 digits before the decimal point across tested platforms. The combined row remains partial because the full Spanner PostgreSQL JSONB function surface has not been audited.
- **Plan exit check to challenge:** Documented JSONB operators/functions, null/path, and numeric limits pass; a split closes only when both successor records pass.
- **Cited code:** third_party/spanner_pg/datatypes/common/jsonb/jsonb_value.cc.
- **Cited tests:** third_party/spanner_pg/datatypes/common/jsonb/jsonb_parse_test.cc, tests/conformance/cases/pg_functions_test.cc.
- **Official contract leads:** [official reference 1](https://cloud.google.com/spanner/docs/reference/postgresql/data-types).


### Catalogs and views (4)

**Shared audit probe:** Compare catalog schemas and dynamic rows after DDL, UDF, grants, and restart. Check the skipped pg_proc UDF case and role-filtered visibility. The two pg_catalog records share one implementation and should not create duplicate work.


#### postgresql.pg_catalog — pg_catalog views

- **Classification:** partial; applicability: local-development; verification label: tested.
- **Current matrix note:** pg_am, pg_attrdef, pg_attribute, pg_class, pg_collation, pg_constraint, pg_index, pg_indexes, pg_namespace, pg_proc, pg_sequence(s), pg_settings, pg_tables, pg_type, and pg_views have populated or static entries with representative tests. The pg_proc UDF-specific test is unconditionally skipped. Other catalog views such as pg_matviews, pg_policies, pg_cursors, and pg_prepared_xacts exist but return no rows in the cited test. Overlaps metadata.pg_catalog.
- **Plan exit check to challenge:** Documented catalog rows track schema/UDF/role changes, including the skipped `pg_proc` UDF case.
- **Cited code:** backend/query/pg_catalog.cc.
- **Cited tests:** tests/conformance/cases/pg_catalog_test.cc.
- **Official contract leads:** [official reference 1](https://cloud.google.com/spanner/docs/postgresql-interface).


#### postgresql.views — PostgreSQL views

- **Classification:** partial; applicability: local-development; verification label: tested.
- **Current matrix note:** CREATE [OR REPLACE] VIEW with SQL SECURITY INVOKER and DROP VIEW work, appear in information_schema.views and pg_views, and honor ORDER BY inside a view. The PostgreSQL DDL translator hardcodes INVOKER, so Spanner's SQL SECURITY DEFINER variant is not implemented. Active fixture cases reject column-name lists and TEMP views; WITH CHECK OPTION and WITH-clause cases are commented out, not tested.
- **Plan exit check to challenge:** Documented view variants and definer/invoker privilege behavior pass.
- **Cited code:** backend/query/queryable_view.cc, backend/schema/updater/schema_updater.cc, third_party/spanner_pg/ddl/pg_to_spanner_ddl_translator.cc.
- **Cited tests:** tests/conformance/cases/views.cc, tests/conformance/cases/pg_views_test.cc, tests/conformance/data/schema_changes/pg/ddl.create_view.test, tests/conformance/cases/pg_catalog_test.cc.
- **Official contract leads:** [official reference 1](https://cloud.google.com/spanner/docs/postgresql-interface).


#### metadata.information_schema — INFORMATION_SCHEMA

- **Classification:** partial; applicability: local-development; verification label: tested.
- **Current matrix note:** SCHEMATA, TABLES, COLUMNS, COLUMN_OPTIONS, INDEXES, INDEX_COLUMNS, constraint views, VIEWS, SEQUENCES, CHANGE_STREAM*, DATABASE_OPTIONS, PLACEMENTS, LOCALITY_GROUP_OPTIONS (plus MODELS* and PROPERTY_GRAPHS in GoogleSQL) are populated; SPANNER_STATISTICS is empty. Missing: ROLES/ROLE_* and *_PRIVILEGES, ROUTINES/PARAMETERS/ROUTINE_OPTIONS, TABLE_SYNONYMS, INDEX_OPTIONS, COLUMN_PARAMETERS and the COLUMNS.IS_STORED_VOLATILE column.
- **Plan exit check to challenge:** Missing documented views/columns have correct schema, rows, and role-filtered visibility in both dialects.
- **Cited code:** backend/query/information_schema_catalog.cc.
- **Cited tests:** tests/conformance/cases/information_schema.cc, backend/query/information_schema_catalog_test.cc.
- **Official contract leads:** [official reference 1](https://cloud.google.com/spanner/docs/information-schema).


#### metadata.pg_catalog — pg_catalog metadata

- **Classification:** partial; applicability: local-development; verification label: tested.
- **Current matrix note:** pg_am, pg_attrdef, pg_attribute, pg_class, pg_collation, pg_constraint, pg_index(es), pg_namespace, pg_proc, pg_sequence(s), pg_settings, pg_tables, pg_type and pg_views are populated for PostgreSQL databases. Other catalogs (pg_roles, pg_extension, pg_description, pg_enum and others) exist but are empty or static; anything else is missing.
- **Plan exit check to challenge:** Same shared dynamic catalog behavior as `postgresql.pg_catalog`; no duplicate implementation.
- **Cited code:** backend/query/pg_catalog.cc.
- **Cited tests:** tests/conformance/cases/pg_catalog_test.cc.
- **Official contract leads:** [official reference 1](https://cloud.google.com/spanner/docs/information-schema).


### Roles and IAM (3)

**Shared audit probe:** Prove trusted principal propagation and authorization on both published gRPC and REST ports. A client-asserted identity, a gateway-only policy check, or a stored policy is insufficient. Test two restricted roles, grants, inheritance, revoke, outage, and restart.


#### security.database_roles — Database roles

- **Classification:** partial; applicability: local-development; verification label: tested.
- **Current matrix note:** CREATE/DROP ROLE persist in both dialects and ListDatabaseRoles enumerates and paginates them. GRANT/REVOKE, role privileges and membership, and session creator_role authorization remain unimplemented.
- **Plan exit check to challenge:** Persist GRANT/REVOKE, privilege and membership state; prove role metadata and allow/deny for restricted sessions in both dialects after restart.
- **Cited code:** frontend/handlers/database_extensions.cc, backend/schema/updater/schema_updater.cc, backend/schema/catalog/role.cc, third_party/spanner_pg/ddl/pg_to_spanner_ddl_translator.cc.
- **Cited tests:** frontend/handlers/database_extensions_test.cc, backend/schema/updater/schema_updater_tests/common.cc, frontend/persistence/metadata_store_test.cc, third_party/spanner_pg/ddl/ddl_test.cc.
- **Official contract leads:** [official reference 1](https://cloud.google.com/spanner/docs/iam).


#### security.iam_policies — IAM policy RPCs

- **Classification:** partial; applicability: compatibility-only; verification label: tested.
- **Current matrix note:** SetIamPolicy and GetIamPolicy store and return policies for instances, databases, instance partitions, instance configs, backups, and backup schedules; policies persist with --data_dir and are removed with their resource. At startup a database that failed to restore keeps its policy, and a policy whose resource no longer exists is dropped with a warning instead of stopping the emulator. A supplied nonempty etag must match the current policy or SetIamPolicy returns ABORTED; every successful write gets a fresh etag. TestIamPermissions still returns every requested permission, and no policy is enforced.
- **Plan exit check to challenge:** Etag CAS, calculated permissions, and enforced allow/deny on direct data and admin RPCs, including restart/outage.
- **Cited code:** frontend/handlers/policies.cc, frontend/server/environment.h, frontend/persistence/metadata_store.cc.
- **Cited tests:** frontend/handlers/policies_test.cc, frontend/persistence/metadata_store_test.cc.
- **Official contract leads:** [official reference 1](https://cloud.google.com/spanner/docs/iam).


#### metadata.database_roles_metadata — Database role metadata

- **Classification:** unsupported; applicability: local-development; verification label: tested.
- **Current matrix note:** CREATE/DROP ROLE now persist and ListDatabaseRoles enumerates created roles, but GRANT/REVOKE are not stored, role and privilege information-schema views are absent, and restricted sessions are not enforced. The full role-metadata gate remains open.
- **Plan exit check to challenge:** Move from unsupported only after role/grant DDL persists, ListDatabaseRoles returns those roles, privilege views reflect grants, and restricted sessions enforce them in both dialects.
- **Cited code:** backend/schema/catalog/role.cc, backend/schema/updater/schema_updater.cc, backend/query/information_schema_catalog.cc, frontend/handlers/database_extensions.cc.
- **Cited tests:** tests/conformance/cases/information_schema.cc, frontend/handlers/database_extensions_test.cc, frontend/persistence/metadata_store_test.cc.
- **Official contract leads:** [official reference 1](https://cloud.google.com/spanner/docs/information-schema).


### Transactions, DML, and indexes (7)

**Shared audit probe:** Run real concurrent schedules with distinct keys and overlapping ranges. Check abort cleanup, a two-key deadlock, read timestamps and deadlines, error-state transitions, and index maintenance while writes continue.


#### writes.partitioned_dml — Partitioned DML

- **Classification:** partial; applicability: local-development; verification label: tested.
- **Current matrix note:** Partitioned DML UPDATE and DELETE statements, including parameterized ones, work in both dialects. INSERT and non-partitionable statements are rejected, reads in a PDML transaction fail, and REPEATABLE_READ isolation is rejected. The whole statement runs as one local transaction, with none of Cloud Spanner's distributed partitioning, retries, or throughput.
- **Plan exit check to challenge:** Per-partition transactions, rejection rules, partial-failure semantics, and both dialects.
- **Cited code:** backend/query/query_engine.cc, frontend/handlers/queries.cc, backend/query/partitioned_dml_validator.h, frontend/entities/session.cc.
- **Cited tests:** tests/conformance/cases/partitioned_dml.cc, backend/query/partitioned_dml_validator_test.cc, tests/conformance/cases/dml.cc.
- **Official contract leads:** [official reference 1](https://cloud.google.com/spanner/docs/dml-syntax).


#### transactions.stale_reads — Stale reads

- **Classification:** partial; applicability: local-development; verification label: tested.
- **Current matrix note:** Exact and bounded staleness reads work within the database's version_retention_period (default 1 hour, settable with ALTER DATABASE); older timestamps fail with FAILED_PRECONDITION. The emulator picks a local timestamp for bounded reads rather than selecting a replica, and rejects read timestamps more than 1 hour in the future. Those selection and future-bound limits keep this row partial.
- **Plan exit check to challenge:** Exact/bounded/future timestamp, retention, deadline, and local timestamp behavior.
- **Cited code:** frontend/converters/reads.cc, backend/storage/in_memory_storage.cc, backend/storage/persistent_storage.cc.
- **Cited tests:** tests/conformance/cases/snapshot_reads.cc, frontend/handlers/reads_test.cc, backend/storage/persistent_storage_test.cc.
- **Official contract leads:** [official reference 1](https://cloud.google.com/spanner/docs/transactions).


#### transactions.select_for_update — SELECT FOR UPDATE

- **Classification:** partial; applicability: local-development; verification label: tested.
- **Current matrix note:** SELECT ... FOR UPDATE works in tested read-write transaction queries and is rejected in read-only transactions or together with a lock_scanned_ranges hint. PostgreSQL rejects FOR UPDATE with set operations and GROUP BY/HAVING; GoogleSQL accepts the cited cases. The key/range lock manager allows unrelated keys; a focused two-key conflict test and all 20 SELECT FOR UPDATE conformance cases pass across both dialects. Broad-scan and direct-DML conflict combinations still need conformance coverage.
- **Plan exit check to challenge:** Row/range conflict behavior without blocking unrelated keys.
- **Cited code:** backend/query/query_validator.cc, backend/query/queryable_table.cc, backend/locking/manager.cc.
- **Cited tests:** tests/conformance/cases/select_for_update.cc, backend/query/queryable_table_test.cc, backend/locking/manager_test.cc.
- **Official contract leads:** [official reference 1](https://cloud.google.com/spanner/docs/transactions).


#### transactions.transaction_errors — Transaction error handling

- **Classification:** partial; applicability: local-development; verification label: tested.
- **Current matrix note:** A constraint-tagged DML error now permits reads of earlier buffered writes in the still-active read-write transaction; further DML and commit replay the saved error, and a rejected commit invalidates it. The formerly disabled read-after-DML case passes in both dialects with a 32-case surrounding transaction-error filter. Batch DML and backend Write failure read behavior remain unqualified, so the full error-state matrix is still partial. If several constraints fail, the reported violation may differ from production.
- **Plan exit check to challenge:** Previously disabled case and transaction state/error-class matrix pass.
- **Cited code:** backend/transaction/read_write_transaction.cc, frontend/entities/transaction.cc.
- **Cited tests:** tests/conformance/cases/transaction_errors.cc, tests/conformance/cases/transactions.cc.
- **Official contract leads:** [official reference 1](https://cloud.google.com/spanner/docs/transactions).


#### transactions.isolation_levels — Transaction isolation levels

- **Classification:** partial; applicability: local-development; verification label: tested.
- **Current matrix note:** SERIALIZABLE and REPEATABLE_READ options are accepted for read-only and read-write transactions; partitioned DML rejects REPEATABLE_READ. Outside that rejection, both options use the same backend transaction path: Spanner's distinct REPEATABLE_READ snapshot and conflict semantics are not emulated. The cited tests check option acceptance and rejection, not isolation behavior.
- **Plan exit check to challenge:** Repeatable-read snapshot/write-conflict behavior differs correctly from serializable.
- **Cited code:** frontend/entities/session.cc.
- **Cited tests:** frontend/handlers/transactions_test.cc.
- **Official contract leads:** [official reference 1](https://cloud.google.com/spanner/docs/isolation-levels).


#### transactions.concurrency_model — Concurrency, aborts, and fault injection

- **Classification:** partial; applicability: local-development; verification label: tested.
- **Current matrix note:** Key/range shared and exclusive locks let disjoint read-write transactions remain active and commit while overlapping operations abort. Focused manager and transaction tests cover disjoint ranges and tables, read upgrade, lock cleanup, abort/retry, and a 5,000-row insert performance gate; full SELECT FOR UPDATE conformance passed 20 cases. --abort_current_transaction_probability (default 20, emulator_main only) controls holder-abort attempts. --enable_fault_injection aborts about 5% of first commit attempts; a focused test observes and retries the first abort. Separate-thread writers, a two-key deadlock cycle, and gateway flag forwarding remain unverified.
- **Plan exit check to challenge:** Concurrent disjoint work succeeds; overlapping work aborts/retries predictably.
- **Cited code:** backend/locking/manager.cc, backend/transaction/read_write_transaction.cc, common/config.cc.
- **Cited tests:** backend/locking/manager_test.cc, backend/database/database_test.cc, tests/conformance/cases/batch_dml.cc, backend/transaction/read_write_transaction_test.cc.
- **Official contract leads:** [official reference 1](https://cloud.google.com/spanner/docs/transactions).


#### indexes.index_backfill — Index backfill

- **Classification:** partial; applicability: local-development; verification label: tested.
- **Current matrix note:** Adding an index to a populated table backfills it (unique, NULL_FILTERED, STORING and interleaved indexes, with duplicate-key and key-size validation). Backfill runs synchronously inside the UpdateDatabaseDdl call, so there is no progress reporting and a long backfill blocks other schema changes and read-write transactions.
- **Plan exit check to challenge:** Correct backfill with usable operation lifecycle and concurrent access at local scale.
- **Cited code:** backend/schema/backfills/index_backfill.cc.
- **Cited tests:** tests/conformance/cases/index_backfill.cc, backend/schema/backfills/index_backfill_test.cc.
- **Official contract leads:** [official reference 1](https://cloud.google.com/spanner/docs/secondary-indexes).


### Change streams and backups (7)

**Shared audit probe:** Exercise the entire record and backup lifecycle through a disposable process restart. Separate an in-flight RPC from a client's ability to reconnect, and distinguish scheduled time, capture time, and backup version_time. Verify encryption of bytes rather than acceptance of a key name.


#### change_streams.read — Read change stream

- **Classification:** partial; applicability: local-development; verification label: tested.
- **Current matrix note:** READ_<stream> queries return data change, heartbeat, and child partition records; start_timestamp is checked against the retention period and the stream's creation time, which survives restarts and restores. A separate stream view now emits DELETE then INSERT for REPLACE of an existing row while commit writes remain netted; record_sequence increases across tables without promising mutation input order. Eight focused conformance cases passed. Remaining gaps include DML INSERT recording NULL for omitted columns, placeholder resume tokens, and no background record garbage collection. In-flight queries do not survive restart.
- **Plan exit check to challenge:** REPLACE shape, cross-table order, retry position, and restart replay pass.
- **Cited code:** frontend/handlers/change_streams.cc, backend/query/change_stream/change_stream_query_validator.cc, backend/transaction/transaction_store.cc.
- **Cited tests:** frontend/handlers/change_streams_test.cc, backend/query/change_stream/change_stream_query_validator_test.cc, tests/conformance/cases/change_streams_read_write.cc.
- **Official contract leads:** [official reference 1](https://cloud.google.com/spanner/docs/change-streams).


#### change_streams.key_ranges — Change stream key ranges

- **Classification:** partial; applicability: local-development; verification label: tested.
- **Current matrix note:** partition_mode = 'MUTABLE_KEY_RANGE' streams return partition start and data change records in both dialects. Tests cover initial partitions and INSERT records; other record shapes have less evidence. Partitions only split: the MOVE churn production can perform never happens locally.
- **Plan exit check to challenge:** MOVE and all mutation record shapes pass in both dialects.
- **Cited code:** backend/schema/catalog/change_stream.h, backend/database/change_stream/change_stream_partition_churner.cc.
- **Cited tests:** tests/conformance/cases/change_streams_mutable_key_range.cc, backend/database/change_stream/change_stream_partition_churner_test.cc.
- **Official contract leads:** [official reference 1](https://cloud.google.com/spanner/docs/change-streams).


#### change_streams.partition_churn — Partition churn

- **Classification:** partial; applicability: local-development; verification label: tested.
- **Current matrix note:** A background churner periodically splits and merges partitions so clients see child partition records. Churn is timer-driven (normally after 20-40 seconds), not load-driven, and all writes go to the active partition whose token sorts first; other active partitions produce only heartbeat and child-partition records. The token lifetime can be shortened with --override_change_stream_partition_token_alive_seconds. A cited replay test verifies that initial partitions are not duplicated after restart with --data_dir; full churned-history replay is not established.
- **Plan exit check to challenge:** Routed partitions, child/heartbeat lifecycle, and complete persisted churn replay pass.
- **Cited code:** backend/database/change_stream/change_stream_partition_churner.cc, frontend/collections/database_manager.cc, backend/actions/change_stream.cc.
- **Cited tests:** backend/database/change_stream/change_stream_partition_churner_test.cc, frontend/collections/database_manager_test.cc.
- **Official contract leads:** [official reference 1](https://cloud.google.com/spanner/docs/change-streams).


#### postgresql.change_streams — PostgreSQL change streams

- **Classification:** partial; applicability: local-development; verification label: tested.
- **Current matrix note:** CREATE/ALTER/DROP CHANGE STREAM and spanner.read_json_<stream>(...) work, with JSONB insert, update, and delete records tested. The updater accepts all four documented value_capture_type values, and focused updater tests cover SET NULL after an explicit value and native PostgreSQL RESET. The shared REPLACE path emits DELETE then INSERT for an existing row; multi-table record sequences are monotonic, while order need not match mutation input order. Broader stream lifecycle and capture fidelity remain partial.
- **Plan exit check to challenge:** Documented capture/reset variants and shared record semantics pass.
- **Cited code:** frontend/handlers/change_streams.cc, frontend/converters/pg_change_streams.cc, backend/query/change_stream/queryable_change_stream_tvf.cc, backend/schema/updater/schema_updater.cc, third_party/spanner_pg/ddl/pg_to_spanner_ddl_translator.cc.
- **Cited tests:** tests/conformance/cases/pg_change_streams_read_write.cc, frontend/converters/pg_change_streams_test.cc, backend/query/change_stream/pg_change_stream_query_validator_test.cc, backend/query/change_stream/pg_queryable_change_stream_tvf_test.cc, backend/schema/updater/schema_updater_tests/change_stream_test.cc, tests/conformance/data/schema_changes/pg/ddl.alter_change_stream.test.
- **Official contract leads:** [official reference 1](https://cloud.google.com/spanner/docs/postgresql-interface).


#### backups.create — CreateBackup RPC

- **Classification:** partial; applicability: local-development; verification label: tested.
- **Current matrix note:** CreateBackup takes a point-in-time copy of the database's storage and requires --data_dir; without it the call returns FAILED_PRECONDITION. expire_time is required and must be 6 hours to 366 days after the capture time; version_time (historical backups) returns INVALID_ARGUMENT. encryption_config is rejected because local snapshots are plaintext.
- **Plan exit check to challenge:** Historical snapshot and actual requested encryption behavior pass.
- **Cited code:** frontend/handlers/backups.cc, frontend/persistence/backup_catalog.cc, backend/storage/persistent_storage.cc.
- **Cited tests:** frontend/handlers/backups_test.cc, frontend/persistence/backup_catalog_test.cc, backend/storage/persistent_storage_test.cc.
- **Official contract leads:** [official reference 1](https://cloud.google.com/spanner/docs/reference/rpc/google.spanner.admin.database.v1#databaseadmin).


#### backups.restore — RestoreDatabase RPC

- **Classification:** partial; applicability: local-development; verification label: tested.
- **Current matrix note:** RestoreDatabase copies a READY local backup into a new database and requires --data_dir; the target must be in the backup's project on an instance with the same instance config. Schema contents and saved sequence counters, including those used by identity columns, are restored. The first schema batch receives the restore time, so pre-restore change-stream start timestamps are not guaranteed to work. Ordinary failures roll back the target; a failed rollback can require restart reconciliation.
- **Plan exit check to challenge:** Data/schema/options/sequence restore, fresh change-stream operation without old internal history, and failure recovery pass.
- **Cited code:** frontend/handlers/backups.cc, frontend/collections/database_manager.cc, backend/database/database.cc, backend/storage/sequence_state_store.cc, backend/query/change_stream/change_stream_query_validator.cc.
- **Cited tests:** frontend/handlers/backups_test.cc, frontend/collections/database_manager_test.cc, backend/query/query_engine_test.cc.
- **Official contract leads:** [official reference 1](https://cloud.google.com/spanner/docs/reference/rpc/google.spanner.admin.database.v1#databaseadmin).


#### backups.schedules — Backup schedule RPCs

- **Classification:** partial; applicability: local-development; verification label: tested.
- **Current matrix note:** Backup schedule create, get, list, update (update_mask required), and delete persist with --data_dir. The native worker runs supported 12-hour, daily, weekly, and monthly full-backup cron schedules in UTC, creates each due backup once, applies retention from capture time, and persists its cursor. Full handler and catalog targets and a direct gRPC --data_dir process-restart probe passed. Schedule-update races are guarded by a catalog compare-and-swap over the whole schedule. Incremental schedules and encryption remain unimplemented, so this row remains partial; packaged LocalCloud qualification is open.
- **Plan exit check to challenge:** Due schedules create backups once, with retention and restart recovery.
- **Cited code:** frontend/handlers/backups.cc, frontend/persistence/backup_catalog.cc, binaries/emulator_main.cc.
- **Cited tests:** frontend/handlers/backups_test.cc, frontend/persistence/backup_catalog_test.cc.
- **Official contract leads:** [official reference 1](https://cloud.google.com/spanner/docs/reference/rpc/google.spanner.admin.database.v1).


### Search and vector (5)

**Shared audit probe:** Use documented multilingual and HTML fixtures, SQL NULL, option effects, and public query restrictions. Exact distance functions are a separate supported feature. Require an ANN index that changes candidate visitation, survives mutations/restart, and meets the plan's fixed recall gate.


#### search.search_indexes — Search indexes

- **Classification:** partial; applicability: local-development; verification label: tested.
- **Current matrix note:** CREATE/DROP SEARCH INDEX over TOKENLIST columns (with STORING, PARTITION BY/ORDER BY) and search-index hints work in both dialects. ALTER SEARCH INDEX is not parsed. sort_order_sharding is stored and printed but has no physical sharding effect; disable_automatic_uid_column affects backing-table key-count validation, so search-index options are not all ignored.
- **Plan exit check to challenge:** Documented create/alter/drop/options and consistent indexed reads pass.
- **Cited code:** backend/schema/updater/schema_updater.cc, backend/schema/parser/ddl_parser.cc, backend/schema/validators/table_validator.cc, backend/schema/printer/print_ddl.cc.
- **Cited tests:** tests/conformance/cases/search_test.cc, backend/schema/updater/schema_updater_tests/index.cc, backend/schema/parser/ddl_parser_test.cc.
- **Official contract leads:** [official reference 1](https://cloud.google.com/spanner/docs/full-text-search).


#### search.search_functions — SEARCH function

- **Classification:** partial; applicability: local-development; verification label: tested.
- **Current matrix note:** SEARCH (words, words_phrase and rquery dialects with AND/OR/NOT/AROUND), SEARCH_SUBSTRING, SEARCH_NGRAMS, SNIPPET and JSON search work in queries. Read-write transaction search requires the allow_search_indexes_in_transaction statement hint; FOR UPDATE, partitioned DML, and batch DML search uses are rejected. WORDS and WORDS_PHRASE apply language_tag to query case normalization (including tested Turkish casing), but RQUERY does not use locale-aware parsing and broader language analysis is incomplete. enhance_query is accepted without local query enhancement.
- **Plan exit check to challenge:** Search query grammar, language/enhancement options, and transaction restrictions pass.
- **Cited code:** backend/query/search/search_evaluator.cc, backend/query/search/search_substring_evaluator.cc, backend/query/search/search_ngrams_evaluator.cc, backend/query/search/snippet_evaluator.cc, backend/query/search/search_function_catalog.cc.
- **Cited tests:** tests/conformance/cases/search_test.cc, backend/query/search/search_evaluator_test.cc, backend/query/search/search_substring_evaluator_test.cc, backend/query/search/search_ngrams_evaluator_test.cc, backend/query/search/snippet_evaluator_test.cc.
- **Official contract leads:** [official reference 1](https://cloud.google.com/spanner/docs/full-text-search).


#### search.tokenization — Tokenization

- **Classification:** partial; applicability: local-development; verification label: tested.
- **Current matrix note:** TOKENIZE_FULLTEXT, TOKENIZE_SUBSTRING, TOKENIZE_NGRAMS, TOKENIZE_NUMBER, TOKENIZE_BOOL, TOKENIZE_JSON, TOKEN and TOKENLIST_CONCAT are available. ICU word segmentation and Unicode code-point n-grams, diacritic removal in full-text/substring/ngram tokenization and matching, category signals, and basic HTML text extraction have focused tests and a GoogleSQL SQL conformance case. SQL NULL inputs return SQL NULL across the tokenizers and TOKENLIST_CONCAT in unit and public GoogleSQL tests. The HTML scanner handles common/numeric entities but not full HTML5 or script/style content; documented French/CJK language behavior remains unverified, default rquery rejects Unicode terms, short_tokens_only_for_anchors is ignored, and mixed diacritic settings in TOKENLIST_CONCAT are incomplete.
- **Plan exit check to challenge:** Documented Unicode, diacritic, content, and category options produce tested tokens.
- **Cited code:** backend/query/search/plain_full_text_tokenizer.cc, backend/query/search/substring_tokenizer.cc, backend/query/search/ngrams_tokenizer.cc, backend/query/search/tokenizer.cc, backend/query/search/score_evaluator.cc, backend/query/search/numeric_tokenizer.cc, backend/query/search/search_function_catalog.cc.
- **Cited tests:** backend/query/search/plain_full_text_tokenizer_test.cc, backend/query/search/substring_tokenizer_test.cc, backend/query/search/ngrams_tokenizer_test.cc, backend/query/search/score_evaluator_test.cc, backend/query/search/tokenlist_concat_test.cc, backend/query/search/numeric_tokenizer_test.cc, tests/conformance/cases/search_test.cc.
- **Official contract leads:** [official reference 1](https://cloud.google.com/spanner/docs/full-text-search).


#### search.scoring — Search scoring

- **Classification:** partial; applicability: compatibility-only; verification label: tested.
- **Current matrix note:** SCORE and SCORE_NGRAMS return deterministic local scores (term-occurrence counts or n-gram overlap), not production relevance values. SCORE accepts omitted, NULL, and empty JSON options but rejects nonempty options it cannot honor rather than silently ignoring them; malformed JSON is rejected. SCORE_NGRAMS accepts only the documented trigrams algorithm and returns zero for empty trigram sets. The pinned signature lacks the currently documented array_aggregator option.
- **Plan exit check to challenge:** Documented dialect/language/enhancement/dictionary/JSON options each have tested ranking, NULL, or error behavior.
- **Cited code:** backend/query/search/score_evaluator.cc, backend/query/search/score_ngrams_evaluator.cc.
- **Cited tests:** backend/query/search/score_evaluator_test.cc, backend/query/search/score_ngrams_evaluator_test.cc, tests/conformance/cases/search_test.cc.
- **Official contract leads:** [official reference 1](https://cloud.google.com/spanner/docs/full-text-search).


#### vector.ann_indexes — Approximate nearest-neighbor indexes

- **Classification:** partial; applicability: local-development; verification label: tested.
- **Current matrix note:** CREATE/ALTER/DROP VECTOR INDEX (distance_type, tree_depth, num_leaves, STORING) is validated, and APPROX_COSINE_DISTANCE/APPROX_EUCLIDEAN_DISTANCE/APPROX_DOT_PRODUCT queries enforce production's shape rules (ORDER BY ... LIMIT, matching distance type, IS NOT NULL filter). Queries run as an exact brute-force scan, so results are exact rather than approximate and recall or latency cannot be evaluated.
- **Plan exit check to challenge:** A maintained approximate local index passes the fixed recall@10 >= 0.90 gate, candidate-count option test, and restart test; production latency is excluded.
- **Cited code:** backend/schema/updater/schema_updater.cc, backend/query/ann_validator.cc, backend/query/ann_functions_rewriter.cc.
- **Cited tests:** tests/conformance/cases/ann_test.cc, backend/schema/updater/schema_updater_tests/index.cc.
- **Official contract leads:** [official reference 1](https://cloud.google.com/spanner/docs/find-k-nearest-neighbors).


### Administration, REST, and clients (8)

**Shared audit probe:** Test actual handlers and HTTP transport, masks, long errors, pagination, etags, persistence, and SDK versions. Split physical capacity/placement from metadata semantics. Recheck the just-completed focused REST and database-option checkpoints without treating them as packaged proof.


#### instance.configs — Instance configs RPCs

- **Classification:** partial; applicability: compatibility-only; verification label: tested.
- **Current matrix note:** The built-in emulator-config is always listed and its ID is reserved. Custom configs validate canonical names, custom- IDs, the built-in base config, labels, and update masks; create/get/list/update/delete, bounded config-list pagination, typed operation metadata, backup reference guards, etag-guarded update/delete, and --data_dir persistence have handler tests. ListInstanceConfigOperations validates parent-bound opaque tokens and supports simple done, name, metadata.@type, and response.@type predicates; compound and nested Cloud filter syntax and start-time ordering remain open. Configs are metadata only; replicas and regions have no effect.
- **Plan exit check to challenge:** Config CRUD and references pass; physical replicas/regions are explicitly out of local scope.
- **Cited code:** frontend/handlers/instances.cc, frontend/handlers/instance_extensions.cc.
- **Cited tests:** frontend/handlers/instances_test.cc, frontend/handlers/instance_extensions_test.cc, tests/gcloud/instance_admin_test.py.
- **Official contract leads:** [official reference 1](https://cloud.google.com/spanner/docs/reference/rpc/google.spanner.admin.instance.v1#instanceadmin).


#### ops.instance_partitions — Instance partitions

- **Classification:** partial; applicability: local-development; verification label: tested.
- **Current matrix note:** Instance partition create, get, list (paginated), update, delete, and ListInstancePartitionOperations work and are persisted with --data_dir; referencing_databases lists databases whose placements use the partition. Update returns typed operation metadata, and operation listing has bounded pagination. Delete fails with FAILED_PRECONDITION while a placement uses the partition, and partitions are deleted with their instance. A direct restart probe covered persisted partition state and placement references using a binary built before the latest metadata/pagination changes; current-source handler tests cover those changes. Operation filtering and physical capacity remain open; regions are metadata only.
- **Plan exit check to challenge:** Partition CRUD, placements, deletion guards, and restart pass; physical capacity is separated.
- **Cited code:** frontend/handlers/instance_partitions.cc, frontend/collections/instance_partition_manager.cc.
- **Cited tests:** frontend/handlers/instance_partitions_test.cc, frontend/collections/instance_partition_manager_test.cc, tests/conformance/cases/placements.cc, tests/gcloud/instance_admin_test.py.
- **Official contract leads:** [official reference 1](https://cloud.google.com/spanner/docs/instances).


#### ops.quotas — Cloud quotas and limits

- **Classification:** partial; applicability: local-development; verification label: tested.
- **Current matrix note:** Schema limits are enforced (columns per table, name lengths, indexes per table, key sizes, value size, interleaving depth, change streams per table and column), plus 100 databases per instance (--override_max_databases_per_instance). Commit and BatchWrite reject more than 80,000 distinct explicitly written cells with INVALID_ARGUMENT; this is a lower-bound check that omits index, generated-column, delete, and DML effects. Commit-size and admin API rate limits are not enforced; the tables-per-database and indexes-per-database tests are disabled.
- **Plan exit check to challenge:** Local schema/mutation limits and boundary errors pass; cloud rate/capacity quotas are separated.
- **Cited code:** common/limits.h, frontend/collections/database_manager.cc, frontend/converters/mutations.cc, frontend/handlers/transactions.cc, frontend/handlers/batch.cc.
- **Cited tests:** tests/conformance/cases/limits.cc, frontend/collections/database_manager_test.cc, frontend/handlers/databases_test.cc, frontend/converters/mutations_test.cc, frontend/handlers/transactions_test.cc, frontend/handlers/batch_test.cc.
- **Official contract leads:** [official reference 1](https://cloud.google.com/spanner/docs/instances).


#### clients.rest_gateway — REST gateway compatibility

- **Classification:** partial; applicability: local-development; verification label: tested.
- **Current matrix note:** REST (port 9020) is served by the grpc-gateway proxy in gateway_main, which gcloud tests use end to end. Errors retain the gRPC status code; a direct gateway_main HTTP test now confirms a 2,052-byte invalid field-mask message survives with HTTP 400 and code 3 after the shared cap rose to 4,096 bytes. The rich status preserves standard google.rpc details, including PostgreSQL SQLSTATE as ErrorInfo, but the SQLSTATE case has conversion-unit coverage rather than an HTTP end-to-end test. The packaged gateway remains unqualified. Only gateway_main exposes REST; emulator_main alone is gRPC-only. URL field masks in JSON camelCase are converted to proto field names; snake_case paths work too.
- **Plan exit check to challenge:** Codes/messages/details/field masks survive direct REST and packaged gateway tests.
- **Cited code:** gateway/gateway.go, binaries/gateway_main.go, frontend/common/status.cc, common/limits.h.
- **Cited tests:** frontend/common/status_test.cc, tests/gcloud/read_write_test.py, tests/gcloud/instance_admin_test.py, gateway/gateway_test.go.
- **Official contract leads:** [official reference 1](https://cloud.google.com/spanner/docs/emulator).


#### clients.client_libraries — Google client library compatibility

- **Classification:** partial; applicability: local-development; verification label: tested.
- **Current matrix note:** The conformance suite drives the emulator through the google-cloud-cpp Spanner client. Other language clients connect with SPANNER_EMULATOR_HOST but are not tested in this repository.
- **Plan exit check to challenge:** A recorded multi-language SDK matrix passes representative admin/query/transaction flows.
- **Cited code:** frontend/server/server.cc.
- **Cited tests:** tests/conformance/endpoints/emulator_conformance_test.cc, tests/conformance/cases/sessions.cc.
- **Official contract leads:** [official reference 1](https://cloud.google.com/spanner/docs/emulator).


#### database.internal_graph_operation — InternalUpdateGraphOperation RPC

- **Classification:** partial; applicability: compatibility-only; verification label: tested.
- **Current matrix note:** InternalUpdateGraphOperation validates the database and operation ID, looks up an existing local operation, and marks it failed when a nonzero status is supplied. Successful status updates and production graph backfill work are not modeled.
- **Plan exit check to challenge:** Documented local operation status and graph effects pass or the internal-only scope is split.
- **Cited code:** frontend/handlers/database_extensions.cc.
- **Cited tests:** frontend/handlers/database_extensions_test.cc.
- **Official contract leads:** [official reference 1](https://cloud.google.com/spanner/docs/reference/rpc/google.spanner.admin.database.v1).


#### instance.move — MoveInstance RPC

- **Classification:** partial; applicability: local-development; verification label: tested.
- **Current matrix note:** MoveInstance validates a canonical target_config, rejects instances with source backups, updates the instance's config metadata, and records a completed operation with typed progress metadata that is persisted with --data_dir. The operation completes immediately; no data or physical placement moves. The pinned request proto lacks the currently documented target_database_move_configs field.
- **Plan exit check to challenge:** Config/reference move is atomic and persists; physical relocation is separated.
- **Cited code:** frontend/handlers/instance_extensions.cc.
- **Cited tests:** frontend/handlers/instance_extensions_test.cc.
- **Official contract leads:** [official reference 1](https://cloud.google.com/spanner/docs/reference/rpc/google.spanner.admin.instance.v1).


#### schema.database_options — ALTER DATABASE options

- **Classification:** partial; applicability: local-development; verification label: tested.
- **Current matrix note:** ALTER DATABASE SET OPTIONS supports version_retention_period (enforced for stale reads and version garbage collection), default_time_zone (default America/Los_Angeles; applied to queries, parameters, DML, generated columns, checks, and views; cannot change once tables exist), and default_sequence_kind. Unrelated ALTER statements now retain prior option values. GetDatabase reports version_retention_period, configured default_leader, and a moving earliest_version_time bounded by database creation time. default_leader, witness_location, and read_lease_regions are projected in INFORMATION_SCHEMA.DATABASE_OPTIONS from stored options; they have no physical placement effect. columnar_policy is catalog metadata only. Focused native catalog, GetDatabase, and metadata-replay tests pass; real process restart and packaged qualification remain open.
- **Plan exit check to challenge:** Observable options, `GetDatabase` fields, and restart pass; leader physics is separated.
- **Cited code:** backend/schema/updater/schema_updater.cc, backend/schema/catalog/versioned_catalog.cc, backend/transaction/read_only_transaction.cc, backend/query/information_schema_catalog.cc, backend/query/analyzer_options.cc, backend/database/database.cc, frontend/entities/database.cc.
- **Cited tests:** backend/schema/updater/schema_updater_tests/database_option.cc, tests/conformance/cases/default_time_zone.cc, tests/conformance/cases/snapshot_reads.cc, tests/conformance/cases/information_schema.cc, tests/conformance/cases/query.cc, backend/query/information_schema_catalog_test.cc, frontend/handlers/databases_test.cc.
- **Official contract leads:** [official reference 1](https://cloud.google.com/spanner/docs/data-definition-language).


### Remote UDF and ML (3)

**Shared audit probe:** Configured-provider output must depend on inputs and preserve provider errors. Test missing/malformed provider responses, timeout, restart, direct gRPC and REST, and the immutable LocalCloud candidate. A default mock output is not provider parity.


#### schema.udfs — User-defined functions

- **Classification:** partial; applicability: local-development; verification label: tested.
- **Current matrix note:** SQL UDFs (CREATE [OR REPLACE] FUNCTION, DROP FUNCTION) work in both dialects, including default parameters, invoker security, dependency and cycle checks, and use in views, defaults, generated columns and indexes. Remote UDFs return deterministic pseudo-random values unless --remote_functions_host_port points at a local HTTP backend. gateway_main forwards that flag; its native test and binary build pass. A public GoogleSQL gRPC fixture reached the configured provider and propagated a provider error; evaluator tests cover distinct request IDs and malformed provider responses. The packaged LocalCloud path has not been qualified.
- **Plan exit check to challenge:** SQL UDF lifecycle plus configured remote UDF success/error through the packaged gateway.
- **Cited code:** backend/schema/updater/schema_updater.cc, backend/query/queryable_udf.cc, backend/query/remote_udf/remote_udf_evaluator.cc, binaries/gateway_main.go.
- **Cited tests:** backend/schema/updater/schema_updater_tests/udf.cc, tests/conformance/cases/udfs.cc, backend/query/remote_udf/remote_udf_evaluator_test.cc, backend/query/query_engine_test.cc, gateway/udf_provider_test.go.
- **Official contract leads:** [official reference 1](https://cloud.google.com/spanner/docs/data-definition-language).


#### ml.ml_predict — ML.PREDICT

- **Classification:** partial; applicability: local-development; verification label: tested.
- **Current matrix note:** ML.PREDICT (GoogleSQL), spanner.ml_predict_row (PostgreSQL) and AI.IF/AI.SCORE/AI.CLASSIFY execute and validate model inputs. By default they return deterministic fake outputs derived from a fingerprint of the inputs; configured calls use a local HTTP backend via --remote_functions_host_port. A native public gRPC fixture passed input-dependent predictions in both dialects and provider-error propagation through gateway_main with the forwarded flag. Provider predictions through an immutable packaged LocalCloud image have not been qualified.
- **Plan exit check to challenge:** Both dialects receive provider-produced predictions and correct provider errors in packaged LocalCloud.
- **Cited code:** backend/query/ml/ml_predict_table_valued_function.cc, backend/query/ml/ml_predict_row_function.cc, backend/query/ml/model_evaluator.cc.
- **Cited tests:** backend/query/ml/model_evaluator_test.cc, backend/query/query_engine_test.cc, tests/conformance/cases/query.cc, gateway/ml_provider_test.go.
- **Official contract leads:** [official reference 1](https://cloud.google.com/spanner/docs/ml).


#### ml.remote_models — Remote model operations

- **Classification:** partial; applicability: compatibility-only; verification label: tested.
- **Current matrix note:** Remote model and remote UDF calls return deterministic mock results unless --remote_functions_host_port=host:port is configured, which POSTs requests to that local HTTP server. gateway_main forwards the flag, and a native public gRPC fixture passed input-dependent GoogleSQL and PostgreSQL predictions plus a provider error. LocalCloud configuration and wrapper source exists, but no immutable packaged image has passed a provider SDK workflow; Vertex AI is not called.
- **Plan exit check to challenge:** Configured model/UDF calls use a real provider binding through restart; no fake successful fallback.
- **Cited code:** backend/query/ml/model_evaluator.cc, backend/query/remote_udf/remote_udf_evaluator.cc.
- **Cited tests:** backend/query/ml/model_evaluator_test.cc, backend/query/remote_udf/remote_udf_evaluator_test.cc, backend/query/query_engine_test.cc, gateway/ml_provider_test.go.
- **Official contract leads:** [official reference 1](https://cloud.google.com/spanner/docs/ml).


### Plans and statistics (2)

**Shared audit probe:** A placeholder plan and an empty statistics table do not count. Measure local operators/queries/reads/transactions/locks, validate structure, retention, and role visibility, and label estimates as local rather than Cloud performance.


#### googlesql.query_modes — Query modes and plans

- **Classification:** partial; applicability: local-development; verification label: tested.
- **Current matrix note:** PLAN mode analyzes without executing and returns result and parameter metadata with a single placeholder plan node, which lets clients infer parameter types. PROFILE mode executes and returns only rows_returned and elapsed_time. No real query plans or per-operator statistics are produced, and elapsed_time says nothing about production latency.
- **Plan exit check to challenge:** PLAN/PROFILE return meaningful local operator structure and measured profile data.
- **Cited code:** frontend/handlers/queries.cc, backend/query/query_engine.cc.
- **Cited tests:** tests/conformance/cases/query_modes.cc, backend/query/query_engine_test.cc.
- **Official contract leads:** [official reference 1](https://cloud.google.com/spanner/docs/google-sql-reference).


#### metadata.spanner_sys — SPANNER_SYS

- **Classification:** partial; applicability: local-development; verification label: tested.
- **Current matrix note:** Only SPANNER_SYS.SUPPORTED_OPTIMIZER_VERSIONS exists (one row). Query, read, transaction and lock statistics, oldest active queries and other introspection tables are absent, so queries against them fail.
- **Plan exit check to challenge:** At least the named query/read/transaction/lock tables report measured local rows and enforce role visibility; other locally meaningful families are inventoried and closed.
- **Cited code:** backend/query/spanner_sys_catalog.cc.
- **Cited tests:** tests/conformance/cases/spanner_sys.cc.
- **Official contract leads:** [official reference 1](https://cloud.google.com/spanner/docs/introspection).


### Unclassified client surface (1)

**Shared audit probe:** Run current JDBC and PGAdapter against the actual endpoint, including DDL, prepared query, transaction, types, metadata, and errors. Attribute failures to the driver, adapter, native emulator, or packaging.


#### clients.jdbc — JDBC compatibility

- **Classification:** unknown; applicability: compatibility-only; verification label: unverified.
- **Current matrix note:** No JDBC tests exist in this repository. The JDBC driver and PGAdapter (README) connect through the gRPC endpoint; compatibility is unverified.
- **Plan exit check to challenge:** Classify only after a versioned JDBC and PGAdapter public-endpoint workflow; no current in-repo acceptance result.
- **Cited code:** none cited.
- **Cited tests:** none cited.
- **Official contract leads:** [official reference 1](https://cloud.google.com/spanner/docs/emulator).


## Audit the other 106 records too

The following IDs have current statuses of supported, accepted-no-op, or not-applicable. Review **each** against its full canonical note, cited source/tests, official contract and public behavior; put it in the final 152-row worksheet. In particular, try to falsify the five recent promotions, any broad supported claim with a caveat in its note, intentional accepted no-ops that might hide an observable effect, and physical not-applicable classifications that might contain local functionality. Do not treat the list as prevalidated.


### supported (89)

- data.sessions.create — CreateSession RPC
- data.sessions.get — GetSession RPC
- data.sessions.list — ListSessions RPC
- data.sessions.delete — DeleteSession RPC
- data.sessions.batch_create — BatchCreateSessions RPC
- data.sql.execute — ExecuteSql RPC
- data.sql.execute_streaming — ExecuteStreamingSql RPC
- data.read.read — Read RPC
- data.read.streaming_read — StreamingRead RPC
- data.sql.partition_query — PartitionQuery RPC
- data.read.partition_read — PartitionRead RPC
- data.batch.create_transaction — Transaction selectors and inline begin
- instance.create — CreateInstance RPC
- instance.get — GetInstance RPC
- instance.list — ListInstances RPC
- instance.update — UpdateInstance RPC
- instance.delete — DeleteInstance RPC
- database.create — CreateDatabase RPC
- database.get — GetDatabase RPC
- database.list — ListDatabases RPC
- database.drop — DropDatabase RPC
- database.update_ddl — UpdateDatabaseDdl RPC
- operations.get — GetOperation RPC
- operations.list — ListOperations RPC
- schema.tables — CREATE TABLE
- schema.alter_table — ALTER TABLE
- schema.drop_table — DROP TABLE
- schema.views — Views
- schema.named_schemas — Named schemas
- schema.sequences — Sequences
- schema.placements — Geo-partitioning placements and placement keys
- types.bool_int_float — BOOL, INT64, FLOAT32, FLOAT64
- types.numeric — NUMERIC
- types.arrays — ARRAY
- types.structs — STRUCT
- types.proto — PROTO and ENUM
- types.interval — INTERVAL
- googlesql.select — SELECT queries
- googlesql.joins — Joins
- googlesql.subqueries — Subqueries
- googlesql.parameters — Query parameters
- googlesql.tablesample — TABLESAMPLE
- postgresql.create_database — PostgreSQL dialect databases
- postgresql.synonyms — PostgreSQL synonyms
- writes.mutations_insert — Insert, InsertOrUpdate, and Replace mutations
- writes.mutations_update — Update mutations
- writes.mutations_delete — Delete mutations
- writes.dml — DML statements
- writes.batch_dml — Batch DML
- writes.batch_write — BatchWrite RPC
- transactions.read_write — Read-write transactions
- transactions.read_only — Read-only transactions
- transactions.commit_timestamps — Commit timestamps
- transactions.read_your_writes — Read-your-writes
- indexes.secondary_indexes — Secondary indexes
- indexes.unique_indexes — Unique indexes
- indexes.foreign_keys — Foreign keys
- indexes.check_constraints — Check constraints
- indexes.generated_columns — Generated columns
- indexes.identity_columns — Identity columns
- indexes.column_defaults — Column defaults
- indexes.on_update — ON UPDATE columns
- change_streams.create — Create change stream
- change_streams.exclusions — Change stream exclusions
- vector.vector_type — Vector columns (ARRAY<FLOAT32/FLOAT64> with vector_length)
- vector.distance_functions — Vector distance functions
- graph.ddl — Graph DDL
- graph.gql_queries — GQL queries
- graph.graph_schema — Graph schema metadata
- ml.models — MODEL schema objects
- metadata.operation_metadata — Operation metadata
- clients.grpc_endpoint — gRPC endpoint compatibility
- clients.cli — gcloud emulator usage
- clients.docker — Docker image usage
- emulator.in_memory — In-memory operation
- emulator.persistence — Local persistence
- emulator.startup — Startup and ports
- emulator.packaging — Local binaries and Docker packaging
- database.update — UpdateDatabase RPC
- backups.metadata — Backup metadata lifecycle RPCs
- operations.lifecycle — DeleteOperation and WaitOperation RPCs
- transactions.lifecycle — BeginTransaction, Commit, and Rollback RPCs
- data.sessions.multiplexed — Multiplexed sessions
- emulator.restore_fault_isolation — Per-database restore fault isolation
- types.string_bytes_date_timestamp — STRING, BYTES, DATE, TIMESTAMP
- types.uuid — UUID
- writes.upsert_dml — INSERT OR IGNORE / INSERT OR UPDATE and ON CONFLICT DML
- writes.dml_returning — THEN RETURN / RETURNING
- schema.interleaving — Interleaved tables (INTERLEAVE IN PARENT / INTERLEAVE IN)


### accepted-no-op (11)

- operations.cancel — CancelOperation RPC
- googlesql.hints — Query hints
- vector.vector_query_options — Vector query options
- graph.algorithms — Graph algorithms
- security.fine_grained_access — Fine-grained access control
- ops.leader_options — Default leader options
- data.cache_updates — FetchCacheUpdate RPC
- database.split_points — AddSplitPoints RPC
- data.directed_reads_data_boost — Directed reads and Data Boost request options
- schema.row_deletion_policy — Row deletion policies (TTL)
- schema.locality_groups — Locality groups (tiered storage)


### not-applicable (6)

- ml.external_connections — External connections
- security.encryption — Customer-managed encryption
- security.audit_logs — Cloud audit logs
- ops.replication — Replication and multi-region serving
- ops.autoscaling — Autoscaling
- ops.monitoring — Cloud monitoring metrics


## Verification procedure and evidence labels

1. Record git HEAD, branch, dirty paths, matrix hash, test binary and image provenance before running checks. This is a shared dirty tree: preserve unrelated changes. Do not run simultaneous Bazel builds against its shared output directory.
2. Start each row with current official contract and a counterexample that would disprove the claimed status. Use a focused public test when behavior crosses API boundaries. For stateful claims, use a disposable --data_dir and a real stop/restart; an in-memory metadata replay is a narrower tier.
3. Reuse the flat offline archive cache, repository cache and disk cache for Bazel. Configuration of these caches does not prove zero network access. Run focused targets and then the full native conformance suite only when needed to check integrated changes. If an implementation follow-up edits YAML, regenerate Markdown and run the coverage checks.
4. For packaged claims, build an immutable Spanner image and a LocalCloud candidate tied to its exact Spanner image ID; test the published gRPC and REST endpoints, direct SDK flows, restart with disposable volumes, and provider/identity/KMS outage paths where applicable. A local image ID is enough for local qualification; a registry digest matters on publication. Do not use shared production data or a live cloud project.

~~~sh
bazel test //path/to:focused_test \
  --distdir="$PWD/bazel-distdir" \
  --repository_cache="$PWD/bazel-distdir" \
  --disk_cache=/Users/jsenjaliya/.cache/bazel-disk/cloud-spanner-emulator \
  --test_output=errors
python3 tools/feature_coverage.py audit-rpcs
python3 tools/feature_coverage.py check
python3 -m unittest tools.feature_coverage_test
~~~

The full native conformance target is //tests/conformance/endpoints:emulator_conformance_test with the same cache flags. The generation command, for a separately authorized inventory change, is python3 tools/feature_coverage.py generate before check. Label each result with the strongest **actual** tier: source inspected; test exists but unrun; focused native passed; full native suite passed; disposable native restart passed; packaged Spanner image passed; immutable LocalCloud candidate passed. Record command, date, pass/fail/skips, dialect, port, and image or binary identity.

## Required report

Deliver a **152-row worksheet** with one row per canonical feature ID. Required columns: current status and verification, exact locally observable contract, official source and retrieval date, source and test paths, executed command/result or “not run,” dialect/API/persistence/image tier, strongest positive evidence, counterexample or missing behavior, note accuracy, proposed status or split, confidence, smallest failing acceptance test, and owner/dependency. Distinguish implementation defect, test gap, docs gap, packaging gap, Cloud-only property, and pinned-proto/version gap.

For the 46 open rows above, give individual findings, not a category verdict. Summarize current versus proposed counts including any splits, contradictions among YAML/generated Markdown/plan/user docs, P0/P1/P2 closure order, sister-project reuse with protocol/license/version caveats, and a reproducible next batch. If the audit is incomplete, enumerate every unreviewed ID and tier. Never silently mark it passing.

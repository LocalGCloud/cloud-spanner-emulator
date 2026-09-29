# Developer-usability audit worksheet (2026-09-27)

Evaluation of all 152 records in `docs/feature-coverage.yaml` (sha256 `4dc7b64d…`) against the maintainer's definition: **supported = a developer can use the feature locally with correct, documented, observable behavior through the public API**. Physical production properties (latency, replicas, capacity, private relevance/ANN internals) are split out, not counted against local support. This is the **pre-implementation** evaluation; the closure plan is `2026-09-27-usability-closure-plan.md`, and the final statuses are in the coverage YAML.

## Evidence gathered

| Evidence | Command / scope | Result |
| --- | --- | --- |
| Coverage tooling | `python3 tools/feature_coverage.py audit-rpcs`; `check`; `python3 -m unittest tools.feature_coverage_test` | RPC map complete; generated Markdown in sync; 22/22 tests pass |
| Counts recomputed from YAML | 152 records | 89 supported, 44 partial, 11 accepted-no-op, 6 not-applicable, 1 unsupported, 1 unknown (matches the checkpoint) |
| Full native sweep (baseline dirty tree `8a0a2613` snapshot) | fixed-env `bazel test -c opt //tests/conformance/... //backend/... //frontend/... //gateway/... //common/...` | 138 targets: 135 pass, 3 fail. Failures: `query_engine_test` TestMlQuery_Http (both dialects) and `model_evaluator_test` RemotePredictSuccess/PgPredictRemote pin the old constant `requestId`; 7 conformance shards fail on 6 GoogleSQL search cases (regression from the SQL-NULL tokenizer change: empty TOKENLIST_CONCAT crash, SEARCH on SQL NULL returns TRUE for NOT queries, numeric-tokenizer check lost for NULL columns). 1,984 s, 3,987 disk-cache hits. |
| Client SDK/driver matrix (binary sha256 `e7c9ba45…`, gateway `47652e0f…`) | Python google-cloud-spanner 3.71.0, Go v1.95.1, Node 9.0.0, Java 6.123.0, JDBC 2.45.0, PGAdapter 0.55.3 + pgJDBC 42.7.13 + psql 17.5, REST via curl | All pass (16/16, 16/16, 16/16, 16/16, 18/18, 13/13, 28/28): admin, DDL, mutations, DML, params, snapshot and stale reads, batch DML, error codes, PG dialect, multiplexed sessions. REST error body uses the gRPC-gateway shape, not Google's `{"error":…}` envelope. |
| Official doc-example conformance | Every self-contained example on the Spanner GoogleSQL function pages and PG function/operator pages (retrieved 2026-09-27), run against the same binary | 1,165 examples: 1,015 pass, 35 mismatch, 35 error, 80 skipped. 38 genuine defects in 11 functions: PG `jsonb || jsonb` array order, PG `->`/`->>` negative index, SNIPPET JSON layout, SPLIT_SUBSTR (both dialects), ZSTD_*, PG generate_series, PG make_interval, `pg.ilike`, TO/FROM_BASE32, DEBUG_TOKENLIST, PG `!~~`. Remaining mismatches are order-unspecified results or documentation errors. |
| Official contracts | FGAC/INFORMATION_SCHEMA, SPANNER_SYS/plans, isolation, backups, search DDL, change streams, commit stats, TTL, PDML, list filters, GoogleSQL and PG function inventories | Retrieved 2026-09-27 from docs.cloud.google.com (pages "Last updated 2026-09-24") |

Evidence tiers used below: **SRC** source inspected; **UNRUN** test exists, not run; **FOC** focused native passed; **FULL** full native suite passed; **RST** disposable native restart passed; **NPE** native public endpoint (external SDKs/drivers); **PKG** packaged image; **LC** LocalCloud candidate.

## Open records (46): individual findings

### `instance.configs` — Instance configs RPCs

- **Current:** partial / compatibility-only / tested
- **Local contract:** Custom instance config CRUD with validation, etags, typed operations, persistence; operation list filters.
- **Official source:** https://cloud.google.com/spanner/docs/reference/rpc/google.spanner.admin.instance.v1#instanceadmin (reviewed 2026-09-27)
- **Implementation and tests:** instances_test, instance_extensions_test, gcloud tests.
- **Strongest verified tier:** full native suite passed
- **Gap / counterexample:** impl: ListInstanceConfigOperations accepts only one simple predicate (compound AIP-160 filters fail). cloud: replicas/regions metadata only.
- **Proposed status:** supported (after F1) — confidence high
- **Smallest acceptance test:** Compound filter (metadata.@type=... AND done=true) returns matching operations.
- **Plan task:** F1

### `backups.create` — CreateBackup RPC

- **Current:** partial / local-development / tested
- **Local contract:** CreateBackup with --data_dir: point-in-time copy, expire_time 6h..366d, version_time within [earliest_version_time, now], encryption_config GOOGLE_DEFAULT_ENCRYPTION/USE_DATABASE_ENCRYPTION reported in encryption_info.
- **Official source:** https://cloud.google.com/spanner/docs/reference/rpc/google.spanner.admin.database.v1#databaseadmin (reviewed 2026-09-27)
- **Implementation and tests:** backups.cc, backup_catalog.cc; CreateCheckpoint copies all versions (persistent_storage.cc:306-329).
- **Strongest verified tier:** full native suite passed
- **Gap / counterexample:** impl: version_time → INVALID_ARGUMENT; any encryption_config → UNIMPLEMENTED (even Google-default types). CMEK stays cloud-only (security.encryption).
- **Proposed status:** partial now; supported after E3 — confidence high
- **Smallest acceptance test:** Write v1, note t1, write v2, CreateBackup(version_time=t1), restore → reads v1; encryption_config GOOGLE_DEFAULT accepted and reported; CMEK rejected.
- **Plan task:** E3

### `backups.restore` — RestoreDatabase RPC

- **Current:** partial / local-development / tested
- **Local contract:** RestoreDatabase copies a READY backup to a new database with data/schema/options/sequence counters; change stream definitions restored without history; rollback on failure; default encryption types accepted.
- **Official source:** https://cloud.google.com/spanner/docs/reference/rpc/google.spanner.admin.database.v1#databaseadmin (reviewed 2026-09-27)
- **Implementation and tests:** backups_test, database_manager_test, query_engine_test sequence restore.
- **Strongest verified tier:** full native suite passed
- **Gap / counterexample:** impl (minor): restore does not drop row deletion policies (production does) — moot until TTL exists (E5); encryption_config ignored rather than validated.
- **Proposed status:** supported (E3/E5 polish) — confidence high
- **Smallest acceptance test:** Existing backup lifecycle tests + E3 encryption validation.
- **Plan task:** E3, E5

### `schema.udfs` — User-defined functions

- **Current:** partial / local-development / tested
- **Local contract:** SQL UDF lifecycle both dialects; remote UDFs return deterministic local values by default or call --remote_functions_host_port provider with error propagation.
- **Official source:** https://cloud.google.com/spanner/docs/data-definition-language (reviewed 2026-09-27)
- **Implementation and tests:** udfs.cc, udf updater tests, remote_udf_evaluator_test, gateway udf_provider_test (public gRPC fixture).
- **Strongest verified tier:** focused native passed
- **Gap / counterexample:** pkg: LocalCloud packaged path unqualified (separate project tier).
- **Proposed status:** supported — confidence high
- **Smallest acceptance test:** Existing tests.
- **Plan task:** none

### `types.json` — JSON type and functions

- **Current:** partial / local-development / tested
- **Local contract:** JSON column/ARRAY<JSON>/key restrictions; every Spanner-documented JSON function (JSON_QUERY/VALUE[_ARRAY], JSON_QUERY_ARRAY, JSON_ARRAY/OBJECT, JSON_SET/REMOVE/STRIP_NULLS/ARRAY_APPEND/ARRAY_INSERT, JSON_KEYS/TYPE/CONTAINS, BOOL/INT64/FLOAT32/FLOAT64/STRING[_ARRAY], LAX_*, PARSE_JSON, TO_JSON, SAFE_TO_JSON, TO_JSON_STRING, subscripts) evaluates as documented; JSON null distinct from SQL NULL.
- **Official source:** https://cloud.google.com/spanner/docs/data-types (reviewed 2026-09-27)
- **Implementation and tests:** Allowlist gsql_supported_functions.cc covers every documented JSON function and the pinned GoogleSQL evaluator implements all of them (function inventory 2026-09-27). Value tests: query.cc JSON cases; QET:3851-3928 callable-only.
- **Strongest verified tier:** full native suite passed
- **Gap / counterexample:** test: JSON_ARRAY, JSON_OBJECT, JSON_QUERY_ARRAY, JSON_VALUE_ARRAY have no value test; JSON null vs SQL NULL untested. No missing function.
- **Proposed status:** supported (after A2 value tests) — confidence high
- **Smallest acceptance test:** Conformance: each documented JSON function returns its documented example value; JSON 'null' vs SQL NULL distinguished.
- **Plan task:** A2

### `googlesql.query_modes` — Query modes and plans

- **Current:** partial / local-development / tested
- **Local contract:** PLAN returns a plan tree + metadata without executing; PROFILE executes and returns plan with measured stats; streaming returns plan/stats on the last message.
- **Official source:** https://cloud.google.com/spanner/docs/google-sql-reference (reviewed 2026-09-27)
- **Implementation and tests:** queries.cc returns one 'No query plan' node; PROFILE rows_returned/elapsed_time only.
- **Strongest verified tier:** full native suite passed
- **Gap / counterexample:** impl: no operator tree; streaming path returns no plan and puts PROFILE stats on the first message.
- **Proposed status:** partial now; supported after D2 — confidence high
- **Smallest acceptance test:** PLAN of 'SELECT * FROM T WHERE k=1' has Serialize Result → Distributed Union → Filter Scan/Scan(T); PROFILE scan node rows == 1.
- **Plan task:** D2 (stats agent)

### `googlesql.aggregate_functions` — Aggregate functions

- **Current:** partial / local-development / tested
- **Local contract:** ANY_VALUE, ARRAY_AGG, ARRAY_CONCAT_AGG, AVG, BIT_AND/OR/XOR, COUNT, COUNTIF, LOGICAL_AND/OR, MAX, MIN, STRING_AGG, SUM, STDDEV[_SAMP], VAR_SAMP, VARIANCE with NULL/empty/overflow/window semantics.
- **Official source:** https://cloud.google.com/spanner/docs/google-sql-reference (reviewed 2026-09-27)
- **Implementation and tests:** All documented aggregates are allowlisted and aggregate-filtered (sql_feature_filter.cc:65-91) and implemented by the reference evaluator.
- **Strongest verified tier:** full native suite passed
- **Gap / counterexample:** test: most aggregates lack value tests (only STDDEV/VARIANCE, ARRAY_AGG, STRING_AGG covered). No missing function.
- **Proposed status:** supported (after A2) — confidence high
- **Smallest acceptance test:** Table-driven conformance over a fixed table: each aggregate incl. empty input, all-NULL, INT64 overflow error, IGNORE NULLS/ORDER BY/LIMIT modifiers.
- **Plan task:** A2

### `googlesql.string_functions` — String functions

- **Current:** partial / local-development / tested
- **Local contract:** Every Spanner-documented string function evaluates as documented, including bytes variants, Unicode, regex, TO/FROM_BASE32, SPLIT_SUBSTR, LCASE/UCASE aliases, and ZSTD compression functions.
- **Official source:** https://cloud.google.com/spanner/docs/google-sql-reference (reviewed 2026-09-27)
- **Implementation and tests:** Allowlist + evaluator; query.cc covers FORMAT, regex errors, aliases.
- **Strongest verified tier:** full native suite passed
- **Gap / counterexample:** impl: TO_BASE32/FROM_BASE32 are allowlisted but fail at execution with UNIMPLEMENTED (open-source evaluator lacks base32) — docs claim they work; SPLIT_SUBSTR rejected by allowlist; LCASE/UCASE not registered; ZSTD_COMPRESS/ZSTD_DECOMPRESS_* rejected and the open-source implementation is a placeholder. test: most functions untested.
- **Proposed status:** partial now; supported after A1+A2 — confidence high
- **Smallest acceptance test:** SELECT TO_BASE32(b'abcde\xFF') = 'MFRGGZDF74======' and FROM_BASE32 round-trip; SPLIT_SUBSTR('www.abc.xyz.com', '.', 1, 3); LCASE/UCASE; ZSTD_DECOMPRESS_TO_STRING(ZSTD_COMPRESS('zstd')).
- **Plan task:** A1, A2

### `googlesql.date_timestamp_functions` — Date and timestamp functions

- **Current:** partial / local-development / tested
- **Local contract:** Documented DATE/TIMESTAMP/INTERVAL functions incl. ADDDATE/SUBDATE aliases, time zones (default America/Los_Angeles or default_time_zone), DST, ISO week parts, UNIX_*/TIMESTAMP_* converters, interval arithmetic.
- **Official source:** https://cloud.google.com/spanner/docs/google-sql-reference (reviewed 2026-09-27)
- **Implementation and tests:** All non-alias functions allowlisted and implemented; default_time_zone.cc and query.cc cover time zones and ISO parts.
- **Strongest verified tier:** full native suite passed
- **Gap / counterexample:** impl: ADDDATE/SUBDATE aliases missing. test: CURRENT_DATE, UNIX_*, TIMESTAMP_MICROS/MILLIS, GENERATE_DATE_ARRAY, PARSE_DATE, interval functions untested.
- **Proposed status:** supported (after A1 alias + A2) — confidence high
- **Smallest acceptance test:** ADDDATE(DATE '2024-01-31', INTERVAL 1 MONTH); UNIX_MILLIS/TIMESTAMP_MILLIS round trip; JUSTIFY_*; DST boundary TIMESTAMP_ADD in America/Los_Angeles.
- **Plan task:** A1, A2

### `postgresql.queries` — PostgreSQL query translation

- **Current:** partial / local-development / tested
- **Local contract:** Spanner PostgreSQL query grammar and documented function/operator subset translate and evaluate correctly.
- **Official source:** https://cloud.google.com/spanner/docs/postgresql-interface (reviewed 2026-09-27)
- **Implementation and tests:** spanner_pg transformer + emulator_postgresql_catalog.textproto map ~270 PG names; pg_functions_test.cc; previously disabled SUM/AVG empty and ARRAY_AGG ORDER BY cases enabled in the working tree.
- **Strongest verified tier:** full native suite passed
- **Gap / counterexample:** impl: generate_series unmapped (implementation exists); spanner.split_substr missing; !~~ and NOT LIKE/NOT ILIKE ... ESCAPE fail; array_agg(uuid) signature missing; regexp_replace replaces all matches (docs: first). test: many documented functions have unit tests only.
- **Proposed status:** partial now; supported after A3 — confidence medium
- **Smallest acceptance test:** PG conformance: SELECT * FROM generate_series(1,3); spanner.split_substr; 'a_b' NOT LIKE 'a\_c' ESCAPE '\'; array_agg(uuid col); regexp_replace first-match per docs.
- **Plan task:** A3

### `postgresql.pg_catalog` — pg_catalog views

- **Current:** partial / local-development / tested
- **Local contract:** Documented pg_catalog tables/views return rows tracking schema, UDF and view changes; documented no-content columns/tables stay empty (pg_roles has no content per docs).
- **Official source:** https://cloud.google.com/spanner/docs/postgresql-interface (reviewed 2026-09-27)
- **Implementation and tests:** pg_catalog.cc populates pg_attrdef/attribute/class/constraint/index/indexes/namespace/proc/sequence(s)/tables/views from the schema.
- **Strongest verified tier:** full native suite passed
- **Gap / counterexample:** impl: pg_proc.proname uses the schema-qualified name for UDFs in named schemas (pg_catalog.cc:2007). test: PGProc_UDFs unconditionally skipped since an upstream import.
- **Proposed status:** supported (after A8) — confidence high
- **Smallest acceptance test:** Enable PGProc_UDFs: CREATE FUNCTION in default and named schema → pg_proc rows with unqualified proname, correct pronamespace.
- **Plan task:** A8 (FGAC agent)

### `postgresql.views` — PostgreSQL views

- **Current:** partial / local-development / tested
- **Local contract:** CREATE [OR REPLACE] VIEW v SQL SECURITY {INVOKER|DEFINER} AS query and DROP VIEW; DEFINER views need only SELECT on the view under a database role.
- **Official source:** https://cloud.google.com/spanner/docs/postgresql-interface (reviewed 2026-09-27)
- **Implementation and tests:** views.cc, pg_views_test.cc; translator hardcodes INVOKER.
- **Strongest verified tier:** full native suite passed
- **Gap / counterexample:** impl: PostgreSQL SQL SECURITY DEFINER (documented in the Spanner PG DDL reference) not translated; privilege effect needs FGAC. Column lists/TEMP/WITH CHECK OPTION are not in Spanner's PG grammar (correctly rejected).
- **Proposed status:** supported (after A12 + C2) — confidence high
- **Smallest acceptance test:** PG CREATE VIEW ... SQL SECURITY DEFINER appears as DEFINER in information_schema.views; restricted role with SELECT on view only can query DEFINER view but not an INVOKER view.
- **Plan task:** A12, C2

### `postgresql.change_streams` — PostgreSQL change streams

- **Current:** partial / local-development / tested
- **Local contract:** PG CREATE/ALTER/DROP CHANGE STREAM (all value_capture_type values, SET NULL/RESET), spanner.read_json_<stream> JSONB records with shared record semantics.
- **Official source:** https://cloud.google.com/spanner/docs/postgresql-interface (reviewed 2026-09-27)
- **Implementation and tests:** pg_change_streams_read_write.cc, converter and updater tests.
- **Strongest verified tier:** full native suite passed
- **Gap / counterexample:** impl: shares the DML UPDATE record-content defect (E1).
- **Proposed status:** supported (after E1) — confidence high
- **Smallest acceptance test:** PG variant of the UPDATE record-content case.
- **Plan task:** E1

### `postgresql.jsonb` — PostgreSQL JSONB type and functions

- **Current:** partial / local-development / tested
- **Local contract:** JSONB type and documented operators/functions (->, ->>, @>, <@, ?, ?|, ?&, ||, -, #-, jsonb_build_*, jsonb_set[_lax], jsonb_insert, jsonb_strip_nulls, jsonb_object_keys, jsonb_array_elements, jsonb_typeof, to_jsonb, spanner.jsonb_query_array, spanner.*_array) with numeric limits.
- **Official source:** https://cloud.google.com/spanner/docs/reference/postgresql/data-types (reviewed 2026-09-27)
- **Implementation and tests:** All documented JSONB functions/operators are implemented (emulator_functions.cc); parse/normalization unit tests; PGF conformance for core operators.
- **Strongest verified tier:** full native suite passed
- **Gap / counterexample:** test: write functions and spanner.*_array extractors have unit-only or no tests. No missing function found.
- **Proposed status:** supported (after A4 tests) — confidence high
- **Smallest acceptance test:** PG conformance for jsonb_set/jsonb_insert/jsonb_strip_nulls/||/-/#- and spanner.int64_array/string_array incl. documented error cases.
- **Plan task:** A4

### `writes.partitioned_dml` — Partitioned DML

- **Current:** partial / local-development / tested
- **Local contract:** PDML UPDATE/DELETE with parameters in both dialects; INSERT and non-partitionable statements rejected; reads rejected; REPEATABLE_READ rejected; row_count_lower_bound returned.
- **Official source:** https://cloud.google.com/spanner/docs/dml-syntax (reviewed 2026-09-27)
- **Implementation and tests:** partitioned_dml.cc, partitioned_dml_validator_test.cc.
- **Strongest verified tier:** full native suite passed
- **Gap / counterexample:** cloud: per-partition distributed execution/retries/throughput. Local execution is atomic, a stricter subset whose final state matches an idempotent statement's production result.
- **Proposed status:** supported — confidence high
- **Smallest acceptance test:** Existing partitioned_dml conformance (both dialects).
- **Plan task:** none

### `transactions.stale_reads` — Stale reads

- **Current:** partial / local-development / tested
- **Local contract:** Exact/bounded staleness and exact/min timestamps within version_retention_period; older reads FAILED_PRECONDITION; future exact timestamps wait (production: 'Spanner will wait'), >1h rejected like production's deadline.
- **Official source:** https://cloud.google.com/spanner/docs/transactions (reviewed 2026-09-27)
- **Implementation and tests:** snapshot_reads.cc; LockManager::WaitForSafeRead waits for future timestamps; reads.cc/queries.cc 1h guard.
- **Strongest verified tier:** full native suite passed
- **Gap / counterexample:** impl (minor): a waiting future read ignores the client's gRPC deadline. cloud: replica selection for bounded staleness.
- **Proposed status:** supported (B4 deadline cap optional) — confidence high
- **Smallest acceptance test:** Existing snapshot_reads; optional: exact timestamp now+2s with 1s deadline → DEADLINE_EXCEEDED.
- **Plan task:** B4

### `transactions.select_for_update` — SELECT FOR UPDATE

- **Current:** partial / local-development / tested
- **Local contract:** FOR UPDATE takes exclusive locks in read-write transactions, conflicts with overlapping writers without blocking unrelated keys; rejected in read-only transactions and with lock_scanned_ranges; PG rejects set ops/GROUP BY.
- **Official source:** https://cloud.google.com/spanner/docs/transactions (reviewed 2026-09-27)
- **Implementation and tests:** select_for_update.cc (20 cases), key/range lock manager.
- **Strongest verified tier:** full native suite passed
- **Gap / counterexample:** impl: lock_scanned_ranges=exclusive hint only sets a flag, never takes exclusive locks. test: broad-scan vs DML conflict.
- **Proposed status:** supported (after B2) — confidence high
- **Smallest acceptance test:** Txn A: SELECT ... FOR UPDATE on key 1; Txn B UPDATE key 2 commits; Txn B UPDATE key 1 aborts one side.
- **Plan task:** B2

### `transactions.transaction_errors` — Transaction error handling

- **Current:** partial / local-development / tested
- **Local contract:** Constraint errors invalidate further DML/commit while reads of earlier buffered writes still work; aborts reset; batch DML stops at first failure keeping the transaction usable.
- **Official source:** https://cloud.google.com/spanner/docs/transactions (reviewed 2026-09-27)
- **Implementation and tests:** transaction_errors.cc (32 cases), batch_dml.cc InvalidDmlFailsButCommitSucceeds.
- **Strongest verified tier:** full native suite passed
- **Gap / counterexample:** Minor: which of several simultaneous constraint violations is reported may differ from production (non-contractual).
- **Proposed status:** supported — confidence high
- **Smallest acceptance test:** Existing transaction_errors + batch_dml conformance.
- **Plan task:** none

### `indexes.index_backfill` — Index backfill

- **Current:** partial / local-development / tested
- **Local contract:** Adding an index to a populated table backfills it (unique/NULL_FILTERED/STORING/interleaved) with duplicate/size validation before the LRO completes.
- **Official source:** https://cloud.google.com/spanner/docs/secondary-indexes (reviewed 2026-09-27)
- **Implementation and tests:** index_backfill.cc conformance and unit tests.
- **Strongest verified tier:** full native suite passed
- **Gap / counterexample:** cloud: asynchronous progress reporting and throughput. Locally the UpdateDatabaseDdl operation completes after a synchronous backfill; clients waiting on the LRO observe the same result.
- **Proposed status:** supported — confidence high
- **Smallest acceptance test:** Existing index_backfill conformance.
- **Plan task:** none

### `change_streams.read` — Read change stream

- **Current:** partial / local-development / tested
- **Local contract:** READ_<stream> returns data change, heartbeat and child partition records with documented mod shapes per value_capture_type; REPLACE → DELETE+INSERT; monotonic record_sequence; start_timestamp retention checks; restart keeps creation time.
- **Official source:** https://cloud.google.com/spanner/docs/change-streams (reviewed 2026-09-27)
- **Implementation and tests:** change_streams_read_write.cc (8 focused cases), change_streams_test.cc.
- **Strongest verified tier:** full native suite passed
- **Gap / counterexample:** impl: DML UPDATE records every tracked column (BuildUpdate writes the full row) instead of the updated columns — the DISABLED_MultipleDMLVerifyDataChangeRecordContent test (docs blame INSERT incorrectly). Resume tokens are placeholders (change stream clients resume by partition token + timestamp). cloud: in-flight query survival across restart.
- **Proposed status:** supported (after E1) — confidence high
- **Smallest acceptance test:** Enable DISABLED_MultipleDMLVerifyDataChangeRecordContent: UPDATE SET c1 records only c1 (+keys) under OLD_AND_NEW_VALUES.
- **Plan task:** E1

### `change_streams.key_ranges` — Change stream key ranges

- **Current:** partial / local-development / tested
- **Local contract:** MUTABLE_KEY_RANGE streams return partition_start/partition_end/partition_event (move_in/move_out) and data change records in both dialects.
- **Official source:** https://cloud.google.com/spanner/docs/change-streams (reviewed 2026-09-27)
- **Implementation and tests:** change_streams_mutable_key_range.cc; churner skips MOVE for mutable streams (churner.cc:210-215).
- **Strongest verified tier:** full native suite passed
- **Gap / counterexample:** impl: MOVE partition_event records never emitted, so client code for move_in/move_out is never exercised; converter returns InternalError if one arrived.
- **Proposed status:** partial now; supported after E2 — confidence medium
- **Smallest acceptance test:** With short token lifetime, a mutable stream emits partition_event with move_out/move_in pairs at the same commit timestamp; records after the move arrive on the destination partition.
- **Plan task:** E2

### `change_streams.partition_churn` — Partition churn

- **Current:** partial / local-development / tested
- **Local contract:** Background churn produces child partition records (split/merge) with heartbeats; token lifetime configurable; initial partitions not duplicated after restart.
- **Official source:** https://cloud.google.com/spanner/docs/change-streams (reviewed 2026-09-27)
- **Implementation and tests:** churner_test, database_manager_test replay.
- **Strongest verified tier:** full native suite passed
- **Gap / counterexample:** cloud: load-driven churn and multi-partition write routing (all writes go to one active partition); client-visible record protocol is complete.
- **Proposed status:** supported — confidence medium
- **Smallest acceptance test:** Existing churner and replay tests.
- **Plan task:** none

### `search.search_indexes` — Search indexes

- **Current:** partial / local-development / tested
- **Local contract:** CREATE/ALTER/DROP SEARCH INDEX (ALTER ADD/DROP [STORED] COLUMN; PG ADD/DROP [INCLUDE] COLUMN), STORING, PARTITION BY/ORDER BY, options, hints, consistent indexed reads.
- **Official source:** https://cloud.google.com/spanner/docs/full-text-search (reviewed 2026-09-27)
- **Implementation and tests:** search_test.cc, parser/updater tests.
- **Strongest verified tier:** focused native passed (cited suite regressed in the baseline; see evidence table)
- **Gap / counterexample:** impl: ALTER SEARCH INDEX not parsed (GoogleSQL) / not translated (PG, grammar exists); interleaved search index without sort_order_sharding=true not rejected.
- **Proposed status:** partial now; supported after A6 — confidence high
- **Smallest acceptance test:** ALTER SEARCH INDEX idx ADD STORED COLUMN c then query uses it; DROP COLUMN tokenlist; PG ADD INCLUDE COLUMN.
- **Plan task:** A6

### `search.search_functions` — SEARCH function

- **Current:** partial / local-development / tested
- **Local contract:** SEARCH (words/words_phrase/rquery), SEARCH_SUBSTRING, SEARCH_NGRAMS, SNIPPET, JSON search; transaction restrictions and hints.
- **Official source:** https://cloud.google.com/spanner/docs/full-text-search (reviewed 2026-09-27)
- **Implementation and tests:** 182 *Search* conformance cases (GoogleSQL).
- **Strongest verified tier:** focused native passed (cited suite regressed in the baseline; see evidence table)
- **Gap / counterexample:** test: search_test.cc never sets up PostgreSQL, so PG search functions are unverified. enhance_query is accepted without Google's query enhancement (cloud/proprietary).
- **Proposed status:** supported (after A5 PG run) — confidence medium
- **Smallest acceptance test:** search_test in both dialects.
- **Plan task:** A5

### `search.tokenization` — Tokenization

- **Current:** partial / local-development / tested
- **Local contract:** TOKENIZE_* functions, TOKEN, TOKENLIST_CONCAT with Unicode segmentation, diacritics, HTML content, SQL NULL semantics.
- **Official source:** https://cloud.google.com/spanner/docs/full-text-search (reviewed 2026-09-27)
- **Implementation and tests:** Search unit targets and conformance; ICU segmentation.
- **Strongest verified tier:** focused native passed (cited suite regressed in the baseline; see evidence table)
- **Gap / counterexample:** Minor: full HTML5 parsing, script/style suppression and some language-specific segmentation differ from production's proprietary tokenizer; DEBUG_TOKENLIST missing.
- **Proposed status:** supported — confidence medium
- **Smallest acceptance test:** Existing tokenizer targets.
- **Plan task:** A5

### `search.scoring` — Search scoring

- **Current:** partial / compatibility-only / tested
- **Local contract:** SCORE/SCORE_NGRAMS return deterministic local relevance usable for ORDER BY; documented options validated (nonempty unsupported options rejected, not ignored).
- **Official source:** https://cloud.google.com/spanner/docs/full-text-search (reviewed 2026-09-27)
- **Implementation and tests:** score_evaluator_test, score_ngrams_evaluator_test.
- **Strongest verified tier:** focused native passed (cited suite regressed in the baseline; see evidence table)
- **Gap / counterexample:** cloud: production relevance model is proprietary (numeric equality is not a contract). proto: pinned signature lacks array_aggregator.
- **Proposed status:** supported (residual documented) — confidence medium
- **Smallest acceptance test:** Existing evaluator tests.
- **Plan task:** none

### `vector.ann_indexes` — Approximate nearest-neighbor indexes

- **Current:** partial / local-development / tested
- **Local contract:** CREATE/ALTER/DROP VECTOR INDEX and APPROX_* query shape rules in both dialects; results at least as good as ANN (exact).
- **Official source:** https://cloud.google.com/spanner/docs/find-k-nearest-neighbors (reviewed 2026-09-27)
- **Implementation and tests:** ann_test.cc; exact brute-force execution.
- **Strongest verified tier:** full native suite passed
- **Gap / counterexample:** impl: PostgreSQL CREATE INDEX ... USING ScaNN and spanner.approx_* functions missing (docs/capabilities.md implies they work). cloud: approximate recall/latency tuning.
- **Proposed status:** split: supported (DDL + APPROX query contract with exact results, after A7) + new vector.ann_recall_latency not-applicable — confidence medium
- **Smallest acceptance test:** PG: CREATE INDEX ... USING ScaNN (embedding) WITH (distance_type=...); SELECT ... ORDER BY spanner.approx_cosine_distance(...) LIMIT 5.
- **Plan task:** A7

### `ml.ml_predict` — ML.PREDICT

- **Current:** partial / local-development / tested
- **Local contract:** ML.PREDICT / spanner.ml_predict_row / AI.* validate inputs and return deterministic outputs or provider predictions.
- **Official source:** https://cloud.google.com/spanner/docs/ml (reviewed 2026-09-27)
- **Implementation and tests:** model_evaluator_test, query_engine_test, gateway ml_provider_test (y=2x+1 both dialects).
- **Strongest verified tier:** focused native passed (cited suite regressed in the baseline; see evidence table)
- **Gap / counterexample:** pkg: LocalCloud packaged path unqualified.
- **Proposed status:** supported — confidence high
- **Smallest acceptance test:** Existing tests.
- **Plan task:** none

### `ml.remote_models` — Remote model operations

- **Current:** partial / compatibility-only / tested
- **Local contract:** Remote model/UDF calls use deterministic local results unless a local provider is configured; configured provider errors propagate (no fake success).
- **Official source:** https://cloud.google.com/spanner/docs/ml (reviewed 2026-09-27)
- **Implementation and tests:** Same as ml.ml_predict.
- **Strongest verified tier:** focused native passed (cited suite regressed in the baseline; see evidence table)
- **Gap / counterexample:** cloud: Vertex AI endpoints (ml.external_connections). pkg: LocalCloud path.
- **Proposed status:** supported — confidence high
- **Smallest acceptance test:** Existing tests.
- **Plan task:** none

### `metadata.information_schema` — INFORMATION_SCHEMA

- **Current:** partial / local-development / tested
- **Local contract:** Every documented INFORMATION_SCHEMA view/column in both dialects, with documented role-based row filtering.
- **Official source:** https://cloud.google.com/spanner/docs/information-schema (reviewed 2026-09-27)
- **Implementation and tests:** information_schema_catalog.cc populates ~30 views per dialect; schemas for most missing views already exist in the metadata CSVs.
- **Strongest verified tier:** full native suite passed
- **Gap / counterexample:** impl: ROLES, ROLE_GRANTEES, *_PRIVILEGES, ROLE_*_GRANTS, ROUTINES, PARAMETERS, ROUTINE_OPTIONS, TABLE_SYNONYMS (PG: enabled_roles, applicable_roles, routines, ...) absent; no role filtering; PG locality_group_options lacks locality_group_name (CSV bug). INDEX_OPTIONS, COLUMN_PARAMETERS, IS_STORED_VOLATILE are undocumented (excluded from the contract).
- **Proposed status:** supported (after C3) — confidence high
- **Smallest acceptance test:** Both dialects: role/privilege/routine/synonym views return rows after GRANT/CREATE FUNCTION/SYNONYM; restricted role sees filtered rows, spanner_info_reader sees all.
- **Plan task:** C3 (FGAC agent)

### `metadata.spanner_sys` — SPANNER_SYS

- **Current:** partial / local-development / tested
- **Local contract:** QUERY/READ/TXN/LOCK stats TOP/TOTAL tables, OLDEST_ACTIVE_QUERIES etc. with locally measured rows; spanner_sys_reader visibility.
- **Official source:** https://cloud.google.com/spanner/docs/introspection (reviewed 2026-09-27)
- **Implementation and tests:** Only SUPPORTED_OPTIMIZER_VERSIONS.
- **Strongest verified tier:** full native suite passed
- **Gap / counterexample:** impl: statistics tables absent (queries fail); FLOAT64 mapped to FLOAT32 and missing BYTES/ARRAY types block enabling them.
- **Proposed status:** partial now; supported after D1 — confidence high
- **Smallest acceptance test:** Run query Q twice → QUERY_STATS_TOP_MINUTE has TEXT=Q, EXECUTION_COUNT=2 (both dialects).
- **Plan task:** D1 (stats agent)

### `metadata.pg_catalog` — pg_catalog metadata

- **Current:** partial / local-development / tested
- **Local contract:** Same shared implementation as postgresql.pg_catalog.
- **Official source:** https://cloud.google.com/spanner/docs/information-schema (reviewed 2026-09-27)
- **Implementation and tests:** pg_catalog.cc.
- **Strongest verified tier:** full native suite passed
- **Gap / counterexample:** Same as postgresql.pg_catalog; no duplicate work.
- **Proposed status:** supported (after A8) — confidence high
- **Smallest acceptance test:** Same as postgresql.pg_catalog.
- **Plan task:** A8

### `metadata.database_roles_metadata` — Database role metadata

- **Current:** unsupported / local-development / tested
- **Local contract:** Role/grant DDL persists, ListDatabaseRoles returns roles, privilege views reflect grants, restricted sessions enforce them in both dialects.
- **Official source:** https://cloud.google.com/spanner/docs/information-schema (reviewed 2026-09-27)
- **Implementation and tests:** Only CREATE/DROP ROLE + ListDatabaseRoles.
- **Strongest verified tier:** full native suite passed
- **Gap / counterexample:** impl: grants, privilege views, enforcement missing.
- **Proposed status:** unsupported now; supported after C1-C3 — confidence high
- **Smallest acceptance test:** Same FGAC suite + information_schema role views.
- **Plan task:** C1-C3

### `security.iam_policies` — IAM policy RPCs

- **Current:** partial / compatibility-only / tested
- **Local contract:** Set/GetIamPolicy store and return policies on all Spanner resource kinds with etag compare-and-swap; TestIamPermissions answers for the caller. Enforcement requires an authenticated principal.
- **Official source:** https://cloud.google.com/spanner/docs/iam (reviewed 2026-09-27)
- **Implementation and tests:** policies.cc; etag CAS and persistence tests pass.
- **Strongest verified tier:** full native suite passed
- **Gap / counterexample:** cloud: the emulator receives no authenticated identity (clients connect with emulator/no credentials), so allow/deny enforcement has no local principal. TestIamPermissions returning all requested permissions is correct for an unauthenticated full-access caller.
- **Proposed status:** split: supported (policy management) + new security.iam_enforcement not-applicable — confidence high
- **Smallest acceptance test:** Existing policies_test (set/get/etag conflict/persist) — already passing.
- **Plan task:** G (YAML split)

### `security.database_roles` — Database roles

- **Current:** partial / local-development / tested
- **Local contract:** CREATE/DROP ROLE, GRANT/REVOKE privileges and membership persisted in both dialects; ListDatabaseRoles incl. system roles; sessions with creator_role are authorized by effective privileges.
- **Official source:** https://cloud.google.com/spanner/docs/iam (reviewed 2026-09-27)
- **Implementation and tests:** Role nodes persist (role.cc, schema_updater.cc:1234-1264); ListDatabaseRoles paginates user roles.
- **Strongest verified tier:** full native suite passed
- **Gap / counterexample:** impl: GRANT/REVOKE are no-ops (GoogleSQL) or rejected (PG); creator_role ignored end to end (sessions.cc); no enforcement; ListDatabaseRoles omits public/spanner_info_reader/spanner_sys_reader.
- **Proposed status:** partial now; supported after C1+C2 — confidence high
- **Smallest acceptance test:** Two-dialect FGAC suite: role with SELECT(col) can query col, denied on other col (PERMISSION_DENIED 'Role r does not have required privileges on table T.'); unknown creator_role → 'Role not found: r.'; grants survive --data_dir restart.
- **Plan task:** C1, C2 (FGAC agent)

### `ops.instance_partitions` — Instance partitions

- **Current:** partial / local-development / tested
- **Local contract:** Instance partition CRUD, placements referencing partitions, deletion guards, persistence, operation listing.
- **Official source:** https://cloud.google.com/spanner/docs/instances (reviewed 2026-09-27)
- **Implementation and tests:** instance_partitions_test, manager_test, placements.cc, restart probe.
- **Strongest verified tier:** disposable native restart passed
- **Gap / counterexample:** impl: operation filters ignored. cloud: capacity/regions.
- **Proposed status:** supported (after F1) — confidence high
- **Smallest acceptance test:** Operation filter by name/done.
- **Plan task:** F1

### `ops.quotas` — Cloud quotas and limits

- **Current:** partial / local-development / tested
- **Local contract:** Local schema limits and mutation limits (80,000 cells) with production error codes.
- **Official source:** https://cloud.google.com/spanner/docs/instances (reviewed 2026-09-27)
- **Implementation and tests:** limits.cc, mutation/commit/batch tests.
- **Strongest verified tier:** full native suite passed
- **Gap / counterexample:** cloud: admin/API rate and capacity quotas. Minor: cell count omits index/DML effects (lower bound).
- **Proposed status:** split: supported (local limits) + new ops.rate_quotas not-applicable — confidence high
- **Smallest acceptance test:** Existing limit tests.
- **Plan task:** G

### `clients.rest_gateway` — REST gateway compatibility

- **Current:** partial / local-development / tested
- **Local contract:** REST (gateway_main :9020) maps all RPCs with correct HTTP status, camelCase/snake_case masks, long messages and standard error details.
- **Official source:** https://cloud.google.com/spanner/docs/emulator (reviewed 2026-09-27)
- **Implementation and tests:** gateway_test HTTP cases, gcloud suites; SDK matrix REST 28/28.
- **Strongest verified tier:** native public endpoint (external SDKs/drivers)
- **Gap / counterexample:** impl: error body is {'code':<grpc>,'message'} rather than Google's {'error':{'code':<http>,'message','status','details'}} shape; only hand-written REST callers notice.
- **Proposed status:** supported (after A9) — confidence high
- **Smallest acceptance test:** curl bad request → body.error.status == 'INVALID_ARGUMENT', body.error.code == 400.
- **Plan task:** A9

### `clients.client_libraries` — Google client library compatibility

- **Current:** partial / local-development / tested
- **Local contract:** Google client libraries work with SPANNER_EMULATOR_HOST for admin/query/transaction flows in both dialects.
- **Official source:** https://cloud.google.com/spanner/docs/emulator (reviewed 2026-09-27)
- **Implementation and tests:** 2026-09-27 matrix on binary sha256 e7c9ba45…: Python 3.71.0, Go 1.95.1, Node 9.0.0, Java 6.123.0 all 16/16 incl. multiplexed sessions, stale reads, batch DML, ALREADY_EXISTS propagation, PG dialect.
- **Strongest verified tier:** native public endpoint (external SDKs/drivers)
- **Gap / counterexample:** none for the local contract (C++ covered by conformance).
- **Proposed status:** supported — confidence high
- **Smallest acceptance test:** Re-run the scratch matrix on the final binary.
- **Plan task:** G

### `clients.jdbc` — JDBC compatibility

- **Current:** unknown / compatibility-only / unverified
- **Local contract:** Spanner JDBC driver and PGAdapter work against the emulator endpoint.
- **Official source:** https://cloud.google.com/spanner/docs/emulator (reviewed 2026-09-27)
- **Implementation and tests:** 2026-09-27 matrix: google-cloud-spanner-jdbc 2.45.0 18/18 (DDL, prepared queries, commit/rollback, 9 types, getTables/getColumns, SQLException code 6, PG dialect); PGAdapter 0.55.3 + postgresql 42.7.13 13/13 (SQLSTATE 23505) and psql 17.5.
- **Strongest verified tier:** native public endpoint (external SDKs/drivers)
- **Gap / counterexample:** none for the local contract.
- **Proposed status:** supported (was unknown) — confidence high
- **Smallest acceptance test:** Re-run jdbc/ and pgadapter/ programs on the final binary.
- **Plan task:** G

### `backups.schedules` — Backup schedule RPCs

- **Current:** partial / local-development / tested
- **Local contract:** Backup schedule CRUD (update_mask), full and incremental specs, UTC cron, due backups created once with retention, cursor persists across restart.
- **Official source:** https://cloud.google.com/spanner/docs/reference/rpc/google.spanner.admin.database.v1 (reviewed 2026-09-27)
- **Implementation and tests:** Full schedules run and persist (backups_test, restart probe).
- **Strongest verified tier:** disposable native restart passed
- **Gap / counterexample:** impl: incremental_backup_spec → UNIMPLEMENTED; encryption_config rejected.
- **Proposed status:** partial now; supported after E4 — confidence high
- **Smallest acceptance test:** Incremental schedule creates backups sharing incremental_backup_chain_id with oldest_version_time; each restores the full database.
- **Plan task:** E4

### `database.internal_graph_operation` — InternalUpdateGraphOperation RPC

- **Current:** partial / compatibility-only / tested
- **Local contract:** Internal RPC: marks an existing local operation failed (nonzero status) or completed (OK status).
- **Official source:** https://cloud.google.com/spanner/docs/reference/rpc/google.spanner.admin.database.v1 (reviewed 2026-09-27)
- **Implementation and tests:** database_extensions.cc/test.
- **Strongest verified tier:** full native suite passed
- **Gap / counterexample:** impl: success status path not modeled. Not a developer-facing API.
- **Proposed status:** supported (after A11) — confidence medium
- **Smallest acceptance test:** OK status → operation done without error.
- **Plan task:** A11

### `instance.move` — MoveInstance RPC

- **Current:** partial / local-development / tested
- **Local contract:** MoveInstance validates target_config, rejects instances with backups, updates config metadata, typed completed operation, persisted.
- **Official source:** https://cloud.google.com/spanner/docs/reference/rpc/google.spanner.admin.instance.v1 (reviewed 2026-09-27)
- **Implementation and tests:** instance_extensions_test.
- **Strongest verified tier:** full native suite passed
- **Gap / counterexample:** cloud: physical data movement. proto: pinned proto lacks target_database_move_configs.
- **Proposed status:** supported — confidence high
- **Smallest acceptance test:** Existing handler tests.
- **Plan task:** none

### `transactions.isolation_levels` — Transaction isolation levels

- **Current:** partial / local-development / tested
- **Local contract:** REPEATABLE_READ: snapshot chosen at first read, lock-free reads, write-write conflicts abort at commit, write skew allowed; SERIALIZABLE unchanged; PDML rejects RR.
- **Official source:** https://cloud.google.com/spanner/docs/isolation-levels (reviewed 2026-09-27)
- **Implementation and tests:** Only option acceptance/rejection (session.cc:191-195); CreateReadWrite drops isolation_level.
- **Strongest verified tier:** full native suite passed
- **Gap / counterexample:** impl: REPEATABLE_READ runs serializable semantics (reads take locks at latest data, no snapshot, no commit-time validation).
- **Proposed status:** partial now; supported after B1 — confidence high
- **Smallest acceptance test:** RR txn A reads x at snapshot; txn B commits x+1; A still reads old x; A's write to x aborts at commit; write-skew pair both commit under RR, one aborts under SERIALIZABLE.
- **Plan task:** B1

### `transactions.concurrency_model` — Concurrency, aborts, and fault injection

- **Current:** partial / local-development / tested
- **Local contract:** Disjoint read-write transactions proceed concurrently; overlapping ones abort one side (wound-wait) and retries succeed; fault injection aborts ~5% of first commits.
- **Official source:** https://cloud.google.com/spanner/docs/transactions (reviewed 2026-09-27)
- **Implementation and tests:** Key/range lock manager, manager_test, read_write_transaction_test, 5,000-row gate.
- **Strongest verified tier:** full native suite passed
- **Gap / counterexample:** test: two-key deadlock cycle and separate-thread writers not covered; gateway forwards --enable_fault_injection (abort probability flag is emulator_main only, documented).
- **Proposed status:** supported (B3 adds tests) — confidence high
- **Smallest acceptance test:** Two threads lock keys in opposite order; exactly one aborts, retry commits both.
- **Plan task:** B3

### `schema.database_options` — ALTER DATABASE options

- **Current:** partial / local-development / tested
- **Local contract:** ALTER DATABASE SET OPTIONS version_retention_period (enforced), default_time_zone (applied), default_sequence_kind, default_leader/witness_location/read_lease_regions/columnar_policy as metadata; GetDatabase projections; survives restart.
- **Official source:** https://cloud.google.com/spanner/docs/data-definition-language (reviewed 2026-09-27)
- **Implementation and tests:** database_option.cc updater tests, default_time_zone.cc, databases_test.
- **Strongest verified tier:** full native suite passed
- **Gap / counterexample:** test: no live process restart probe (metadata replay only). cloud: leader placement.
- **Proposed status:** supported (after G restart probe) — confidence high
- **Smallest acceptance test:** Disposable --data_dir: set options, restart emulator_main, GetDatabase + INFORMATION_SCHEMA.DATABASE_OPTIONS unchanged.
- **Plan task:** G

## Other records (106)

| ID | Current | Proposed | Tier | Finding |
| --- | --- | --- | --- | --- |
| `data.sessions.create` | supported | supported | native public endpoint (external SDKs/drivers) | Works; SDK matrix creates sessions in all clients. creator_role gains validation with FGAC (C2). |
| `data.sessions.get` | supported | supported | full native suite passed | Accurate. |
| `data.sessions.list` | supported | supported | full native suite passed | Label filter ignored (F1 adds labels.* filter). |
| `data.sessions.delete` | supported | supported | full native suite passed | Accurate. |
| `data.sessions.batch_create` | supported | supported | full native suite passed | Accurate; session_template.creator_role handled in C2. |
| `data.sql.execute` | supported | supported | native public endpoint (external SDKs/drivers) | Accurate; PLAN/PROFILE caveat closes with D2. |
| `data.sql.execute_streaming` | supported | supported | native public endpoint (external SDKs/drivers) | Resume tokens unsupported (clients restart the stream). PROFILE stats placed on first message and no plan — fixed in D2. |
| `data.read.read` | supported | supported | native public endpoint (external SDKs/drivers) | Accurate. |
| `data.read.streaming_read` | supported | supported | native public endpoint (external SDKs/drivers) | Accurate. |
| `data.sql.partition_query` | supported | supported | full native suite passed | Two-partition shape is a valid local answer; partition_options ignored (physical). |
| `data.read.partition_read` | supported | supported | full native suite passed | Accurate. |
| `data.batch.create_transaction` | supported | supported | native public endpoint (external SDKs/drivers) | Inline begin exercised by every SDK in the matrix. |
| `instance.create` | supported | supported | native public endpoint (external SDKs/drivers) | Accurate. |
| `instance.get` | supported | supported | full native suite passed | Accurate. |
| `instance.list` | supported | supported | full native suite passed | Filter ignored (F1 adds name/display_name/labels filter). |
| `instance.update` | supported | supported | disposable native restart passed | Recent promotion held: masks validated, typed metadata, restart probe passed. |
| `instance.delete` | supported | supported | disposable native restart passed | Accurate. |
| `database.create` | supported | supported | native public endpoint (external SDKs/drivers) | encryption_config ignored (CMEK is security.encryption). |
| `database.get` | supported | supported | full native suite passed | Accurate. |
| `database.list` | supported | supported | full native suite passed | Accurate (ListDatabases has no filter field). |
| `database.drop` | supported | supported | full native suite passed | Accurate. |
| `database.update_ddl` | supported | supported | native public endpoint (external SDKs/drivers) | Accurate. |
| `operations.get` | supported | supported | full native suite passed | Accurate. |
| `operations.list` | supported | supported | full native suite passed | Filters ignored for database/backup/partition operation lists — F1 adds AIP-160 filters. |
| `operations.cancel` | accepted-no-op | accepted-no-op | full native suite passed | Correct: operations are always done, cancelling a done operation has no effect. |
| `schema.tables` | supported | supported | native public endpoint (external SDKs/drivers) | Accurate. |
| `schema.alter_table` | supported | supported | full native suite passed | Accurate. |
| `schema.drop_table` | supported | supported | full native suite passed | Accurate. |
| `schema.views` | supported | supported | full native suite passed | DEFINER has no privilege effect until FGAC (C2); note updates then. |
| `schema.named_schemas` | supported | supported | full native suite passed | Accurate; USAGE privilege arrives with C1. |
| `schema.sequences` | supported | supported | full native suite passed | Accurate. |
| `schema.placements` | supported | supported | full native suite passed | Accurate; physical placement covered by ops.replication. |
| `types.bool_int_float` | supported | supported | native public endpoint (external SDKs/drivers) | Accurate. |
| `types.numeric` | supported | supported | native public endpoint (external SDKs/drivers) | Accurate. |
| `types.arrays` | supported | supported | native public endpoint (external SDKs/drivers) | Accurate. |
| `types.structs` | supported | supported | full native suite passed | Accurate. |
| `types.proto` | supported | supported | full native suite passed | Accurate. |
| `types.interval` | supported | supported | full native suite passed | Interval arithmetic lacks end-to-end tests (A2 adds them). |
| `googlesql.select` | supported | supported | native public endpoint (external SDKs/drivers) | Accurate. |
| `googlesql.joins` | supported | supported | full native suite passed | Accurate. |
| `googlesql.subqueries` | supported | supported | full native suite passed | Accurate. |
| `googlesql.parameters` | supported | supported | native public endpoint (external SDKs/drivers) | Accurate. |
| `googlesql.hints` | accepted-no-op | accepted-no-op | full native suite passed | Correct classification: hints validated, performance hints have no execution effect (lock_scanned_ranges gains an effect in B2). |
| `googlesql.tablesample` | supported | supported | full native suite passed | Accurate. |
| `postgresql.create_database` | supported | supported | native public endpoint (external SDKs/drivers) | PGAdapter matrix confirms wire-protocol access via PGAdapter. |
| `postgresql.synonyms` | supported | supported | full native suite passed | TABLE_SYNONYMS arrives with C3. |
| `writes.mutations_insert` | supported | supported | native public endpoint (external SDKs/drivers) | Note says per-commit limits not enforced; the 80,000-cell limit is now enforced (docs contradiction to fix). |
| `writes.mutations_update` | supported | supported | native public endpoint (external SDKs/drivers) | Accurate. |
| `writes.mutations_delete` | supported | supported | full native suite passed | Accurate. |
| `writes.dml` | supported | supported | native public endpoint (external SDKs/drivers) | Accurate. |
| `writes.batch_dml` | supported | supported | native public endpoint (external SDKs/drivers) | Accurate. |
| `writes.batch_write` | supported | supported | full native suite passed | Accurate. |
| `transactions.read_write` | supported | supported | native public endpoint (external SDKs/drivers) | Accurate. |
| `transactions.read_only` | supported | supported | native public endpoint (external SDKs/drivers) | Accurate. |
| `transactions.commit_timestamps` | supported | supported | native public endpoint (external SDKs/drivers) | Accurate. |
| `transactions.read_your_writes` | supported | supported | full native suite passed | Accurate. |
| `indexes.secondary_indexes` | supported | supported | native public endpoint (external SDKs/drivers) | Accurate. |
| `indexes.unique_indexes` | supported | supported | full native suite passed | Accurate. |
| `indexes.foreign_keys` | supported | supported | full native suite passed | Accurate. |
| `indexes.check_constraints` | supported | supported | full native suite passed | Accurate. |
| `indexes.generated_columns` | supported | supported | full native suite passed | Accurate. |
| `indexes.identity_columns` | supported | supported | full native suite passed | Accurate. |
| `indexes.column_defaults` | supported | supported | full native suite passed | Accurate. |
| `indexes.on_update` | supported | supported | full native suite passed | Accurate. |
| `change_streams.create` | supported | supported | full native suite passed | Accurate. |
| `change_streams.exclusions` | supported | supported | full native suite passed | exclude_ttl_deletes gains an effect with TTL (E5). |
| `vector.vector_type` | supported | supported | full native suite passed | Accurate. |
| `vector.distance_functions` | supported | supported | full native suite passed | Recent promotion held (both dialects, both widths). |
| `vector.vector_query_options` | accepted-no-op | accepted-no-op | full native suite passed | Correct: exact search makes num_leaves_to_search irrelevant. |
| `graph.ddl` | supported | supported | full native suite passed | Accurate. |
| `graph.gql_queries` | supported | supported | full native suite passed | Accurate. |
| `graph.graph_schema` | supported | supported | full native suite passed | Accurate. |
| `graph.algorithms` | accepted-no-op | accepted-no-op | full native suite passed | Note inaccurate: stand-ins take no arguments and use different output names, so documented calls fail rather than 'parse and validate'. Notes corrected; computation out of scope. |
| `ml.models` | supported | supported | full native suite passed | Accurate. |
| `ml.external_connections` | not-applicable | not-applicable | source inspected | Accurate. |
| `metadata.operation_metadata` | supported | supported | full native suite passed | Accurate. |
| `security.fine_grained_access` | accepted-no-op | accepted-no-op → supported (with C1-C3) | full native suite passed | GRANT/REVOKE are no-ops today; FGAC implementation converts this record to supported. |
| `security.encryption` | not-applicable | not-applicable | source inspected | Accurate for CMEK; Google-default encryption types become accepted for backups (E3). |
| `security.audit_logs` | not-applicable | not-applicable | source inspected | Accurate. |
| `ops.replication` | not-applicable | not-applicable | source inspected | Accurate. |
| `ops.autoscaling` | not-applicable | not-applicable | source inspected | Accurate. |
| `ops.monitoring` | not-applicable | not-applicable | source inspected | Note says SPANNER_SYS stats not provided — changes with D1; Cloud Monitoring export stays not-applicable. |
| `ops.leader_options` | accepted-no-op | accepted-no-op | full native suite passed | Correct: default_leader stored and projected; routing is physical. |
| `clients.grpc_endpoint` | supported | supported | native public endpoint (external SDKs/drivers) | Deadlines ignored (B4 bounds future-read waits). |
| `clients.cli` | supported | supported | full native suite passed | gcloud suites in //tests/gcloud (not in the native sweep). |
| `clients.docker` | supported | supported | source inspected | No automated image test in this repo (packaging tier). |
| `emulator.in_memory` | supported | supported | full native suite passed | Accurate. |
| `emulator.persistence` | supported | supported | disposable native restart passed | Restart probes in earlier slices; grants/TTL persistence added by C1/E5. |
| `emulator.startup` | supported | supported | native public endpoint (external SDKs/drivers) | gateway_main launched emulator_main for the SDK matrix. |
| `emulator.packaging` | supported | supported | source inspected | Publishing workflow only; no image test. |
| `data.cache_updates` | accepted-no-op | accepted-no-op | full native suite passed | Correct. |
| `database.update` | supported | supported | disposable native restart passed | Recent promotion held. |
| `backups.metadata` | supported | supported | disposable native restart passed | Recent promotion held. ListBackups filter precedence (AND tighter than OR) contradicts AIP-160 — fixed in F1. |
| `database.split_points` | accepted-no-op | accepted-no-op | full native suite passed | Correct: physical split placement. |
| `operations.lifecycle` | supported | supported | full native suite passed | Accurate. |
| `transactions.lifecycle` | supported | supported | native public endpoint (external SDKs/drivers) | return_commit_stats ignored — A10 implements CommitStats.mutation_count. |
| `data.sessions.multiplexed` | supported | supported | native public endpoint (external SDKs/drivers) | Python/Go/Node/Java default to multiplexed sessions in the matrix. |
| `data.directed_reads_data_boost` | accepted-no-op | accepted-no-op | full native suite passed | Correct: no replicas. |
| `emulator.restore_fault_isolation` | supported | supported | full native suite passed | Accurate. |
| `types.string_bytes_date_timestamp` | supported | supported | native public endpoint (external SDKs/drivers) | Accurate. |
| `types.uuid` | supported | supported | disposable native restart passed | Recent promotion held: both dialects, change streams, restart. |
| `writes.upsert_dml` | supported | supported | full native suite passed | Accurate. |
| `writes.dml_returning` | supported | supported | full native suite passed | Accurate. |
| `schema.interleaving` | supported | supported | full native suite passed | Accurate. |
| `schema.row_deletion_policy` | accepted-no-op | accepted-no-op → supported (with E5) | full native suite passed | Rows never expire today; TTL sweeper converts this record to supported. |
| `schema.locality_groups` | accepted-no-op | accepted-no-op | full native suite passed | Correct: tiered storage is physical. |

## Status summary

Baseline: {'supported': 89, 'partial': 44, 'accepted-no-op': 11, 'not-applicable': 6, 'unsupported': 1, 'unknown': 1}. Under the usability definition, evaluation before any code change: 14 partial rows are already usable and only need evidence/notes (writes.partitioned_dml, transactions.stale_reads, transactions.transaction_errors, indexes.index_backfill, change_streams.partition_churn, backups.restore, instance.move, schema.udfs, ml.ml_predict, ml.remote_models, clients.client_libraries, clients.jdbc (unknown), security.iam_policies and ops.quotas with physical splits); the rest need implementation or regression fixes listed per row.

## Contradictions found

- `docs/feature-coverage.md` lists TO_BASE32/FROM_BASE32 as working; they failed at execution (fixed).
- `docs/known-gaps.md` and `docs/change-streams.md` blame DML INSERT for extra NULL columns; the cause is DML UPDATE writing the full row.
- `graph.algorithms` note says calls "parse and validate"; documented calls with arguments fail.
- `docs/capabilities.md` implies PostgreSQL APPROX_* vector functions work; they were not mapped.
- `writes.mutations_insert` says per-commit limits are not enforced; the 80,000-cell limit is enforced.
- `ops.monitoring` says SPANNER_SYS statistics are absent; this changes with the statistics work.
- The plan's "182 *Search* cases passed" claim no longer holds for the dirty tree (6 regressions).
- The ML/remote-UDF "evaluator targets passed" claim no longer holds (stale requestId expectations).
- ListBackups filters bind AND tighter than OR, contrary to Google AIP-160 filter precedence.
- `lock_scanned_ranges=exclusive` never took exclusive locks although documented as a locking hint.

## Sister-project reuse

- `../bigquery-emulator-on-duckdb` exact VECTOR_SEARCH: exact-scan oracle only; does not supply an ANN index (Spanner APPROX_* stays exact locally, which satisfies the query contract; recall/latency split as not-applicable).
- Its geography/S2 and HLL/ZetaSketch libraries: no matching open Spanner feature.
- Its ML provider binding: same explicit-provider/no-fake-success principle already used by `--remote_functions_host_port`; its OpenAI-style transport does not speak Spanner's Remote UDF JSON protocol, so no code reuse.
- LocalCloud IAM/KMS/inference: not on the native Spanner gRPC/REST paths; packaged qualification is a LocalCloud-side tier and is not claimed here.

## Post-implementation results (2026-09-28)

The closure plan was implemented in six batches (core SQL/transactions/REST, SPANNER_SYS and plans, PostgreSQL/search/vector, a conformance byte fix, FGAC and information schema, and streams/backups/TTL/filters/locking), merged, repaired for merge fallout (two concatenated handler tests, a lock-manager test updated for the new release-on-abort rule), and applied to the `jay-spanner-extended` working tree without a commit. `gateway_main` also gained `--spanner_sys_expose_open_interval` forwarding.

| Evidence | Baseline (2026-09-27) | Final (2026-09-28) |
| --- | --- | --- |
| Full native suite, fixed-environment Bazel (`//tests/conformance/... //backend/... //frontend/... //gateway/... //common/...`) | 138 targets: 135 pass, 3 fail | 142 targets (4 new): 142 pass. The 3 baseline failures (ML `requestId` expectations, 6 GoogleSQL search cases) pass. |
| Also run: `//third_party/spanner_pg/ddl:ddl_test`, `//third_party/spanner_pg/catalog:all` | `DatabaseRoleDdl` expectation stale | 17 targets: 17 pass (159/159 overall) |
| Binaries | `emulator_main` `e7c9ba45…`, `gateway_main` `47652e0f…` | `emulator_main` `79cb9f53…`, `gateway_main` `cffb13af…` |
| Client matrix, native public endpoint (`tests/client_matrix`) | All pass; REST errors in grpc-gateway shape | Python 3.71.0 16/16 (and 16/16 without multiplexed sessions), Go 1.95.1 16/16, Node 9.0.0 16/16, Java 6.123.0 16/16, JDBC 2.45.0 18/18, PGAdapter 0.55.3 + pgJDBC 42.7.13 13/13 and the psql 17.5 script, REST 28/28 including Google `{"error": {code, message, status}}` envelope checks |
| Official doc-example conformance (1,165 examples) | 1,015 pass; 38 genuine defects in 11 functions | 1,051 pass, 32 mismatch, 2 error, 80 skipped; 0 defects. Non-matches are 19 order-unspecified results, 13 documentation errors, and 2 `DEBUG_TOKENLIST` format differences (Spanner documents the format as unstable). |
| Disposable `--data_dir` restart probe (both dialects) | Not run for grants/TTL/options | Setup 4/4, verify after restart 33/33: GRANTs, DEFINER views, database options, TTL policies and sequences in `GetDatabaseDdl`; `ListDatabaseRoles` with system roles; `creator_role` sessions still restricted and unknown roles rejected; `INFORMATION_SCHEMA.DATABASE_OPTIONS`/`ROLES`/`TABLE_PRIVILEGES`/`COLUMN_PRIVILEGES`; `GetDatabase` fields; TTL sweeper deleted expired rows; `version_time` backup with Google default encryption survived restart and restored the historical image without TTL policies. |
| Coverage tooling | 22/22 | `validate`, `generate`, `audit-rpcs`, `check` pass; `tools.feature_coverage_test` 22/22 |
| Packaged Docker/LocalCloud image | Not qualified | Not qualified (no packaged tier claimed) |

Status counts: baseline 89 supported, 44 partial, 11 accepted-no-op, 6 not-applicable, 1 unsupported, 1 unknown (152 records). Final: 137 supported, 9 accepted-no-op, 9 not-applicable (155 records). All 44 partial records, `metadata.database_roles_metadata` (unsupported), `clients.jdbc` (unknown), and `security.fine_grained_access` and `schema.row_deletion_policy` (accepted-no-op) became supported. New not-applicable records split out physical properties: `security.iam_enforcement`, `ops.rate_quotas`, and `vector.ann_recall_latency`.

Still not supported, by design:

- accepted-no-op: `operations.cancel` (operations finish synchronously), `googlesql.hints` (performance hints; `LOCK_SCANNED_RANGES` now has an effect), `vector.vector_query_options` (exact search), `graph.algorithms` (zero-argument stand-ins; notes corrected; supported since the remaining-limitations batches below), `ops.leader_options`, `data.cache_updates`, `database.split_points`, `data.directed_reads_data_boost`, `schema.locality_groups` (physical placement, caching, replicas, and storage tiers).
- not-applicable: `ml.external_connections`, `security.encryption` (customer-managed keys), `security.audit_logs`, `security.iam_enforcement`, `ops.replication`, `ops.autoscaling`, `ops.monitoring` (Cloud Monitoring export; SPANNER_SYS is now local), `ops.rate_quotas`, `vector.ann_recall_latency`.

Residual differences recorded in `docs/known-gaps.md` rather than as partial rows (as of the closure; the batches below removed several): SPANNER_SYS statistics are in memory with zero physical columns and lock waits; plans come from the local engine; PostgreSQL `GetDatabaseDdl` prints search and vector indexes as plain `CREATE INDEX`; FGAC does not check IAM role use, INVOKER view bodies in `PLAN` mode, sequences in defaults, or `pg_catalog` rows; TTL runs every sweep interval instead of within about 72 hours; `MOVE` churns keep one child and writes use one partition; incremental backups are physically full; `DEBUG_TOKENLIST` output format.

The contradictions listed above were corrected in `docs/feature-coverage.yaml`, `docs/capabilities.md`, `docs/known-gaps.md`, `docs/change-streams.md`, and `docs/persistence.md`.

### Remaining-limitations round (2026-09-28)

After the closure, three batches addressed most of the residual differences above. They were merged and tested together on commit `bd6a9646`; the `jay-spanner-extended` working tree carries the same code without a commit.

- **Batch 1:** persistent-storage range scans follow the on-disk key order (length, then content), which fixes range reads and range deletes that skipped rows with `--data_dir` and closes the unique-index incident (its stress test failed about half its runs before and passed 30/30 after; no format change). PostgreSQL `GetDatabaseDdl` prints `CREATE SEARCH INDEX` and `CREATE INDEX ... USING scann (...) WITH (...)`. New `SPANNER_SYS` tables `ROW_DELETION_POLICIES`, `USER_SPLIT_POINTS` (`AddSplitPoints` now validates and persists split points), `TABLE_SIZES_STATS_1HOUR`, and `ACTIVE_PARTITIONED_DMLS`. FGAC checks INVOKER view bodies at analysis (so `PLAN` is checked), requires sequence privileges for defaults and generated columns, and filters `pg_catalog` by role. `max_commit_delay` is validated (0–500 ms); `google.longrunning` `ListOperations` applies the AIP-160 filter; optimizer hints are accepted on DML; `SCORE`/`SCORE_NGRAMS` options; change stream retention cleanup; the `--data_dir` lock.
- **Batch 2a:** resume tokens for `ExecuteStreamingSql`, `StreamingRead`, and change stream queries; wound-wait lock waits with `--lock_wait_timeout_ms` (forwarded by `gateway_main`) and measured `LOCK_STATS` waits; `SPANNER_SYS` statistics persisted per database; `MUTABLE_KEY_RANGE` streams no longer re-send partition start records.
- **Batch 2b:** the 13 graph algorithms computed in memory with `EXPORT DATA` write-back; search tokenization (Unicode RQUERY terms, HTML entities and skipped `script`/`style`, `short_tokens_only_for_anchors`, mixed `remove_diacritics`, French, CJK script boundaries, `DEBUG_TOKENLIST`), and the PostgreSQL `spanner.tokenize_substring` crash fix.

| Evidence | Closure (2026-09-28) | Remaining-limitations round |
| --- | --- | --- |
| Combined native suite, fixed-environment Bazel | 159/159 targets | 164/164 targets on `bd6a9646` |
| Client matrix, native public endpoint | All clients passed | Not re-run |
| Official doc-example conformance | 1,051 pass, 0 defects | Not re-run; `DEBUG_TOKENLIST`'s two documented examples are covered by unit tests |
| Disposable `--data_dir` restart probe | 33/33 | Not re-run; split points, persisted `SPANNER_SYS` statistics, and the directory lock have native unit tests only |
| Coverage tooling | 22/22 | `validate`, `generate`, `audit-rpcs`, `check` pass; `tools.feature_coverage_test` 22/22 |
| Packaged Docker/LocalCloud image | Not qualified | Not qualified |

Status counts: before this round 137 supported, 9 accepted-no-op, 9 not-applicable; after it 138 supported, 8 accepted-no-op, 9 not-applicable (155 records). `graph.algorithms` became supported. `database.split_points` stays accepted-no-op: split points are validated, stored, persisted, and shown in `USER_SPLIT_POINTS`, but the documented effect (pre-splitting data ahead of load) is physical, as with `schema.locality_groups` and `ops.leader_options`. `googlesql.hints` stays accepted-no-op because the newly accepted DML optimizer hints don't change execution. Notes and evidence changed for the streaming, change stream, search, vector, graph, `SPANNER_SYS`, FGAC, transaction, TTL, operations, and persistence records; the `emulator.persistence` note no longer credits the restart probe with the split point, statistics, and lock checks.

Removed from or corrected in `docs/known-gaps.md`: the open unique-index incident, PostgreSQL `GetDatabaseDdl` printing plain `CREATE INDEX`, the missing `SPANNER_SYS` tables above, the three FGAC gaps (INVOKER bodies in `PLAN`, sequences in defaults, `pg_catalog` rows), `OPTIMIZER_VERSION` failing on DML, `max_commit_delay` being ignored, unfiltered `ListOperations`, missing resume tokens (including change stream placeholders), unmodeled lock waits and zero lock wait times, in-memory-only `SPANNER_SYS` statistics, unexpired change stream records, the unlocked data directory, graph algorithm stand-ins, and the search gaps for Unicode RQUERY, HTML, anchors, mixed diacritics, French, `DEBUG_TOKENLIST`, and `SCORE` options.

Remaining or added: no 10-second idle-transaction abort, lock waits ignore RPC deadlines, `READ_STATS` `AVG_LOCKING_DELAY_SECONDS` is 0; resume tokens only at row boundaries; `TABLE_SIZES_STATS_1HOUR` is a logical estimate and `ACTIVE_PARTITIONED_DMLS` shows progress 0; split points have no storage effect; graph algorithm results are emulator-defined where Spanner's method isn't published, `machine_category`/`zone`/`max_idle_time` are validated only, permission, schema and `RETURN`-clause rules aren't enforced, `GRAPH_OPERATION_EXECUTION_STATUS` is empty, and a write-back inside a read-write transaction can conflict with its own locks; no CJK dictionaries and no full HTML5 entity table; `bigram_weight` and `idf_weight` return `UNIMPLEMENTED`. Two pre-existing bugs are listed as open: graph `RETURN ... ORDER BY` is ignored (from code/tests), and a change stream scan can skip a commit at an exact scan-boundary microsecond (from code).

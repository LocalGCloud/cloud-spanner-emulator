# Spanner partial-feature support implementation plan

> For agentic workers: execute one numbered task as a reviewed change with a failing behavior test, a focused passing test, and an evidence update before starting the next task.

**Goal:** Make every locally meaningful behavior in the 48 current `partial` feature records work and qualify it as `supported` for LocalCloud development. Preserve explicit `not-applicable` records for physical production behavior a single-node emulator cannot provide.

**Architecture:** The C++ Spanner emulator owns Spanner SQL, storage, transactions, and API semantics. LocalCloud may supply identity, KMS, and a real local inference provider through explicit process boundaries; the direct Spanner gRPC and REST listeners must be covered. BigQuery's emulator supplies test and provider-binding patterns, not a drop-in Spanner execution engine.

**Tech stack:** C++/Bazel/GoogleTest/GoogleSQL, LevelDB, Go REST gateway, LocalCloud Java/Supervisord/Docker, Python coverage tooling.

**Spec:** `docs/feature-coverage.yaml` and `docs/plans/2026-08-25-spanner-feature-parity-coverage-design.md`.

**Baseline:** `6f928fed` on `jay-spanner-extended`: 152 records, including 48 partial. This is a plan, not a claim that any planned behavior has passed.

## Global constraints

- `supported` means locally useful behavior with representative public-API evidence, as defined by the coverage design. Parser acceptance, a stored option, or another service's existence does not prove support.
- Qualify standalone emulator behavior and the packaged LocalCloud path separately. A LocalCloud-only feature must state its required configuration; if the standalone and packaged behaviors materially differ, split the inventory record or retain `partial`.
- Do not promise TrueTime, real regional replicas, distributed throughput, production optimizer costs, exact production search scores, production ANN latency, or production telemetry from a single-node process. Split those physical properties into `not-applicable` records when the local observable contract is implemented.
- Treat database SQL roles and grants separately from Google Cloud IAM. Cloud SQL PostgreSQL is not Spanner's PostgreSQL dialect.
- Never return a fabricated successful ML prediction when a real provider was requested. In strict IAM mode, missing identity or an unavailable authorization bridge denies access.
- Every stateful slice needs a `--data_dir` restart test. Every new LocalCloud integration needs a direct SDK test on an immutable candidate image; an open port, list response, or `/readiness` alone is insufficient.
- Update `docs/feature-coverage.yaml`, regenerate `docs/feature-coverage.md`, and reconcile `docs/capabilities.md`, `docs/known-gaps.md`, `docs/configuration.md`, and `docs/CHANGELOG.md` in the same feature change. Keep a precise evidence tier: source, local unit, native conformance, packaged LocalCloud, or live product comparison.

## Component ownership and order

| Package | Primary files and tests | Depends on |
| --- | --- | --- |
| 0. Contract and baseline | `docs/feature-coverage.yaml`, `tools/feature_coverage.py`, `tests/conformance/cases/` | none |
| 1. SQL, types, and catalogs | `backend/query/feature_filter/gsql_supported_functions.cc`, `backend/query/query_engine.cc`, `backend/query/information_schema_catalog.cc`, `backend/query/pg_catalog.cc`, `third_party/spanner_pg/ddl/pg_to_spanner_ddl_translator.cc`; conformance query/PG/catalog tests | 0; role-specific work depends on 2 |
| 2. Roles and IAM | `backend/schema/updater/schema_updater.cc`, `frontend/handlers/policies.cc`, `frontend/handlers/database_extensions.cc`, LocalCloud `IamPolicyEvaluator.java`; role, policy, and direct SDK tests | 0 |
| 3. Transactions, PDML, indexes | `backend/locking/manager.cc`, `backend/transaction/`, `backend/query/partitioned_dml_validator.h`, `backend/schema/backfills/`; transaction, PDML, backfill tests | 0 |
| 4. Streams and backups | `backend/actions/change_stream.cc`, `backend/database/change_stream/change_stream_partition_churner.cc`, `frontend/handlers/backups.cc`; stream, backup, restart tests | 3 for transaction ordering; 7 for packaged KMS wiring |
| 5. Search and vector | `backend/query/search/`, `backend/query/ann_validator.cc`, `backend/query/ann_functions_rewriter.cc`; search/ANN conformance tests | 0, 3 for index maintenance |
| 6. Remote functions and ML | `backend/query/ml/model_evaluator.cc`, `backend/query/remote_udf/remote_udf_evaluator.cc`, `gateway/gateway.go`, LocalCloud `localcloud.defaults.yaml`, wrapper, Supervisor, and entrypoint | 0, packaged wiring from 7 |
| 7. Admin and clients | `frontend/handlers/instances.cc`, `frontend/handlers/instance_extensions.cc`, `frontend/handlers/databases.cc`, `frontend/handlers/instance_partitions.cc`, `gateway/gateway.go`, LocalCloud service config, image/wrapper, Supervisor | 0 |
| 8. Plans and statistics | `frontend/handlers/queries.cc`, `backend/query/spanner_sys_catalog.cc`; query-mode and SPANNER_SYS tests | 3 for lock/transaction metrics; 2 for role-filtered access |
| 9. Qualification | Coverage catalog, native conformance, LocalCloud platform tests, exact candidate image | 1–8 |

Paths under `LocalCloud` refer to the sibling `/Users/jsenjaliya/src/AI/localcloud` checkout. The Spanner image currently enters through `docker/conf/supervisord.conf` and a `Dockerfile` wrapper that forwards `--data_dir`; new settings require `localcloud.defaults.yaml` validation, entrypoint/Supervisor propagation, a packaged command test, and an image rebuild. Writing `spanner.env` alone is insufficient because Supervisor does not consume it.

**Execution waves:** (1) task 0, then evidence-first slices of tasks 1, 5 (exact distance), and 7 (REST/client tests); (2) tasks 2 and 3 in parallel with independent file ownership; (3) task 4 after transaction ordering, while search/vector and remote ML continue independently; (4) task 8 after transaction instrumentation; (5) task 9 after every feature gate. The first reviewable change is a PostgreSQL UUID public-API roundtrip test and any fix it exposes, followed by its catalog update.

### Task 0: Freeze the local support contract

- [ ] Record the official Spanner baseline for both dialects: SQL function signatures, DDL variants, admin field masks, role privileges, change-stream capture values and restore rules, SCORE/vector options, SPANNER_SYS table families, and RPC error behavior. Use the official references below, not generic GoogleSQL or upstream PostgreSQL behavior.
- [ ] For each of the 48 rows in the exit matrix, decide whether the listed exit check covers the entire locally observable contract. Where a row mixes functional behavior with physical infrastructure, split it into a supported local behavior and a named cloud-only/not-applicable behavior before reclassifying it.
- [ ] Capture current failures with focused conformance or component tests. A test that already passes is evidence, not a reason to write a duplicate test. Do not run two Bazel builds against one shared output tree concurrently.
- [ ] Record the baseline LocalCloud image ID and pinned Spanner image provenance, then verify both direct endpoints (gRPC 5370, REST 5371). Test semantic readiness with a constant query and a resource operation after the image starts.

**Exit:** all 48 IDs are assigned to packages 1–8; each has a documented test and scope decision. No status is raised by this task.

### Task 1: SQL, types, PostgreSQL, and catalogs

- [ ] Add GoogleSQL and Spanner PostgreSQL type tests for UUID table keys, non-key values, mutations, reads, SQL results, change streams, casts, and `NEW_UUID`; run the affected converter tests and the conformance target. Keep `GENERATE_UUID` as STRING, matching Spanner.
- [ ] Build a versioned table of Spanner-documented JSON/JSONB, aggregate, string, date/time, and PostgreSQL function signatures. For each missing locally relevant signature, add a failing positive or boundary case, then implement the evaluator/filter change. Test JSON null versus SQL NULL, coercion, overflow, timezone, and numeric boundaries. If a type/function row is split, both successor rows must pass their own gates; an unresolved function successor remains partial.
- [ ] Exercise the five disabled PostgreSQL function cases individually; classify each against Spanner's documented PostgreSQL subset before enabling it or recording a valid exclusion. Add a public-API test for any newly implemented query syntax.
- [ ] Implement PostgreSQL `SQL SECURITY DEFINER` only after task 2 supplies role identity and privilege checks. Enable only DDL fixture variants documented by Spanner. Test invoker and definer access with different principals.
- [ ] Populate documented dynamic `pg_catalog` and `INFORMATION_SCHEMA` views from one schema/role source of truth. Enable the skipped UDF `pg_proc` case; test schema changes, role grants, both dialects, restart, and role-filtered INFORMATION_SCHEMA rows for two limited roles versus `spanner_info_reader`. Keep `SPANNER_SYS` in task 8.
- [ ] Expose and test remote UDF calls through task 6's packaged endpoint before closing the remote part of `schema.udfs`.

**Exit matrix**

| Current partial ID | Required passing behavior before `supported` |
| --- | --- |
| `schema.udfs` | SQL UDF lifecycle plus configured remote UDF success/error through the packaged gateway. |
| `types.json` | JSON type roundtrip and documented local JSON functions/edge cases; a split closes only when both successor records pass. |
| `types.uuid` | PostgreSQL and GoogleSQL UUID write/read/key/query roundtrip plus UUID function cases. |
| `googlesql.aggregate_functions` | Documented signatures and NULL/empty/overflow/window boundaries pass the function matrix. |
| `googlesql.string_functions` | Documented signatures, Unicode, bytes, regex, and bad-input cases pass. |
| `googlesql.date_timestamp_functions` | Documented signatures, timezone, DST, ISO week, and boundary cases pass. |
| `postgresql.queries` | Spanner PostgreSQL grammar/function matrix passes; disabled cases are resolved with evidence. |
| `postgresql.pg_catalog` | Documented catalog rows track schema/UDF/role changes, including the skipped `pg_proc` UDF case. |
| `postgresql.views` | Documented view variants and definer/invoker privilege behavior pass. |
| `postgresql.jsonb` | Documented JSONB operators/functions, null/path, and numeric limits pass; a split closes only when both successor records pass. |
| `metadata.information_schema` | Missing documented views/columns have correct schema, rows, and role-filtered visibility in both dialects. |
| `metadata.pg_catalog` | Same shared dynamic catalog behavior as `postgresql.pg_catalog`; no duplicate implementation. |

Focused targets: `//frontend/converters:values_test`, `//frontend/converters:reads_test`, `//backend/query:query_engine_test`, and `//tests/conformance/endpoints:emulator_conformance_test` with `--test_filter` for the touched suite.

### Task 2: Database roles and IAM enforcement

The currently `unsupported` `metadata.database_roles_metadata` record is a prerequisite even though it is not one of the 48 partial rows. Spanner's role DDL is currently discarded, and `ListDatabaseRoles` returns an empty list. LocalCloud's IAM policy store does not create SQL roles.

- [ ] Persist `CREATE/DROP ROLE`, `GRANT/REVOKE`, membership, and object privileges in the native schema model. Test invalid grants, role inheritance, backup/restart, `ListDatabaseRoles`, and role/privilege views.
- [ ] Define one authoritative policy store and an explicit Spanner resource-to-permission map. Make policy etags conditional writes; make `TestIamPermissions` calculate permissions rather than echo requests. Test project/instance/database/backup boundaries.
- [ ] First prove where direct SDK requests obtain a trusted principal; reject client-asserted identity metadata as an authorization source. Choose either native credential verification or a front proxy that exclusively owns both published ports, and test that the native backend is unreachable around it. Carry the verified principal and selected database role through every native data and admin RPC. If LocalCloud evaluates policies, use a private authenticated process bridge; the existing JVM-only KMS caller identity is not a cross-process credential.
- [ ] Keep standalone development mode explicit. In strict LocalCloud mode, deny missing identity and bridge failure; test policy updates, restart, service outage, two projects, and direct SDK/REST bypass attempts. Never claim enforcement from a gateway-only test.

| Current partial ID | Required passing behavior before `supported` |
| --- | --- |
| `security.iam_policies` | Etag CAS, calculated permissions, and enforced allow/deny on direct data and admin RPCs, including restart/outage. |

**Prerequisite exit gate:** `metadata.database_roles_metadata` moves from unsupported only after role/grant DDL persists, `ListDatabaseRoles` returns the created roles, privilege views reflect grants, and restricted sessions enforce them in both dialects. This additional record is not counted among the 48 partial rows.

Focused targets: `//frontend/handlers:policies_test`, role DDL and metadata conformance filters, and a new LocalCloud direct-SDK platform test.

### Task 3: Transaction and index semantics

- [ ] Replace the one-active-handle-per-database lock with key/range-scoped ownership. Test disjoint writes committing concurrently, overlapping writes and `FOR UPDATE` conflicts, deadlock/abort cleanup, and fault injection.
- [ ] Give `REPEATABLE_READ` a fixed snapshot plus write-conflict validation distinct from serializable. Test write skew/phantom-sensitive cases and rejected PDML isolation combinations against Spanner's documented rules.
- [ ] Make exact future timestamp reads wait until eligible or deadline/cancellation; test retention edges, bounded timestamp selection, and restart. Replica selection stays a cloud-only property.
- [ ] Enable the disabled invalidated-DML read test and assert transaction state for each error class. Preserve rollback behavior; avoid relying on noncontractual error text or violation order.
- [ ] Execute partitioned DML as independent local key-range transactions with correct non-atomic partial progress, idempotence rules, and row-count semantics. Do not claim distributed throughput.
- [ ] Implement an asynchronous local backfill operation with progress/cancellation and without holding the entire DDL transaction path for the full scan; test concurrent reads/writes, uniqueness failure, restart, and operation state.

| Current partial ID | Required passing behavior before `supported` |
| --- | --- |
| `writes.partitioned_dml` | Per-partition transactions, rejection rules, partial-failure semantics, and both dialects. |
| `transactions.stale_reads` | Exact/bounded/future timestamp, retention, deadline, and local timestamp behavior. |
| `transactions.select_for_update` | Row/range conflict behavior without blocking unrelated keys. |
| `transactions.transaction_errors` | Previously disabled case and transaction state/error-class matrix pass. |
| `transactions.isolation_levels` | Repeatable-read snapshot/write-conflict behavior differs correctly from serializable. |
| `transactions.concurrency_model` | Concurrent disjoint work succeeds; overlapping work aborts/retries predictably. |
| `indexes.index_backfill` | Correct backfill with usable operation lifecycle and concurrent access at local scale. |

Focused targets: `//backend/locking:manager_test`, `//backend/transaction:read_write_transaction_test`, affected backfill tests, and conformance filters for `partitioned_dml`, `snapshot_reads`, `select_for_update`, and `transaction_errors`.

### Task 4: Change streams and backup lifecycle

- [ ] Make REPLACE on an existing row emit DELETE then INSERT, preserve deterministic per-transaction record order across tables, and test retry from saved position after restart. Reconnect after process restart is expected; survival of one in-flight RPC is not.
- [ ] Implement mutable-key-range MOVE records and correct active-partition routing. Test inserts, updates, deletes, child/heartbeat records, retention, churn, and full replay after restart.
- [ ] Add PostgreSQL `NEW_ROW_AND_OLD_VALUES` and reset/NULL transitions; exercise the commented DDL fixture cases only where Spanner documents them.
- [ ] Create historical `version_time` backups within retention; restore consistent data/schema/options/sequence state. Spanner restores change-stream definitions but excludes their internal history: test a new stream start boundary, fresh readability, and post-restore records, never pre-backup replay. Inject failures and verify rollback or restart reconciliation without a half-created target.
- [ ] Apply documented backup list filters, expire backups, and run schedules exactly once per due time through restart. Test schedule changes, retention, pagination, and cleanup.
- [ ] For CMEK, build a secured LocalCloud KMS adapter and wire it through task 7's packaged command. Encrypt actual backup payloads, store key-version provenance, and test disabled/wrong-project/rotated keys. If no KMS provider is configured, reject encryption requests explicitly; never store a key name while writing plaintext and call that encryption.

| Current partial ID | Required passing behavior before `supported` |
| --- | --- |
| `change_streams.read` | REPLACE shape, cross-table order, retry position, and restart replay pass. |
| `change_streams.key_ranges` | MOVE and all mutation record shapes pass in both dialects. |
| `change_streams.partition_churn` | Routed partitions, child/heartbeat lifecycle, and complete persisted churn replay pass. |
| `postgresql.change_streams` | Documented capture/reset variants and shared record semantics pass. |
| `backups.create` | Historical snapshot and actual requested encryption behavior pass. |
| `backups.restore` | Data/schema/options/sequence restore, fresh change-stream operation without old internal history, and failure recovery pass. |
| `backups.metadata` | Filtered pagination, expiry deletion, copy/update, and restart pass. |
| `backups.schedules` | Due schedules create backups once, with retention and restart recovery. |

Focused targets: `//frontend/handlers:backups_test`, `//backend/database/change_stream:change_stream_partition_churner_test`, relevant conformance filters, and a packaged LocalCloud KMS/backup platform test.

### Task 5: Search and vector behavior

- [ ] Implement documented `ALTER SEARCH INDEX` and observable index-option behavior. Test DDL roundtrip, index maintenance, both dialects, and error cases.
- [ ] Add Unicode/language/diacritic/content-type/token-category fixtures for tokenizers and SEARCH. Implement documented option effects or split out explicitly unsupported options. Verify read-write transaction hint behavior.
- [ ] Inventory SCORE dialect, language_tag, enhance_query, dictionary, and JSON options (including version). For each locally relevant documented value, assert an expected ordering, NULL result, or validation error; do not assert numeric equality to Google's private relevance implementation.
- [ ] Add exact COSINE_DISTANCE, EUCLIDEAN_DISTANCE, and DOT_PRODUCT tests for ARRAY<FLOAT32> and ARRAY<FLOAT64>, including PostgreSQL `spanner.*` signatures and NULL/zero/dimension/nonfinite cases. BigQuery's corpus covers only cosine/euclidean FLOAT64: adapt those inputs, add the missing metrics/types, then judge expected results against Spanner documentation and API behavior.
- [ ] Keep exact scanning as an oracle. For `vector.ann_indexes`, prototype a maintained local coarse index whose search option changes the candidate set; test index create/backfill/update/delete/rebuild/restart, filtered queries, and documented query-shape errors. On a fixed seeded 10,000-vector fixture, require recall@10 of at least 0.90 for each distance metric, a search-option change that alters the visited candidate count, and at least one query that visits fewer than all vectors. Do not copy BigQuery's exact `VECTOR_SEARCH` and label it ANN.

| Current partial ID | Required passing behavior before `supported` |
| --- | --- |
| `search.search_indexes` | Documented create/alter/drop/options and consistent indexed reads pass. |
| `search.search_functions` | Search query grammar, language/enhancement options, and transaction restrictions pass. |
| `search.tokenization` | Documented Unicode, diacritic, content, and category options produce tested tokens. |
| `search.scoring` | Documented dialect/language/enhancement/dictionary/JSON options each have tested ranking, NULL, or error behavior. |
| `vector.distance_functions` | All three metrics and both float array types, including PostgreSQL signatures and edge/error cases, pass. |
| `vector.ann_indexes` | A maintained approximate local index passes the fixed recall@10 >= 0.90 gate, candidate-count option test, and restart test; production latency is excluded. |

Focused targets: search tokenizer/evaluator tests under `backend/query/search/`, `//tests/conformance/endpoints:emulator_conformance_test` filters for search and ANN. BigQuery's `qualification/search/distance_vectors.json` is a test-input source, not Spanner parity evidence.

### Task 6: Real remote UDF and ML responses

- [ ] Forward `--remote_functions_host_port` or an equivalent explicit endpoint from `gateway_main` through validated `localcloud.defaults.yaml` settings, the entrypoint/Supervisor command, and the Docker wrapper to `emulator_main`; test the actual packaged command, service restart, and endpoint reachability. The native remote transport currently accepts only `localhost:`, so use an authenticated loopback adapter rather than assuming a Docker service name works.
- [ ] Bind model identity to a configured provider and persist it. Translate Spanner's Remote UDF calls/replies protocol at the adapter boundary; validate schema, timeout, provider error, and restart. The configured-provider path already routes to remote prediction: verify that provider errors never fall back to fingerprint output. Decide and document whether the unconfigured path remains an explicit test stub or returns a missing-provider error.
- [ ] Exercise `ML.PREDICT`, `spanner.ml_predict_row`, and `AI.*` through gRPC and REST against a small local inference fixture that computes an input-dependent result (for example, y = 2x + 1), then test malformed replies and provider downtime. Repeat through an immutable LocalCloud candidate image. LocalCloud Vertex AI currently exposes deterministic generation/embedding stubs, not model prediction, so it cannot qualify this step by itself.

| Current partial ID | Required passing behavior before `supported` |
| --- | --- |
| `ml.ml_predict` | Both dialects receive provider-produced predictions and correct provider errors in packaged LocalCloud. |
| `ml.remote_models` | Configured model/UDF calls use a real provider binding through restart; no fake successful fallback. |

Focused targets: `//backend/query/ml:model_evaluator_test`, `//backend/query/remote_udf:remote_udf_evaluator_test`, gateway tests, and a LocalCloud direct-SDK platform test.

### Task 7: Admin API, quotas, REST, and clients

- [ ] Validate and propagate LocalCloud Spanner IAM/KMS/remote-function settings through `localcloud.defaults.yaml`, the entrypoint, Supervisor, and the native gateway/wrapper command. Test the command after service restart; the retired `spanner.env` adapter is not sufficient.
- [ ] Implement the documented mutable fields and mask validation for instance/database updates. Make database drop protection effective when deleting an instance; verify persistence and bad-mask errors.
- [ ] Verify custom instance configs, partitions, placements, and moves as consistent local metadata workflows. Keep physical replica/capacity/data movement separate as cloud-only unless a genuine multi-node simulator is introduced.
- [ ] Return supported database options and retention timestamps through `GetDatabase`; test option effects and restart. Keep physical leader placement separate.
- [ ] Complete backup-independent operation status behavior for `InternalUpdateGraphOperation` only to the documented local API contract; test success/failure and any graph backfill effect before claiming more.
- [ ] Enable disabled table/index quota tests and add local mutation/commit-size validation. Split production API rate/capacity throttling from functional limits.
- [ ] Preserve long REST error messages and standard details; test camelCase field masks and >1 KiB errors through the packaged gateway. Run C++, Java, Go, Python, and Node client smoke workflows against both dialects where each client supports them; record actual SDK versions and failures.

| Current partial ID | Required passing behavior before `supported` |
| --- | --- |
| `instance.update` | Supported update masks have real local state/readback/persistence and invalid masks fail correctly. |
| `instance.configs` | Config CRUD and references pass; physical replicas/regions are explicitly out of local scope. |
| `ops.instance_partitions` | Partition CRUD, placements, deletion guards, and restart pass; physical capacity is separated. |
| `instance.move` | Config/reference move is atomic and persists; physical relocation is separated. |
| `database.update` | Mutable fields and drop protection, including instance deletion, pass. |
| `database.internal_graph_operation` | Documented local operation status and graph effects pass or the internal-only scope is split. |
| `schema.database_options` | Observable options, `GetDatabase` fields, and restart pass; leader physics is separated. |
| `ops.quotas` | Local schema/mutation limits and boundary errors pass; cloud rate/capacity quotas are separated. |
| `clients.rest_gateway` | Codes/messages/details/field masks survive direct REST and packaged gateway tests. |
| `clients.client_libraries` | A recorded multi-language SDK matrix passes representative admin/query/transaction flows. |

Focused targets: matching `//frontend/handlers:*_test` targets, `//gateway:gateway_test`, `//tests/gcloud:instance_admin_test`, and LocalCloud `SpannerNativeSdkSurfacePlatformTest` plus related admin platform tests.

### Task 8: Local plans and local statistics

- [ ] Return a real emulator logical operator tree for PLAN without executing the query. PROFILE must report measured local operator rows/timing with stable structure; label figures as local, not production optimizer estimates.
- [ ] Inventory all documented SPANNER_SYS families at task 0. At minimum expose QUERY_STATS_TOP_MINUTE, READ_STATS_TOP_MINUTE, TXN_STATS_TOP_MINUTE, and LOCK_STATS_TOP_10MINUTE with nonzero local counts/timings from executed operations, plus empty/startup, retention-window, role-filtered access, and restart tests. Split physical-only statistics into explicit not-applicable records. LocalCloud Monitoring export may be additive; it does not populate SQL tables automatically.

| Current partial ID | Required passing behavior before `supported` |
| --- | --- |
| `googlesql.query_modes` | PLAN/PROFILE return meaningful local operator structure and measured profile data. |
| `metadata.spanner_sys` | At least the named query/read/transaction/lock tables report measured local rows and enforce role visibility; other locally meaningful families are inventoried and closed. |

Focused targets: `tests/conformance/cases/query_modes.cc`, `tests/conformance/cases/spanner_sys.cc`, and query/locking component tests.

### Task 9: Qualification and catalog closure

- [ ] For each exit-matrix row, attach the exact implementation and test paths, test result, supported runtime profile, and remaining exclusions. Reclassify only after its own gate passes. If a physical residual is split, keep its new `not-applicable` record visible.
- [ ] Run focused native tests after each change; then the full conformance target and feature-coverage checks. Run `python3 tools/feature_coverage.py audit-rpcs`, `generate`, `check`, and `python3 -m unittest tools.feature_coverage_test`.
- [ ] Build an immutable Spanner image and then a LocalCloud candidate image verified against the Spanner image ID. Require a registry digest only when publishing an image; local qualification uses the immutable local ID. Run direct SDK platform tests for IAM, backups/KMS, change streams, search/vector, and ML; test restart with disposable volumes and exact published ports. Verify actual gRPC/REST service behavior, not only health.
- [ ] Re-read `docs/capabilities.md`, `docs/known-gaps.md`, `docs/configuration.md`, `docs/persistence.md`, `docs/change-streams.md`, LocalCloud `documentation.yaml`, and generated docs for contradictions. Record any unqualified or product-only behavior instead of silently upgrading status.

**Completion rule:** all 48 original IDs have a supported local observable contract with passing evidence, or an explicit split that keeps its unsimulatable production behavior visible as `not-applicable`. Splitting JSON/JSONB or another broad row does not close it while a locally meaningful successor remains partial. The additional role-metadata prerequisite must pass its task 2 gate. Final counts will reflect splits; they are not predetermined. The unrelated `unknown` and other accepted-no-op/unsupported records are not silently counted as complete.

## Reuse and non-reuse decisions

- **BigQuery vector:** its current `VECTOR_SEARCH` is a bounded exact scan and excludes indexed approximate mode. Its reference corpus covers cosine/euclidean FLOAT64 only; reuse its numeric edge-case inputs and qualification discipline, then add Spanner's DOT_PRODUCT and FLOAT32 cases; Spanner needs its own ANN index if `vector.ann_indexes` is to become supported.
- **BigQuery ML:** reuse explicit provider binding and refusal to fake configured-provider success. Its OpenAI-style transport and local training kernels do not implement Spanner's Remote UDF protocol.
- **BigQuery geography:** its S2/Python/DuckDB kernel does not map to any current Spanner partial row. Spanner's documented GoogleSQL type list has no GEOGRAPHY entry; do not add geo work to this 48-row plan without a new Spanner contract.
- **LocalCloud IAM:** the Java policy evaluator is useful, but Spanner's direct ports bypass it today. Native identity propagation and all-path enforcement are required.
- **LocalCloud KMS:** its service-agent envelope pattern is useful for CMEK, but backup bytes need native encryption and key-version provenance.
- **LocalCloud PostgreSQL/AlloyDB, Vertex AI, Monitoring:** these are separate services. They supply possible test infrastructure or adapters; their presence does not establish Spanner PG, ML, vector, or `SPANNER_SYS` support.

## Official contract references

- [Spanner SQL types](https://docs.cloud.google.com/spanner/docs/reference/standard-sql/data-types), [GoogleSQL DDL](https://docs.cloud.google.com/spanner/docs/reference/standard-sql/data-definition-language), [PostgreSQL language scope](https://docs.cloud.google.com/spanner/docs/reference/postgresql/overview), [PostgreSQL functions](https://docs.cloud.google.com/spanner/docs/reference/postgresql/functions)
- [IAM](https://docs.cloud.google.com/spanner/docs/iam), [fine-grained access control](https://docs.cloud.google.com/spanner/docs/fgac-about), [transactions](https://docs.cloud.google.com/spanner/docs/transactions), [repeatable read](https://docs.cloud.google.com/spanner/docs/isolation-levels), [partitioned DML](https://docs.cloud.google.com/spanner/docs/dml-partitioned)
- [Change streams](https://docs.cloud.google.com/spanner/docs/change-streams), [search functions](https://docs.cloud.google.com/spanner/docs/reference/standard-sql/search_functions), [vector indexes](https://docs.cloud.google.com/spanner/docs/vector-indexes), [ANN query behavior](https://docs.cloud.google.com/spanner/docs/find-approximate-nearest-neighbors), [backups](https://docs.cloud.google.com/spanner/docs/backup), [restore exclusions](https://docs.cloud.google.com/spanner/docs/backup/restore-backup-overview), [query statistics](https://docs.cloud.google.com/spanner/docs/introspection/query-statistics)

<!-- generated-by: gsd-doc-writer -->
# Testing

## Test framework and setup

The emulator's C++ unit and conformance tests use GoogleTest through Bazel
`cc_test` targets. The gateway has a Bazel `go_test` target. The `tests/gcloud`
targets are Bazel `py_test` tests that launch the emulator and invoke `gcloud`;
set `GCLOUD_DIR` to the directory containing the `gcloud` executable before
running them. Feature coverage tooling has separate Python `unittest` tests.

Use Bazel 7.6.1 from [`.bazelversion`](../.bazelversion). See
[Building](building.md#native-macos-bazel) for the compiler, protobuf, SDK,
and cache setup needed for native macOS builds. Bazel fetches the test
dependencies. The C++ conformance target starts a local gRPC server and uses
the Google Cloud C++ Spanner client; it is declared as a 32-shard test in
[`tests/conformance/endpoints/BUILD`](../tests/conformance/endpoints/BUILD).

## Running tests

Run a focused C++ unit target (replace the target with the one beside the
source you changed):

```bash
bazel test //backend/storage:sequence_state_store_test
```

Run the Go gateway test or a gcloud CLI test:

```bash
bazel test //gateway:gateway_test
GCLOUD_DIR=/path/to/gcloud-directory bazel test //tests/gcloud:instance_admin_test
```

Run the emulator conformance suite, or narrow it to one GoogleTest class:

```bash
bazel test //tests/conformance/endpoints:emulator_conformance_test
bazel test --test_filter='PGFunctionsTest.*' //tests/conformance/endpoints:emulator_conformance_test
```

The repository's Kokoro build script uses this broad Bazel command. It
excludes `third_party/spanner_pg/src/...`, where some vendored targets do not
build. Expect a long cold build; use focused targets during development.

```bash
bazel test -c opt -- ... -third_party/spanner_pg/src/...
```

External client libraries and drivers (Python, Go, Node, Java, JDBC,
PGAdapter with pgJDBC and psql, and REST through `curl`) have smoke programs in
[`tests/client_matrix`](../tests/client_matrix/). They are not Bazel targets:
start `gateway_main` and run them by hand, as that directory's
[README](../tests/client_matrix/README.md) shows for every client. For
example, from the repository root with built binaries:

```bash
./gateway_main --grpc_binary=./emulator_main --hostname=localhost \
  --grpc_port=19410 --http_port=19420 &
export SPANNER_EMULATOR_HOST=localhost:19410
python3 tests/client_matrix/python/smoke.py   # needs google-cloud-spanner
REST=http://localhost:19420 bash tests/client_matrix/rest/rest_smoke.sh
```

You can also run the client matrix directly against a running Docker container
on standard ports:

```bash
docker run -d --name spanner-emulator -p 9010:9010 -p 9020:9020 localcloud-spanner-emulator:local
export SPANNER_EMULATOR_HOST=localhost:9010
REST=http://localhost:9020 bash tests/client_matrix/rest/rest_smoke.sh
python3 tests/client_matrix/python/smoke.py
```

To run the end-to-end Docker image qualification suite (covering volume
persistence across both dialects, directory locking, flag forwarding, and feature
spot checks):

```bash
python3 tests/image_verification_test.py
```

Each program creates its own instance, prints one `PASS`/`FAIL` line per step,
and exits non-zero if any step fails. The README lists the last passing client
versions.

For the feature coverage inventory, run both checks from the repository root:

```bash
python3 -m unittest tools.feature_coverage_test
python3 tools/feature_coverage.py check
```

The first command tests inventory validation and rendering logic; the second
checks that [`feature-coverage.md`](feature-coverage.md) matches
[`feature-coverage.yaml`](feature-coverage.yaml). Neither command executes
emulator feature behavior. There is no configured watch-mode command.

## Writing new tests

- Put C++ unit tests beside the affected source as `*_test.cc` and add a
  `cc_test` target in that package's `BUILD` file. Existing examples include
  [`backend/storage/sequence_state_store_test.cc`](../backend/storage/sequence_state_store_test.cc)
  and [`frontend/common/status_test.cc`](../frontend/common/status_test.cc).
- Put end-to-end feature cases in `tests/conformance/cases/`, register the
  source in that directory's `BUILD` file, and use the existing `DatabaseTest`
  fixtures. The endpoint target links those cases and runs them against the
  local server.
- For gateway or CLI behavior, follow `gateway/gateway_test.go` or the
  `tests/gcloud/*_test.py` pattern. The gcloud tests use the shared
  `tests/gcloud/emulator.py` harness to start and stop the emulator.
- When changing [`feature-coverage.yaml`](feature-coverage.yaml), cite the
  relevant implementation and test paths for each claim. Run
  `python3 tools/feature_coverage.py audit-rpcs`, then `generate`, the
  `check` command above, and the Python unit tests. A test path in the
  inventory is evidence to review, not proof that every variant is covered.

## Qualification evidence

The 2026-09-28 developer-usability closure
([plan](plans/2026-09-27-usability-closure-plan.md),
[worksheet](plans/2026-09-27-usability-audit-worksheet.md)) recorded the
evidence behind the current coverage statuses, all on native builds:

- the full native suite with fixed-environment Bazel
  (`//tests/conformance/... //backend/... //frontend/... //gateway/... //common/...`
  plus `//third_party/spanner_pg/ddl:ddl_test` and
  `//third_party/spanner_pg/catalog:all`): 159 of 159 targets passed;
- the client matrix above against the final `emulator_main` and
  `gateway_main`: every client passed;
- the official documentation examples (1,165): 1,051 passed with no emulator
  defects; the rest are order-unspecified results, documentation errors, the
  unstable `DEBUG_TOKENLIST` format, or skipped examples;
- a disposable `--data_dir` stop and restart probe in both dialects: 33 of 33
  checks passed.

The remaining-limitations batches that followed (graph algorithms, search,
resume tokens, wound-wait lock waits, persisted `SPANNER_SYS` statistics, the
`--data_dir` range-scan fix and lock, and more; see the
[changelog](CHANGELOG.md#2026-09-28-remaining-limitations)) were verified with
the combined native suite only: 164 of 164 targets passed with
fixed-environment Bazel on commit `bd6a9646`. The client matrix and the
restart probe were not re-run for them, so those results still describe the
closure build. The coverage inventory now counts 155 records: 138 supported,
8 accepted without effect, and 9 not applicable.

### Packaged Docker image qualification (2026-09-28)

Following the native build rounds, the packaged Docker image
`localcloud-spanner-emulator:local` (built via `./build.sh`) was 100% qualified
across all five verification tiers:

1. **Client compatibility matrix** (`tests/client_matrix/` against `localhost:9010`
   and `http://localhost:9020`): REST (22/22 with Google JSON error envelopes
   `{"error":{"code","message","status"}}` for 400, 403, 404, 409), Go (16/16),
   Node.js (16/16), Python (16/16), Java (16/16), JDBC (18/18), PGAdapter
   (13/13), and psql (100%).
2. **Persistent volume restart across dialects** (`tests/image_verification_test.py`):
   GoogleSQL and PostgreSQL databases with database roles, DEFINER views,
   database options (`versionRetentionPeriod = 2h`), TTL policies (`ADD ROW
   DELETION POLICY` and `TTL INTERVAL`), sequences (`BIT_REVERSED_POSITIVE`),
   unique secondary indexes, point-in-time backup restore (`version_time`),
   split points (`AddSplitPoints`), and `SPANNER_SYS` query statistics survived
   container stop and restart on the same Docker volume.
3. **Data directory locking** (`tests/image_verification_test.py`): A second
   container attempting to mount the active `--data_dir` volume exited cleanly
   with code 1 and error message `is in use by another emulator process`,
   protecting against concurrent storage corruption.
4. **Flag forwarding** (`tests/image_verification_test.py`): Verified
   `--row_deletion_policy_sweep_interval_seconds=5` (expired row swept within
   5s) and `--spanner_sys_expose_open_interval=true` (query stats immediately
   visible in `SPANNER_SYS.QUERY_STATS_TOP_MINUTE`).
5. **Feature spot checks** (`tests/image_verification_test.py`): Verified
   `REPEATABLE_READ` snapshot conflict aborts (409), wound-wait lock wait and
   commit (~1.5s wait), `PLAN` and `PROFILE` query plans (16-node trees with
   execution statistics), GQL property graph `CALL PageRank(...)` score
   calculations, full-text Unicode search with `SEARCH(TOKENIZE_FULLTEXT(...), ...)`,
   and PostgreSQL `CREATE INDEX ... USING scann` with `spanner.approx_cosine_distance`.

## Coverage requirements

| Type | Minimum threshold |
|------|-------------------|
| Lines, branches, functions, statements | None configured in this repository |

The feature coverage inventory records per-feature status and evidence; it is
not a quantitative code coverage report.

## CI integration

- [`.github/workflows/feature-coverage.yml`](../.github/workflows/feature-coverage.yml)
  runs the Python unit tests and generated-document check on pushes and pull
  requests that change its listed coverage-related paths.
- [`.github/workflows/docker-publish.yml`](../.github/workflows/docker-publish.yml)
  builds the Docker image on its scheduled, tag, and manual triggers. Its
  [`Dockerfile`](../build/docker/Dockerfile.ubuntu) runs Bazel against the two
  binaries and `//frontend/collections:database_manager_test`; this is a
  focused build test, not the full Bazel suite.
- The checked-in Kokoro [presubmit config](../build/kokoro/gcp_ubuntu/presubmit.cfg)
  invokes `docker_test.sh`, which runs the broad Bazel suite through
  [`build_and_test.sh`](../build/kokoro/gcp_ubuntu/build_and_test.sh) before
  configured client integration tests. The repository files do not establish
  whether that external Kokoro job is currently active for this fork.

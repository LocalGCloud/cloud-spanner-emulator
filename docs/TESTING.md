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

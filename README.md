<!-- generated-by: gsd-doc-writer -->
# Cloud Spanner Emulator (Extended)

This is LocalGCloud's fork of Google's
[Cloud Spanner Emulator](https://github.com/GoogleCloudPlatform/cloud-spanner-emulator),
a locally running emulated Cloud Spanner for development and testing. It adds
persistent storage, backups, more of the admin API, geo-partitioning, change
streams that survive restarts, and fixes, and it is the Spanner runtime bundled
by LocalCloud.

Like upstream, it aims to make application tests portable to Cloud Spanner,
subject to the [documented differences](docs/known-gaps.md). It isn't meant
to be a production database or to match Cloud Spanner's performance.

- Image: [`jaysen2apache/spanner-emulator-extended`](https://hub.docker.com/r/jaysen2apache/spanner-emulator-extended)
  (linux/amd64 and linux/arm64)
- Fork development branch: `jay-spanner-extended`.

## Installation

Pull the published Docker image:

```shell
docker pull jaysen2apache/spanner-emulator-extended
```

Or build the Docker image locally using the cached build script:

```shell
./build.sh                         # builds and tags spanner-emulator-extended:local
```

See [Building](docs/building.md) for build options (`--platform`, `--jobs`, `--skip-tests`, cache management). Ordinary pushes do not
publish an image, so `latest` can lag this checkout; use a published commit
tag or build locally when you need a specific revision.

## Quickstart

```shell
# In-memory, like upstream
docker run -p 9010:9010 -p 9020:9020 jaysen2apache/spanner-emulator-extended

# With persistent storage in /path/to/data
docker run -p 9010:9010 -p 9020:9020 -v /path/to/data:/data \
  jaysen2apache/spanner-emulator-extended \
  ./gateway_main --hostname 0.0.0.0 --data_dir=/data
```

Port 9010 serves gRPC and 9020 serves REST. Point client libraries at the
emulator with `SPANNER_EMULATOR_HOST=localhost:9010`. Without `--data_dir`,
data is lost when the container stops; with it, the mounted directory retains
the supported persistent state across restarts.

The image has no `ENTRYPOINT`, so to pass flags, give the whole command as
above. Appending only flags fails to start. Common flags are described in
[Configuration](docs/configuration.md).

## What this fork adds

The full list, with limits for each, is in [Capabilities](docs/capabilities.md).
Of the 155 features in the [coverage matrix](docs/feature-coverage.md), 138 are
supported, 8 are accepted without effect, and 9 don't apply locally
(2026-09-28).

- **Persistence** with `--data_dir`: rows, schema, sequence counters,
  instances, instance partitions, IAM policies, operations, backups and
  `SPANNER_SYS` statistics survive restarts. The directory is locked against a
  second emulator, and a database that fails to restore is isolated instead of
  stopping the emulator.
  See [Persistence](docs/persistence.md).
- **Backups and restore** with `--data_dir`, including `version_time`
  backups and full and incremental schedules; custom instance configs, drop
  protection, and IAM policy storage (not enforced).
- **Database roles and fine-grained access control**: `GRANT`/`REVOKE` in both
  dialects, enforced for sessions with a `creator_role`, with the
  `INFORMATION_SCHEMA` role and privilege views.
- **`SPANNER_SYS` statistics and query plans**: measured query, read,
  transaction, lock and table size statistics, active partitioned DMLs, row
  deletion policies and user split points, and plan trees for
  `PLAN`/`PROFILE`.
- **Wound-wait lock waits and resume tokens**: conflicting transactions wait
  (`--lock_wait_timeout_ms`) instead of aborting at once, and streaming reads,
  queries and change stream queries resume from their resume tokens.
- **Graph algorithms**: the 13 documented algorithms (PageRank, centrality,
  clustering, similarity, `ShortestPath` and more) compute results, and
  `EXPORT DATA` writes them back to tables.
- **Row deletion policies (TTL)** that delete expired rows, and
  `REPEATABLE_READ` isolation.
- **Change streams that survive restarts**: definitions, records and
  partition history persist with `--data_dir`, so reads can start before a
  restart. See [Change streams](docs/change-streams.md).
- **Geo-partitioning**: placements and placement keys in both dialects. See
  [Placements](docs/placements.md).
- **Fixes**: REST errors use Google's JSON error envelope with the real
  status; unique indexes are re-checked at commit;
  `LOCK_SCANNED_RANGES=exclusive` takes exclusive locks and a deadlock aborts
  exactly one transaction; DML `UPDATE` change stream records carry only the
  changed columns; search over NULL token lists no longer crashes or returns
  wrong rows; PostgreSQL `jsonb || jsonb`, negative `->`/`->>` indexes and
  `regexp_replace` match the documentation; `SNIPPET`, `TO_BASE32`,
  `FROM_BASE32`, `ZSTD_*`, PostgreSQL `generate_series` and large JSONB numbers
  on macOS work; `ListBackups` filters follow AIP-160 precedence;
  `TABLESAMPLE ... REPEATABLE` and the `OPTIMIZER_VERSION` hint (on queries
  and DML) are accepted; `--data_dir` range reads no longer skip rows (the
  cause of duplicate unique index keys); full-text search handles Unicode
  RQUERY terms, HTML 4 entities and `DEBUG_TOKENLIST` as documented, and
  PostgreSQL `spanner.tokenize_substring` with `relative_search_types` no
  longer crashes; `MUTABLE_KEY_RANGE` change streams no longer re-send
  partition start records. See the [changelog](docs/CHANGELOG.md).
- **Builds and qualification**: multi-arch images, native macOS arm64 builds,
  and cached local builds via `./build.sh` (BuildKit cache mounts and local Bazel
  disk cache). The packaged Docker image is 100% qualified across external
  client SDKs and persistence/feature test suites (`tests/image_verification_test.py`).
  See [Building](docs/building.md) and [Testing](docs/TESTING.md).

## Known gaps

Everything known not to work, or to work differently from Cloud Spanner, is in
[Known gaps](docs/known-gaps.md). The most important:

- Read-write transactions can overlap; a younger transaction waits for an
  older one and can still abort. Retry aborted transactions.
- IAM policies are stored but never enforced.
- Backups need `--data_dir`, and every backup is a full copy;
  customer-managed encryption keys are not supported.
- `APPROX_*` vector searches return exact results, and some graph algorithm
  results are emulator-defined where Spanner doesn't publish its method.
- Change stream partitioning is simulated: all writes go to one partition.
- This fork hasn't merged upstream releases after 2026-08-03.

## Documentation

| Document | Covers |
|----------|--------|
| [Capabilities](docs/capabilities.md) | What works, by area, including what this fork adds |
| [Known gaps](docs/known-gaps.md) | What doesn't work or differs from Cloud Spanner |
| [Feature coverage](docs/feature-coverage.md) | Per-feature status with evidence; machine-readable in [`feature-coverage.yaml`](docs/feature-coverage.yaml) |
| [Getting started](docs/GETTING-STARTED.md) | Run the published image and connect a client |
| [Configuration](docs/configuration.md) | Binaries, flags, environment variables, Docker usage |
| [Architecture](docs/ARCHITECTURE.md) | Gateway, frontend, backend and persistence flow |
| [Development](docs/DEVELOPMENT.md) | Local setup and build workflow |
| [Testing](docs/TESTING.md) | Test targets, the client matrix, and qualification evidence |
| [Persistence](docs/persistence.md) | `--data_dir`, backups, recovery, on-disk layout |
| [Change streams](docs/change-streams.md) | Creating and reading change streams, limits, differences |
| [Placements](docs/placements.md) | Geo-partitioning |
| [Building](docs/building.md) | Native, Docker and CI builds, caches, releasing |
| [Changelog](docs/CHANGELOG.md) | Changes in this fork |
| [Internals](docs/internals/) | Design notes for persistent storage and change streams |
| [Plans](docs/plans/) | Dated design records |

## Other ways to run

- **From source**: `bazel run //binaries:gateway_main`, or `./build.sh` for a
  Docker image. See [Building](docs/building.md).
- **Upstream builds**: `gcloud emulators spanner start`, the
  `gcr.io/cloud-spanner-emulator/emulator` image and upstream's Linux tarballs
  run Google's emulator, without this fork's features. See
  [upstream's README](https://github.com/GoogleCloudPlatform/cloud-spanner-emulator#readme).

## Technical details

The emulator is built on the [GoogleSQL](https://github.com/google/googlesql)
reference implementation and has three layers:

- a REST gateway generated by [grpc-gateway](https://github.com/grpc-ecosystem/grpc-gateway)
  (Go, `gateway/`);
- a gRPC frontend implementing Cloud Spanner's API (`frontend/`);
- a database backend emulating Cloud Spanner's database features (`backend/`).

GoogleSQL provides SQL query execution, values, types and SQL functions. This
codebase implements the API surface, DDL, transactions, constraint enforcement
and storage (in memory, or LevelDB with `--data_dir`). PostgreSQL-dialect
support comes from a port of PostgreSQL's parser in `third_party/spanner_pg`.

## FAQ

#### Which client libraries are tested here?

The conformance suite uses the Google Cloud C++ Spanner client against a real
gRPC endpoint. On 2026-09-28 a matrix in
[`tests/client_matrix`](tests/client_matrix/) passed against native builds with
Python 3.71.0, Go 1.95.1, Node 9.0.0, Java 6.123.0, JDBC 2.45.0, PGAdapter
0.55.3 (with pgJDBC and psql), and REST through `curl`. It hasn't been re-run
since the later remaining-limitations batches, which were verified with the
native test suite only. Set
`SPANNER_EMULATOR_HOST` to the emulator's gRPC address and follow your client
library's emulator setup instructions.

#### What's the recommended test setup?

Run one emulator process and create an instance in it. Databases are cheap to
create, so let each test create and drop its own database. Tests stay isolated
and can run in parallel.

#### Why does row order change between runs?

Queries without `ORDER BY` have no guaranteed row order. Add `ORDER BY` when a
test depends on the order.

#### Why does the emulator fail with "Check failed: LoadTimeZone(...)"?

The emulator needs the system's [tzdata](https://www.iana.org/time-zones)
files. Install tzdata on the machine or image that runs it.

## Issues and contributions

This fork is maintained for LocalCloud at
[LocalGCloud/cloud-spanner-emulator](https://github.com/LocalGCloud/cloud-spanner-emulator);
report problems with it to the LocalCloud maintainers. Bugs that also affect
Google's emulator can be reported
[upstream](https://github.com/GoogleCloudPlatform/cloud-spanner-emulator/issues).
See [CONTRIBUTING.md](CONTRIBUTING.md) for this fork's contribution policy.

## Security

See [SECURITY.md](SECURITY.md).

## License

[Apache License 2.0](LICENSE)

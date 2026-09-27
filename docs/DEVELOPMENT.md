<!-- generated-by: gsd-doc-writer -->
# Development

## Local setup

This repository builds the C++ emulator and Go REST gateway with Bazel 7.6.1
(pinned in `.bazelversion`). Bazel fetches their dependencies; there is no
separate package installation step. On macOS, install Xcode Command Line Tools
and the tools used by the native build:

```bash
git clone https://github.com/LocalGCloud/cloud-spanner-emulator.git
cd cloud-spanner-emulator
xcode-select --install
brew install bazelisk gnu-sed protobuf
```

If only Command Line Tools are installed, Bazel may need an explicit macOS SDK
version. Follow the native macOS command and cache setup in
[Building the Emulator](building.md#native-macos-bazel) before a long build.
The Docker build needs Docker Buildx; `./build.sh` additionally uses a host
Bazel executable in its default offline mode. `./build.sh --online` downloads
dependencies inside the build instead. See [Building the Emulator](building.md#local-docker-image-buildsh).

## Build commands

Run these from the repository root after the platform setup in
[Building the Emulator](building.md). Its macOS command pins the compiler,
`protoc`, SDK, job count, and disk cache for repeatable native builds.

| Command | Purpose |
|---------|---------|
| `bazel build //binaries:emulator_main //binaries:gateway_main` | Build the gRPC emulator and REST gateway binaries. |
| `bazel test //frontend/common:status_test` | Run one small C++ test target. |
| `bazel test //gateway:gateway_test` | Run the Go gateway test target through Bazel. |
| `bazel test //tests/conformance/endpoints:emulator_conformance_test` | Run the sharded Spanner conformance target. |
| `./build.sh --online` | Build and load `spanner-emulator-extended:local` for Linux arm64 by default; use `--platform=amd64` for amd64. |
| `python3 -m unittest tools.feature_coverage_test` | Run feature coverage tooling tests. |
| `python3 tools/feature_coverage.py check` | Check that the generated feature coverage Markdown matches its YAML inventory. |

When changing the coverage inventory, run
`python3 tools/feature_coverage.py audit-rpcs`, then
`python3 tools/feature_coverage.py generate`, and finally the two coverage
checks above. The [coverage document](feature-coverage.md) describes the
inventory workflow.

## Code style

`.bazelrc` selects C++20 for C++ targets; `go.mod` declares Go 1.25.0 for Go
code. Follow the style of the surrounding source and tests. This repository
does not define a dedicated formatter or lint command or a repository-level
format configuration; build and test the affected Bazel targets before review.

## Branch conventions

The repository contains `master` and uses `jay-spanner-extended` for this
fork's development and manual image publishing. No branch naming convention is
documented.

## PR process

[CONTRIBUTING.md](../CONTRIBUTING.md) states that external code contributions
are not currently accepted. There is no PR template or documented review
checklist in this repository. Maintainers making internal changes should state
the affected behavior and the exact build, test, and documentation checks they
ran; image publishing is a separate manual or tag-triggered workflow described
in [Building the Emulator](building.md#ci-github-actions).

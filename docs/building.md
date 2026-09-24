# Building the Emulator

This fork builds with Bazel 7.6.1 (`.bazelversion`). Pick the path that fits
what you need:

| Goal | Path | Warm build | Cold build |
|------|------|------------|------------|
| Change code and run tests on a Mac | [Native macOS Bazel](#native-macos-bazel) | Minutes | Many hours (about 11 h observed) |
| Linux Docker image on your machine | [`./build.sh`](#local-docker-image-buildsh) | Minutes | About 4.5 h observed (arm64, 2 jobs) |
| Publish the multi-arch image | [GitHub Actions](#ci-github-actions) | About 1.5 to 3 h observed | Up to about 6 h with the macOS archive |

"Warm" means the Bazel caches for that path already hold compiled output.
None of the paths can use another path's compiled output.
[How caching works](#how-caching-works) explains why.

## How caching works

Almost all build time is compiling C++: GoogleSQL, gRPC, protobuf, ICU,
PostgreSQL (`third_party/spanner_pg`), and the emulator. Only a cache that holds
compiled Bazel outputs saves that time. Each layer below holds something
different:

| Cache | Holds | Lives in | Saves compile time? |
|-------|-------|----------|---------------------|
| Bazel repository cache | Downloaded dependency archives | `--repository_cache` directory, `bazel-distdir/`, or a BuildKit cache mount | No, only downloads |
| Bazel disk cache | Compiled outputs of each action | `--disk_cache` directory or a BuildKit cache mount | Yes |
| Bazel output base | Incremental state from the last build | Bazel's output base or a BuildKit cache mount | Yes |
| Base image `jaysen2apache/spanner-emulator-base:<arch>` | Ubuntu 22.04, GCC 13, JDK, lld | Docker Hub | No, it only skips `apt-get` |
| BuildKit cache mounts | The three Bazel caches above, inside Docker builds | The buildx builder's storage | Yes, but only on that builder |
| Docker Hub layer cache `jaysen2apache/spanner-emulator-extended:buildcache-<arch>` | Docker layers | Docker Hub | No, it never includes cache mounts |
| GitHub Actions cache | Bazel repository and disk cache archives, capped at 2 GiB each | GitHub | Partly; the cap leaves most outputs uncached |

Bazel reuses a compiled output only when every input of the action matches:
source, flags, compiler, and environment. That's why:

- A macOS build (Apple clang) and a Linux build (GCC 13) never share outputs.
- A new buildx builder, or one whose cache was garbage-collected, starts cold,
  even though the base image and the Docker Hub layer cache are available.
- In native builds, a different `PATH`, `CC`, `CXX`, or `PROTOC` causes cache
  misses. Use the same environment every time.

## Native macOS Bazel

Best for day-to-day work: it has the only cache that survives between sessions
without extra setup. It mirrors the macOS job in CI.

Prerequisites: Xcode Command Line Tools and Homebrew packages `bazelisk`,
`gnu-sed`, and `protobuf`.

With only the Command Line Tools installed (no Xcode.app), Bazel can't detect
the macOS SDK and fails with `SDK "macosx10.11" cannot be located`. Pin the
installed SDK in `~/.bazelrc`, and update the pin after a CLT or macOS update:

```shell
echo "build --macos_sdk_version=$(xcrun --sdk macosx --show-sdk-version)" >> ~/.bazelrc
```

Build and test with a fixed environment and a persistent disk cache:

```shell
PATH=/opt/homebrew/opt/gnu-sed/libexec/gnubin:/opt/homebrew/bin:/usr/bin:/bin:/usr/sbin:/sbin \
PROTOC=/opt/homebrew/opt/protobuf/bin/protoc CC=/usr/bin/clang CXX=/usr/bin/clang++ \
bazel --host_jvm_args=-Xmx6g test -c opt --strip=always \
  --macos_sdk_version=$(xcrun --sdk macosx --show-sdk-version) \
  --macos_minimum_os=12.0 --host_macos_minimum_os=12.0 \
  --spawn_strategy=standalone --jobs=8 '--local_resources=memory=HOST_RAM*.70' \
  --disk_cache=$HOME/.cache/bazel-disk/cloud-spanner-emulator \
  --test_output=errors \
  //frontend/common:status_test
```

Use `build` instead of `test` for `//binaries:emulator_main` and
`//binaries:gateway_main`. Binaries land in `bazel-bin/binaries/`; run
`gateway_main --grpc_binary=<path to emulator_main>` to start both servers.

Tips:

- Conformance tests are one sharded target:
  `//tests/conformance/endpoints:emulator_conformance_test`. Narrow it with
  `--test_filter='PGFunctionsTest.*'`.
- `bazel query 'rdeps(//..., X)'` fails while fetching `libpfm`. Scope it
  instead: `bazel query --keep_going 'rdeps(//backend/... + //frontend/... + //tests/..., X)'`.

## Local Docker image (`build.sh`)

`./build.sh` builds `build/docker/Dockerfile.ubuntu` for one Linux
architecture and loads it as `spanner-emulator-extended:local`. It also copies
the binaries to `artifacts/spanner-emulator-main-<arch>` and
`artifacts/gateway-main-<arch>`.

```shell
./build.sh                         # linux/arm64, offline repository cache (default)
./build.sh --online                # download dependencies and import the Docker Hub layer cache
./build.sh --platform=amd64        # linux/amd64
BAZEL_JOBS=3 ./build.sh --online   # override the Bazel job count
```

### What it does

1. **Builder.** Uses the buildx builder `spanner-emulator-local`, creating it
   with `build/docker/buildkitd.toml` if it doesn't exist. That builder's
   storage holds the Bazel caches, so keep the builder. Removing it, or
   `docker buildx prune` on it, makes the next build cold.
2. **Base image.** Uses `jaysen2apache/spanner-emulator-base:<arch>` from
   Docker Hub. If that tag doesn't exist (or with `--rebuild-base-image`), it
   builds `build/docker/Dockerfile.base` and pushes it, which needs
   `docker login`. If that fails, it falls back to `ubuntu:22.04` and installs
   the toolchain during the build.
3. **Dependency manifest.** `build/docker/generate_build_manifest.py` writes
   `build_files.tar` with the BUILD files the `deps` stage needs. The dependency
   fetch layer then changes only when the dependency graph changes.
4. **Downloads.** Offline mode (the default) downloads the Bazel binary and
   runs `bazel fetch` on the host into `bazel-distdir/`, then builds from that.
   This needs Bazel on the host. `--online` skips this, downloads inside the
   build, and imports the Docker Hub layer cache.
5. **Compile.** The `build` stage runs `bazel test` for
   `//binaries:emulator_main`, `//binaries:gateway_main`, and
   `//frontend/collections:database_manager_test`. The Bazel output base,
   disk cache, and repository cache are BuildKit cache mounts named
   `spanner-emulator-<arch>-output`, `-disk`, and `-repo`.
6. **Package.** The runtime image is `gcr.io/distroless/cc-debian12` with the
   two binaries.

### Things to know

- **The base image must be in a registry.** A docker-container buildx builder
  doesn't see images in the local `docker images` store. An image that exists
  only locally can't be used as the base; push it or let `build.sh` push it.
- **The first build on a builder is cold.** Neither the base image nor the
  Docker Hub layer cache holds compiled code, so the first build compiles
  everything, GoogleSQL included. Later builds recompile only what changed.
- **Job count.** `build.sh` picks jobs from the Docker VM's memory: under
  20 GiB 1, under 32 GiB 2, under 44 GiB 3, otherwise 4. Heavy GoogleSQL files
  can use several GB each, so more jobs risks running out of memory. A 32 GiB
  colima VM reports about 31 GiB and gets 2 jobs. Set `BAZEL_JOBS` to override,
  or give the VM more memory.
- **Disk space.** A full arm64 build leaves about 11 GB in cache mounts (output
  base 7.6 GB, disk cache 2.6 GB, repository cache 1 GB) and about 17 GB on the
  builder overall. `buildkitd.toml` keeps cache mounts for up to 60 days.
  Garbage collection starts when free disk drops below 15 GB or cache mounts
  pass 70% of the disk, but it never shrinks the builder's cache below 30% of
  the disk (about 30 GB on a 100 GB colima disk). So one architecture's cache
  is safe; a second architecture or many old layers can push past that. Check
  with `colima ssh -- df -h /var/lib/docker` and
  `docker buildx du --builder spanner-emulator-local`.
- **Toolchain changes.** `spanner-toolchain-cache-guard`
  (`build/bazel/toolchain_cache_guard.sh`) fingerprints the installed packages,
  the Bazel binary, and the Bazel settings. It clears the cache mounts when they
  change, so outputs from an old compiler are never reused. When you change the
  Ubuntu base, GCC, or Bazel, also change `DEFAULT_TOOLCHAIN_CACHE_EPOCH` in
  `build.sh`, or set `SPANNER_TOOLCHAIN_CACHE_EPOCH`, to use fresh cache mount
  names.
- **Don't push a local cache to Docker Hub.** A local build's value is in its
  cache mounts, and `--cache-to` never exports those. The layers it can export
  wouldn't match CI's compile step: CI passes
  `BAZEL_DISK_CACHE_MAX_BYTES=2147483648` (local builds pass 0), and a local
  build context contains files a CI checkout doesn't, such as `bazel-distdir/`
  and uncommitted changes. Exporting to `buildcache-<arch>` would also replace
  the cache CI writes there. Sharing compiled output between local builds and
  CI would need a Bazel remote cache server, which this repo doesn't use.
- `docker build . -f build/docker/Dockerfile.ubuntu` also works, but it runs on
  the default builder with a 1-job default and none of the above set up.

### Environment variables

| Variable | Default | Effect |
|----------|---------|--------|
| `SPANNER_PLATFORM` | `arm64` | Target architecture (`amd64` or `arm64`) |
| `SPANNER_OFFLINE_DIR` | `bazel-distdir` | Host repository cache for offline mode |
| `SPANNER_BASE_IMAGE` | `jaysen2apache/spanner-emulator-base:<arch>` | Base image |
| `SPANNER_BASE_IMAGE_REPO` | `jaysen2apache/spanner-emulator-base` | Repository for the default base image |
| `SPANNER_BUILDER` | `spanner-emulator-local` | buildx builder that holds the caches |
| `SPANNER_REGISTRY_CACHE` | `jaysen2apache/spanner-emulator-extended:buildcache-<arch>` | Layer cache imported with `--online`; empty disables it |
| `SPANNER_CACHE_TO_REF` | unset | Registry ref to export the layer cache to (same as `--cache-to=`) |
| `SPANNER_TOOLCHAIN_CACHE_EPOCH` | `ubuntu22-gcc13-bazel7.6.1` | Selects the cache mount names |
| `BAZEL_JOBS` | from Docker memory | Bazel `--jobs` |

### Checking a slow build

```shell
docker buildx ls                                              # does spanner-emulator-local exist, and since when?
docker buildx du --builder spanner-emulator-local --verbose   # cache mounts and their "Created at" times
docker exec buildx_buildkit_spanner-emulator-local0 ps -eo etime,args | grep cc1plus   # what is compiling now
colima ssh -- df -h /var/lib/docker                           # free disk space
```

Cache mounts created at the start of the build mean it started cold. Files
under `external/googlesql~` in the compile list mean it's rebuilding
dependencies, not just your change.

## CI (GitHub Actions)

`.github/workflows/docker-publish.yml` builds, and publishes to
`jaysen2apache/spanner-emulator-extended`. It never runs on an ordinary push.

| Trigger | Builds | Publishes |
|---------|--------|-----------|
| `workflow_dispatch` on `jay-spanner-extended` | Linux images, plus the macOS archive when `target=arm` | Full commit SHA, 7-character SHA and `latest` |
| Same, with `candidate_only=true` | Same | Full commit SHA only |
| `workflow_dispatch` on any other branch | Same | Nothing |
| Version tag `x.y.z` (no `v` prefix) | Linux images and the macOS archive | Full SHA, 7-character SHA, `latest` and `x.y.z` |
| Schedule (Mon and Thu, 06:00 UTC, on `master`) | Linux images | Nothing; keeps caches warm |

Every published tag is one multi-arch manifest (`linux/amd64` and
`linux/arm64`). The macOS archive is a workflow artifact, not an image.

```shell
git push origin jay-spanner-extended
gh workflow run docker-publish.yml --ref jay-spanner-extended -f target=linux
```

Push first: the workflow file and source come from the pushed branch.
`target=linux` skips the macOS archive, which is the slowest job.

### Jobs

- **`build`** (amd64 on `ubuntu-24.04`, arm64 on `ubuntu-24.04-arm`, 2 Bazel
  jobs each). Resolves the base image, falling back to `ubuntu:22.04` when the
  tag is missing (only `arm64` is published today). It then restores the Bazel
  caches from the GitHub Actions cache into BuildKit cache mounts and builds
  with the Docker Hub layer cache. When Docker Hub credentials are configured,
  every run exports the layer cache. On a publish it also pushes the image by
  digest.
- **macOS job.** Builds the native arm64 archive
  (`spanner-emulator-macos-arm64-<sha>` workflow artifact). It restores only the
  repository cache, so it compiles everything on every run.
- **`merge`.** Combines the two digests into one multi-arch tag.

### Cache keys

- Repository cache: `bazel-repo-v2-<os>-<arch>-<sha256 of build_files.tar>`,
  restored by prefix. It changes when BUILD files or dependencies change.
- Disk cache: `bazel-disk-v2-<os>-<arch>-<toolchain image id>-<commit>`,
  restored by prefix within the same toolchain. A new base image or toolchain
  starts it over.
- The build trims the disk cache to 2 GiB, and a cache is saved only if it's
  2 GiB or smaller (5 GiB compressed for both together). On restore, archives
  up to 5 GiB are accepted. A full build's outputs don't fit in 2 GiB, so every
  run recompiles part of the build.
- GitHub scopes caches to a branch plus the default branch. The scheduled run
  on `master` keeps a shared copy warm. A newly renamed or created branch
  starts from `master`'s caches.

### Releasing

```shell
# A candidate image for testing, without moving `latest`
gh workflow run docker-publish.yml --ref jay-spanner-extended -f target=linux -f candidate_only=true

# Publish `latest` from the working branch
gh workflow run docker-publish.yml --ref jay-spanner-extended -f target=linux

# A versioned release (also builds the macOS archive)
git tag -a x.y.z -m "Release x.y.z"
git push origin x.y.z
```

Consumers such as LocalCloud should pin the full-SHA tag with its digest
(`jaysen2apache/spanner-emulator-extended:<sha>@sha256:<digest>`), because
`latest` moves.

## What triggers a large rebuild

| Change | Effect |
|--------|--------|
| Emulator source (`backend/`, `frontend/`, `common/`) | Only affected targets recompile |
| `third_party/spanner_pg` parser headers (`parsenodes.h`, `gram.y`, `kwlist.h`) | A large part of `spanner_pg` recompiles |
| `MODULE.bazel`, its lock file, or a patch under `build/bazel/` | Changed dependencies and everything that uses them |
| Base image, GCC, or Bazel version | Everything; the cache guard clears the caches |
| New or pruned buildx builder, or cache mounts garbage-collected | Everything, for local Docker builds |
| Switching between macOS and Linux | Nothing is shared between the two |

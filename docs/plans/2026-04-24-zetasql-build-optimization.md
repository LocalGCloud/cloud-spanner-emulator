# ZetaSQL Build Optimization Implementation Plan

> Historical plan from 2026-04-24. The paths, Bazel version, and runnable
> commands below describe that checkout. Use the [current build guide](../building.md)
> for today's commands.

> **For Claude:** REQUIRED SUB-SKILL: Use superpowers:executing-plans to implement this plan task-by-task.

**Goal:** Reduce Docker/CI build time from 30-60 min to <15 min for code-only changes.

**Architecture:** Three-pronged approach — (1) Docker layer splitting separates dep fetching from source compilation so ZetaSQL only rebuilds when WORKSPACE changes, (2) blanket `-O1` on all external C++ deps cuts cold build compile time 30-50%, (3) dedicated Bazel disk cache mount preserves compiled action outputs across Docker builds.

**Tech Stack:** Docker BuildKit, Bazel 6.5.0, GCC 13

**Design doc:** `docs/plans/2026-04-24-zetasql-build-optimization-design.md`

---

### Task 1: Expand per_file_copt rules in .bazelrc

Quick win, independent of Dockerfile changes. Reduces optimization level for all external C++ dependencies.

**Files:**
- Modify: `.bazelrc:22-29`

**Step 1: Replace the existing per_file_copt block**

Current `.bazelrc` lines 22-29 target only 4 specific ZetaSQL files. Replace with blanket rules covering all external deps.

Replace lines 22-29 in `.bazelrc` with:

```
# Reduce optimization on ALL external C++ deps.
# ZetaSQL, ICU, gRPC, protobuf compile 2-4x faster at -O1 vs -O3.
# This is a dev emulator — runtime perf of these deps is irrelevant.
# The original 4-file rules are subsumed by the blanket ZetaSQL rule.
build --per_file_copt=+external/com_google_zetasql/.*\.cc@-O1
build --per_file_copt=+external/com_google_icu/.*\.c@-O1
build --per_file_copt=+external/com_google_icu/.*\.cc@-O1
build --per_file_copt=+external/com_github_grpc_grpc/.*\.cc@-O1
build --per_file_copt=+external/com_google_protobuf/.*\.cc@-O1

# Guard against concurrent cache corruption
build --experimental_guard_against_concurrent_changes
```

**Step 2: Verify the final .bazelrc content**

The complete file should be:

```
build --cxxopt=-std=c++17
build --host_cxxopt=-std=c++17

# gRPC has some issues with building under bazel on MacOS with thread-locals.
# See https://github.com/bazelbuild/bazel/issues/4341 for more details. This
# flag enabled gRPC workarounds for building under bazel.
build --copt -DGRPC_BAZEL_BUILD

# Disable debug symbols for release builds.
build --copt -DNDEBUG

# Required for python test runner to find gcloud binary.
test --test_env GCLOUD_DIR

# Most of these warnings are triggered from ZetaSQL, disable for now. Ideally,
# we would disable these only for projects we don't control, but there does not
# seem to be an easy way to do that yet.
build --copt   -Wno-sign-compare
build --copt   -Wno-deprecated-declarations
build --copt   -Wno-return-type
build --copt   -Wno-unused-but-set-parameter

# Reduce optimization on ALL external C++ deps.
# ZetaSQL, ICU, gRPC, protobuf compile 2-4x faster at -O1 vs -O3.
# This is a dev emulator — runtime perf of these deps is irrelevant.
# The original 4-file rules are subsumed by the blanket ZetaSQL rule.
build --per_file_copt=+external/com_google_zetasql/.*\.cc@-O1
build --per_file_copt=+external/com_google_icu/.*\.c@-O1
build --per_file_copt=+external/com_google_icu/.*\.cc@-O1
build --per_file_copt=+external/com_github_grpc_grpc/.*\.cc@-O1
build --per_file_copt=+external/com_google_protobuf/.*\.cc@-O1

# Guard against concurrent cache corruption
build --experimental_guard_against_concurrent_changes
```

**Step 3: Commit**

```bash
git add .bazelrc
git commit -m "perf: reduce optimization to -O1 for all external C++ deps

ZetaSQL, ICU, gRPC, and protobuf compile 2-4x faster at -O1 vs -O3.
This is a dev emulator so runtime perf of these deps is irrelevant.
Replaces the previous 4-file targeted rules with blanket coverage."
```

---

### Task 2: Rewrite Dockerfile with layer splitting and disk cache

The core change. Splits the single `build` stage into `toolchain` → `deps` → `build` stages so Docker caches dependency work separately from source compilation. Also adds a dedicated disk cache mount.

**Files:**
- Modify: `build/docker/Dockerfile.ubuntu`

**Step 1: Rewrite the Dockerfile**

Replace the entire contents of `build/docker/Dockerfile.ubuntu` with:

```dockerfile
# syntax=docker/dockerfile:1
#
# Copyright 2020 Google LLC
#
# Licensed under the Apache License, Version 2.0 (the "License");
# you may not use this file except in compliance with the License.
# You may obtain a copy of the License at
#
#     http://www.apache.org/licenses/LICENSE-2.0
#
# Unless required by applicable law or agreed to in writing, software
# distributed under the License is distributed on an "AS IS" BASIS,
# WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
# See the License for the specific language governing permissions and
# limitations under the License.
#

################################################################################
#                               TOOLCHAIN                                      #
################################################################################
#
# Supports two modes:
#   Online (default):  docker build -f build/docker/Dockerfile.ubuntu .
#   Offline (distdir): docker build -f build/docker/Dockerfile.ubuntu \
#                        --build-arg OFFLINE_DIR=bazel-distdir .
#
# Build is split into three stages for optimal Docker layer caching:
#   1. toolchain: OS packages, GCC 13, Bazel binary (rarely changes)
#   2. deps:      Fetch external repos — ZetaSQL, protobuf, ICU, etc.
#                  (only re-runs when WORKSPACE or build/bazel/ patches change)
#   3. build:     Compile emulator source code
#                  (re-runs on any source file change, but deps are cached)

FROM ubuntu:22.04 AS toolchain

ARG TARGETARCH
ENV DEBIAN_FRONTEND=noninteractive
ENV GCC_VERSION=13

# Install all build dependencies in a single layer.
# - build-essential, curl, wget, tar, zip, unzip: bazel prerequisites
# - python3: build scripts (python2 removed — EOL)
# - default-jre, default-jdk: java toolchain for bazel
# - software-properties-common, make, rename, git: build tools
# - language-pack-en: en* locales for PG scalar functions
# - tzdata: timezone data
RUN apt-get update                                                          && \
    apt-get -qq install -y --no-install-recommends                             \
      curl tar build-essential wget python3 zip unzip                          \
      default-jre default-jdk ca-certificates-java                             \
      software-properties-common make rename git                               \
      language-pack-en tzdata ca-certificates gnupg dirmngr                 && \
    rm -rf /var/lib/apt/lists/*

# Unfortunately ZetaSQL has issues with clang (default bazel compiler), so
# we install GCC 13 via the toolchain PPA.
# We add the PPA source directly and use the signed-by approach with the key
# fetched over HKP (port 11371) to avoid SSL issues inside Docker builds.
RUN mkdir -p /etc/apt/keyrings                                              && \
    gpg --keyserver hkp://keyserver.ubuntu.com:80                              \
        --recv-keys 1E9377A2BA9EF27F                                        && \
    gpg --export 1E9377A2BA9EF27F                                              \
        > /etc/apt/keyrings/ubuntu-toolchain.gpg                            && \
    echo "deb [signed-by=/etc/apt/keyrings/ubuntu-toolchain.gpg] http://ppa.launchpadcontent.net/ubuntu-toolchain-r/test/ubuntu jammy main" \
      > /etc/apt/sources.list.d/ubuntu-toolchain-r-test.list                && \
    apt-get -qq update                                                      && \
    apt-get -qq install -y --no-install-recommends                             \
      gcc-${GCC_VERSION} g++-${GCC_VERSION}                                && \
    update-alternatives --install /usr/bin/gcc gcc /usr/bin/gcc-${GCC_VERSION} 90  \
                        --slave   /usr/bin/g++ g++ /usr/bin/g++-${GCC_VERSION} && \
    update-alternatives --set gcc /usr/bin/gcc-${GCC_VERSION}                && \
    rm -rf /var/lib/apt/lists/*

ENV PATH=$PATH:/usr/bin:/usr/local/bin
ENV EXTRA_BAZEL_ARGS="--tool_java_runtime_version=local_jdk"
ENV BAZEL_CXXOPTS="-std=c++17"

# Install Bazel directly (not via bazelisk) to avoid x509 TLS errors in
# restricted network environments — Go binaries like bazelisk use their own
# cert verification which can fail behind corporate proxies.
# In offline mode, the binary is picked from the pre-downloaded distdir.
ARG BAZEL_VERSION=6.5.0
ARG OFFLINE_DIR=
RUN BAZEL_BIN="bazel-${BAZEL_VERSION}-linux-${TARGETARCH}" && \
    if [ -n "$OFFLINE_DIR" ] && [ -f "src/${OFFLINE_DIR}/${BAZEL_BIN}" ]; then \
      cp "src/${OFFLINE_DIR}/${BAZEL_BIN}" /usr/local/bin/bazel; \
    else \
      wget -q --no-check-certificate \
        "https://releases.bazel.build/${BAZEL_VERSION}/release/${BAZEL_BIN}" \
        -O /usr/local/bin/bazel; \
    fi && \
    chmod +x /usr/local/bin/bazel

################################################################################
#                                DEPS                                          #
################################################################################
# Fetch all external dependencies (ZetaSQL, protobuf, ICU, gRPC, etc.).
# This layer is cached by Docker as long as WORKSPACE and build/bazel/
# patches don't change — which is rare compared to emulator source changes.

FROM toolchain AS deps

# Copy ONLY files that define external dependencies — NOT the full source tree.
# This is the key optimization: source code changes won't invalidate this layer.
COPY WORKSPACE .bazelrc BUILD.bazel maven_install.json /src/
COPY build/bazel/ /src/build/bazel/

# Fetch all external repos. Uses both the Bazel cache mount (persists across
# Docker builds on the same daemon) and the disk cache mount (persists compiled
# action outputs separately from Bazel's internal state).
ARG OFFLINE_DIR=
RUN --mount=type=cache,target=/root/.cache/bazel,id=bazel-cache \
    --mount=type=cache,target=/tmp/bazel-disk-cache,id=bazel-disk-cache \
    cd /src && \
    REPO_CACHE_FLAG="" && \
    if [ -n "$OFFLINE_DIR" ] && [ -d "$OFFLINE_DIR" ]; then \
      REPO_CACHE_FLAG="--repository_cache=/src/${OFFLINE_DIR}"; \
    fi && \
    CC=/usr/bin/gcc CXX=/usr/bin/g++ \
    bazel fetch ${REPO_CACHE_FLAG} \
      @com_google_zetasql//... \
      @com_google_absl//... \
      @com_github_grpc_grpc//... \
      @com_google_protobuf//...

################################################################################
#                                BUILD                                         #
################################################################################
# Compile emulator source code. External deps are already fetched and their
# compiled outputs are in the disk cache mount from previous builds.

FROM deps AS build

# Now copy the full source tree. This invalidates Docker layer cache on any
# code change, but the Bazel disk cache mount preserves compiled action outputs
# for all external deps (ZetaSQL, protobuf, etc.) — only emulator code
# recompiles.
COPY . /src/

# Build the emulator.
# Excluding all targets from //third_party/spanner_pg/src/... from building as
# not all the targets from the PostgreSQL source needs to be built and some of
# the unused build targets will fail.
#
# OFFLINE_DIR: when set (e.g. "bazel-distdir"), Bazel uses pre-downloaded deps
# via --repository_cache (sha256-addressed, includes transitive deps) instead
# of fetching from the network.
# BAZEL_JOBS / BAZEL_RAM: tune parallelism for constrained environments.
ARG OFFLINE_DIR=
ARG BAZEL_JOBS=auto
ARG BAZEL_RAM=HOST_RAM*.67
RUN --mount=type=cache,target=/root/.cache/bazel,id=bazel-cache \
    --mount=type=cache,target=/tmp/bazel-disk-cache,id=bazel-disk-cache \
    cd /src && \
    REPO_CACHE_FLAG="" && \
    if [ -n "$OFFLINE_DIR" ] && [ -d "$OFFLINE_DIR" ]; then \
      REPO_CACHE_FLAG="--repository_cache=/src/${OFFLINE_DIR}"; \
    fi && \
    CC=/usr/bin/gcc CXX=/usr/bin/g++ \
    bazel build -c opt --spawn_strategy=standalone \
      --jobs=${BAZEL_JOBS} --local_ram_resources=${BAZEL_RAM} \
      --disk_cache=/tmp/bazel-disk-cache \
      ${REPO_CACHE_FLAG} \
      -- ... -third_party/spanner_pg/src/... && \
    mkdir -p /build/output && \
    cp -L bazel-bin/binaries/emulator_main /build/output/ && \
    cp -L bazel-bin/binaries/gateway_main_/gateway_main /build/output/ && \
    strip /build/output/emulator_main && \
    strip /build/output/gateway_main

# Generate licenses file.
RUN for file in $(find -L /src/bazel-src/external                              \
                       -name "LICENSE" -o -name "COPYING")                   ; \
    do                                                                         \
      echo "----"                                                            ; \
      echo $file                                                             ; \
      echo "----"                                                            ; \
      cat $file                                                              ; \
    done > licenses.txt                                                     && \
    gzip licenses.txt

################################################################################
#                                   RELEASE                                    #
################################################################################

# Now build the release image from the build image.
FROM gcr.io/distroless/cc-debian12

# Copy binaries (persisted from cache mount during build).
COPY --from=build /build/output/emulator_main .
COPY --from=build /build/output/gateway_main .

# Copy the libstdc++.so.6 shared library. We use wildcards to execute a conditional
# copy depending on the target architecture.
COPY --from=build /usr/lib/x86_64-linux-gnu*/libstdc++.so.6 /usr/lib/x86_64-linux-gnu
COPY --from=build /usr/lib/aarch64-linux-gnu*/libstdc++.so.6 /usr/lib/aarch64-linux-gnu

# Copy licenses
COPY --from=build /licenses.txt.gz .

# Expose the default ports 9010 (gRPC) and 9020 (REST)
EXPOSE 9010 9020

# Run the gateway process, bind to 0.0.0.0 as under MacOS, listening on
# localhost will make the server invisible outside the container.
CMD ["./gateway_main", "--hostname", "0.0.0.0"]
```

**Step 2: Verify the key differences from the original**

Confirm these changes:
1. Three stages: `toolchain` → `deps` → `build` (was single `build`)
2. `COPY . src/` moved from before Bazel install to the `build` stage only
3. `deps` stage copies only: `WORKSPACE`, `.bazelrc`, `BUILD.bazel`, `maven_install.json`, `build/bazel/`
4. `deps` stage runs `bazel fetch` on external repos (not `bazel build`)
5. Both `deps` and `build` stages have two cache mounts: `bazel-cache` + `bazel-disk-cache`
6. `build` stage passes `--disk_cache=/tmp/bazel-disk-cache` to bazel build
7. `BAZEL_CXXOPTS` env moved to `toolchain` stage (available to all downstream stages)
8. License generation uses `/src/bazel-src/external` (absolute path since workdir may differ)
9. `OFFLINE_DIR` ARG re-declared in each stage that needs it (Docker multi-stage requirement)

**Step 3: Commit**

```bash
git add build/docker/Dockerfile.ubuntu
git commit -m "perf: split Dockerfile into toolchain/deps/build stages

Separates dependency fetching from source compilation so Docker
caches ZetaSQL and other external deps independently of emulator
source changes. Adds dedicated Bazel disk cache mount for action
output reuse across builds.

Three stages:
- toolchain: OS packages, GCC 13, Bazel (rarely changes)
- deps: fetch external repos (changes only with WORKSPACE/patches)
- build: compile emulator (re-runs on source changes, cache-warm)"
```

---

### Task 3: Test online build

Verify the refactored Dockerfile builds correctly in online mode.

**Files:** None (testing only)

**Step 1: Run a full online build**

```bash
cd /Users/jsenjaliya/src/my/local_cloud_dependencies/cloud-spanner-emulator
./build.sh
```

Expected: Build completes. `artifacts/spanner-emulator-main` exists and is a valid Linux binary.

**Step 2: Make a trivial source change and rebuild**

```bash
# Touch a source file to simulate a code change
touch backend/common/ids.h
./build.sh
```

Expected:
- Docker reuses `toolchain` and `deps` layers (look for `CACHED` in build output)
- Only `build` stage re-runs
- Build is significantly faster than first run

**Step 3: Verify binaries work**

```bash
docker run --rm -p 9010:9010 -p 9020:9020 spanner-emulator-build &
sleep 5
curl -s http://localhost:9020/v1/projects/test-project/instances | head -5
docker stop $(docker ps -q --filter ancestor=spanner-emulator-build)
```

Expected: Emulator responds to REST API.

---

### Task 4: Test offline build

Verify offline mode still works with the new layer structure.

**Files:** None (testing only)

**Step 1: Run offline build**

```bash
cd /Users/jsenjaliya/src/my/local_cloud_dependencies/cloud-spanner-emulator
./build.sh --offline-dir=bazel-distdir
```

Expected: Build completes using pre-cached deps from `bazel-distdir/`. No network fetches inside Docker.

**Step 2: Verify the offline binary is identical**

```bash
# Compare binary sizes (should be same as online build)
ls -la artifacts/spanner-emulator-main
file artifacts/spanner-emulator-main
```

Expected: Valid Linux ELF binary, stripped, similar size to online build.

---

### Task 5: Verify -O1 doesn't cause regressions

Run whatever test suite is available to confirm no behavioral changes from reduced optimization.

**Files:** None (testing only)

**Step 1: Check available tests**

```bash
# See what test targets exist
cd /Users/jsenjaliya/src/my/local_cloud_dependencies/cloud-spanner-emulator
grep -r "test_suite\|cc_test\|go_test" BUILD.bazel tests/conformance/cases/BUILD | head -20
```

**Step 2: Run emulator conformance tests (if feasible inside Docker)**

```bash
# If tests can run against the built Docker image:
docker run --rm -d -p 9010:9010 -p 9020:9020 --name emulator-test spanner-emulator-build
# Run conformance tests against localhost:9010/9020
# (exact command depends on test harness setup)
docker stop emulator-test
```

**Step 3: Commit all changes if tests pass**

If Task 1 and Task 2 weren't already committed:

```bash
git add .bazelrc build/docker/Dockerfile.ubuntu
git commit -m "perf: optimize ZetaSQL build — layer splitting + reduced optimization

Three optimizations combined:
1. Dockerfile split into toolchain/deps/build stages for Docker layer caching
2. All external C++ deps compiled at -O1 instead of -O3
3. Dedicated Bazel disk cache mount for action output reuse

Expected impact:
- Code-only changes: 70-80% faster (deps layer cached)
- Cold builds: 30-50% faster (from -O1 compilation)
- Warm rebuilds: 80-90% faster (disk cache hit)"
```

---

## Implementation Notes

**Docker layer caching behavior:**
- `toolchain` stage: Cached until base image or apt packages change (~never)
- `deps` stage: Cached until `WORKSPACE`, `.bazelrc`, `BUILD.bazel`, `maven_install.json`, or `build/bazel/*` change (~rare, only on dependency bumps)
- `build` stage: Re-runs on any source file change (~every build) but Bazel disk cache mount provides compiled action outputs for all external deps

**Bazel cache mount behavior:**
- `bazel-cache` mount at `/root/.cache/bazel`: Bazel's output base, analysis cache, repo cache. Persists across Docker builds on same daemon.
- `bazel-disk-cache` mount at `/tmp/bazel-disk-cache`: Content-addressed action outputs. Survives even when Bazel output base changes (e.g., different workspace hash).

**Offline mode compatibility:**
- `build.sh` unchanged — still runs `bazel fetch` on host to populate repo cache
- `OFFLINE_DIR` ARG re-declared in `deps` and `build` stages
- `--repository_cache` flag constructed identically in both stages

**Why `bazel fetch` instead of `bazel build` in deps stage:**
- `bazel fetch @external//...` downloads and unpacks external archives
- `bazel build @external//...` would compile them too, but risks failures from missing local context
- The disk cache mount handles compilation caching more reliably
- `fetch` is idempotent and fast when repos are already cached

**Rollback:** If any issue, revert to prior Dockerfile with `git checkout HEAD~1 -- build/docker/Dockerfile.ubuntu .bazelrc`

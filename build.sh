#!/bin/bash
# Build the Spanner emulator using build/docker/Dockerfile.ubuntu
#
# Usage:
#   ./build.sh                                    # arm64, offline (bazel-distdir), base image (default)
#   ./build.sh --online                           # online mode (skips local bazel-distdir)
#   ./build.sh --platform=amd64                   # amd64 (x86_64) platform
#   ./build.sh --offline-dir=custom-dir           # custom offline directory
#   ./build.sh --base-image=ubuntu:22.04          # specify custom base image
#   ./build.sh --base-image-repo=user/repo        # custom Docker Hub repo for the base image
#   ./build.sh --rebuild-base-image               # force rebuild+push even if the registry tag exists
#   ./build.sh --cache-from=myregistry/repo:tag   # import BuildKit cache from registry
#   ./build.sh --cache-to=myregistry/repo:tag     # export BuildKit cache to registry
#   ./build.sh --no-registry-cache                # disable importing BuildKit registry cache
#
# By default, ./build.sh runs in offline mode using bazel-distdir, builds for
# linux/arm64 (Apple Silicon native), and uses a base image pulled from
# Docker Hub (jaysen2apache/spanner-emulator-base:<arch>). The base image is
# a real registry ref (not a local-only tag) so it resolves both from the
# docker-container buildx builder used here and from GitHub Actions.
set -e

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
cd "$SCRIPT_DIR"
SOURCE_REVISION="${SOURCE_REVISION:-$(git rev-parse HEAD 2>/dev/null || echo unknown)}"

# ── Parse arguments ──────────────────────────────────────────────────────────
PLATFORM="${SPANNER_PLATFORM:-arm64}"
OFFLINE_DIR="${SPANNER_OFFLINE_DIR:-bazel-distdir}"
BASE_IMAGE_REPO="${SPANNER_BASE_IMAGE_REPO:-jaysen2apache/spanner-emulator-base}"
BASE_IMAGE="${SPANNER_BASE_IMAGE:-}"
REBUILD_BASE_IMAGE=0
CACHE_TO="${SPANNER_CACHE_TO_REF:-}"
REGISTRY_CACHE="${SPANNER_REGISTRY_CACHE-__DEFAULT__}"
LOCAL_CACHE_DIR="${SPANNER_LOCAL_CACHE_DIR:-}"
FORCE_FETCH="${SPANNER_FORCE_FETCH:-0}"
SKIP_FETCH="${SPANNER_SKIP_FETCH:-0}"
RUN_TESTS="${SPANNER_RUN_TESTS:-0}"
BAZEL_JOBS_CLI=""

for arg in "$@"; do
  case "$arg" in
    --platform=*)          PLATFORM="${arg#*=}" ;;
    --offline-dir=*)       OFFLINE_DIR="${arg#*=}" ;;
    --online|--no-offline) OFFLINE_DIR="" ;;
    --base-image=*)        BASE_IMAGE="${arg#*=}" ;;
    --base-image-repo=*)   BASE_IMAGE_REPO="${arg#*=}" ;;
    --rebuild-base-image)  REBUILD_BASE_IMAGE=1 ;;
    --cache-to=*)          CACHE_TO="${arg#*=}" ;;
    --cache-from=*)        REGISTRY_CACHE="${arg#*=}" ;;
    --registry-cache=*)    REGISTRY_CACHE="${arg#*=}" ;;
    --no-registry-cache)   REGISTRY_CACHE="" ;;
    --local-cache-dir=*)   LOCAL_CACHE_DIR="${arg#*=}" ;;
    --force-fetch)         FORCE_FETCH=1 ;;
    --skip-fetch)          SKIP_FETCH=1 ;;
    --jobs=*)              BAZEL_JOBS_CLI="${arg#*=}" ;;
    --run-tests)           RUN_TESTS=1 ;;
    --no-tests|--skip-tests) RUN_TESTS=0 ;;
  esac
done
case "$PLATFORM" in
  amd64) BAZEL_ARCH="x86_64" ;;
  arm64) BAZEL_ARCH="arm64" ;;
  *)
    echo "ERROR: Unsupported platform: $PLATFORM (expected amd64 or arm64)" >&2
    exit 1
    ;;
esac
if [ -z "$BASE_IMAGE" ]; then
  BASE_IMAGE="${BASE_IMAGE_REPO}:${PLATFORM}"
fi
BUILDER_NAME="${SPANNER_BUILDER:-spanner-emulator-local}"
BUILDKIT_CONFIG="$SCRIPT_DIR/build/docker/buildkitd.toml"


# Never change the legacy value: it names caches created before epochs existed.
LEGACY_TOOLCHAIN_CACHE_EPOCH="ubuntu22-gcc13-bazel7.6.1"
# Change only this default when the Ubuntu base, GCC, or Bazel changes.
DEFAULT_TOOLCHAIN_CACHE_EPOCH="ubuntu22-gcc13-bazel7.6.1"
TOOLCHAIN_CACHE_EPOCH="${SPANNER_TOOLCHAIN_CACHE_EPOCH:-$DEFAULT_TOOLCHAIN_CACHE_EPOCH}"
case "$TOOLCHAIN_CACHE_EPOCH" in
  ''|*[!A-Za-z0-9._-]*)
    echo "ERROR: Invalid SPANNER_TOOLCHAIN_CACHE_EPOCH: $TOOLCHAIN_CACHE_EPOCH" >&2
    exit 1
    ;;
esac
if [ "$TOOLCHAIN_CACHE_EPOCH" = "$LEGACY_TOOLCHAIN_CACHE_EPOCH" ]; then
  BAZEL_CACHE_NAMESPACE="spanner-emulator-${PLATFORM}"
else
  BAZEL_CACHE_NAMESPACE="spanner-emulator-${PLATFORM}-${TOOLCHAIN_CACHE_EPOCH}"
fi
BAZEL_REPO_CACHE_NAMESPACE="spanner-emulator-${PLATFORM}"

DOCKERFILE="build/docker/Dockerfile.ubuntu"
IMAGE_TAG="spanner-emulator-extended:local"
if [ "$REGISTRY_CACHE" = "__DEFAULT__" ]; then
  REGISTRY_CACHE="jaysen2apache/spanner-emulator-extended:buildcache-${PLATFORM}"
fi

echo "============================================"
echo "  Building Spanner Emulator"
echo "  Platform:     linux/${PLATFORM}"
echo "  Base Image:   $BASE_IMAGE"
echo "  Cache:        $TOOLCHAIN_CACHE_EPOCH"
echo "  Revision:     $SOURCE_REVISION"
if [ -n "$OFFLINE_DIR" ]; then
  echo "  Mode:         offline (repo cache: $OFFLINE_DIR)"
else
  echo "  Mode:         online"
fi
if [ -n "$REGISTRY_CACHE" ]; then
  echo "  Import Cache: $REGISTRY_CACHE"
fi
if [ -n "$CACHE_TO" ]; then
  echo "  Export Cache: $CACHE_TO"
fi
echo "  Started:      $(date)"
echo "============================================"
BUILD_START=$(date +%s)

BUILD_ARGS=()
CACHE_ARGS=()

echo ""
echo "Bootstrapping Buildx builder: $BUILDER_NAME"
if docker buildx inspect "$BUILDER_NAME" >/dev/null 2>&1; then
  if ! docker buildx inspect "$BUILDER_NAME" --bootstrap; then
    echo "ERROR: Existing Buildx builder '$BUILDER_NAME' could not bootstrap." >&2
    echo "Set SPANNER_BUILDER to a different builder name and retry." >&2
    exit 1
  fi
else
  if ! docker buildx create --name "$BUILDER_NAME" --driver docker-container \
    --buildkitd-config "$BUILDKIT_CONFIG" --bootstrap; then
    echo "ERROR: Could not create Buildx builder '$BUILDER_NAME'." >&2
    echo "If that name already exists but is unusable, set SPANNER_BUILDER to a different name." >&2
    exit 1
  fi
fi
if [ "$BASE_IMAGE" = "${BASE_IMAGE_REPO}:${PLATFORM}" ]; then
  # NOTE: the docker-container buildx builder used below runs BuildKit in its
  # own isolated container and does NOT share the local `docker images` store.
  # A base image built with `--load` only lands in the host Docker engine, so
  # a later `FROM` from this builder can't see it and falls back to pulling
  # from Docker Hub by name — which fails for a local-only tag. Pushing the
  # base image to a real registry ref sidesteps that: both this builder and
  # GitHub Actions resolve it the same way any other base image is resolved.
  BASE_IMAGE_READY=0
  if [ "$REBUILD_BASE_IMAGE" -eq 0 ] && docker buildx imagetools inspect "$BASE_IMAGE" >/dev/null 2>&1; then
    BASE_IMAGE_READY=1
  fi
  if [ "$BASE_IMAGE_READY" -eq 0 ]; then
    if [ "$REBUILD_BASE_IMAGE" -eq 1 ]; then
      echo "Rebuilding base image '$BASE_IMAGE' (--rebuild-base-image)..."
    else
      echo "Base image '$BASE_IMAGE' not found on registry. Building and pushing..."
    fi
    if docker buildx build --builder "$BUILDER_NAME" --platform "linux/${PLATFORM}" --push -f build/docker/Dockerfile.base -t "$BASE_IMAGE" .; then
      echo "Base image '$BASE_IMAGE' pushed successfully."
    else
      echo "WARN: Could not build/push '$BASE_IMAGE' (check 'docker login'); falling back to ubuntu:22.04." >&2
      BASE_IMAGE="ubuntu:22.04"
    fi
  fi
fi
BUILDER_CACHE_POLICY=$(
  docker buildx inspect "$BUILDER_NAME" 2>/dev/null |
    sed -n 's/^[[:space:]]*org\.localgcloud\.spanner-cache-policy:[[:space:]]*//p'
)
if [ "$BUILDER_CACHE_POLICY" != "v1" ]; then
  echo "WARN: Builder '$BUILDER_NAME' is not using the durable Bazel cache policy." >&2
  echo "      Finish active builds, then reconfigure it with $BUILDKIT_CONFIG." >&2
fi

echo ""
echo "Creating deterministic BUILD files manifest..."
python3 build/docker/generate_build_manifest.py

# ── Offline mode: populate repository cache ──────────────────────────────────
if [ -n "$OFFLINE_DIR" ]; then
  DISTDIR="$SCRIPT_DIR/$OFFLINE_DIR"
  mkdir -p "$DISTDIR"

  echo ""
  echo "[1/3] Checking repository cache in $OFFLINE_DIR/..."

  # Pre-download the Bazel binary itself so Docker doesn't need network for it
  BAZEL_VERSION=$(cat .bazelversion | tr -d '[:space:]')
  bazel_fname="bazel-${BAZEL_VERSION}-linux-${BAZEL_ARCH}"
  bazel_path="$DISTDIR/$bazel_fname"
  if [ ! -f "$bazel_path" ] || [ "$(wc -c < "$bazel_path")" -lt 1048576 ]; then
    echo "  GET: $bazel_fname"
    rm -f "$bazel_path"
    curl -fL --max-time 600 \
      -o "$bazel_path" \
      "https://releases.bazel.build/${BAZEL_VERSION}/release/${bazel_fname}"
  fi

  # Skip host bazel fetch if repository cache is already populated
  DO_FETCH=1
  if [ "$SKIP_FETCH" = "1" ]; then
    DO_FETCH=0
  elif [ "$FORCE_FETCH" != "1" ] && [ -d "$DISTDIR/content_addressable" ] && [ -n "$(find "$DISTDIR/content_addressable" -type f 2>/dev/null | head -1)" ]; then
    DO_FETCH=0
  fi

  if [ "$DO_FETCH" -eq 1 ]; then
    if command -v bazel >/dev/null 2>&1; then
      echo "  Running bazel fetch to discover all deps..."
      bazel fetch --repository_cache="$DISTDIR" \
        //... -- -third_party/spanner_pg/src/... 2>&1 \
        | grep -E "^(INFO|WARNING)" | head -20 || true
      echo "  Repository cache populated"
    else
      echo "  WARN: bazel not found on host, skipping fetch."
      echo "  Install bazel/bazelisk to enable full offline builds."
    fi
  else
    echo "  Repository cache already populated ($OFFLINE_DIR/content_addressable); skipping host fetch."
    echo "  (Pass --force-fetch to re-evaluate and fetch dependencies on host)"
  fi

  BUILD_ARGS+=(--build-arg "OFFLINE_DIR=$OFFLINE_DIR")
  BUILD_ARGS+=(--build-arg "BAZEL_SOURCE=${OFFLINE_DIR}/${bazel_fname}")
else
  echo ""
  echo "[1/3] Skipping repo cache (online mode)..."
fi
BUILD_ARGS+=(--build-arg "BASE_IMAGE=${BASE_IMAGE}")
BUILD_ARGS+=(--build-arg "RUN_TESTS=${RUN_TESTS}")

if [ -n "$REGISTRY_CACHE" ]; then
  echo "  Importing portable BuildKit cache: $REGISTRY_CACHE"
  CACHE_ARGS+=(--cache-from "type=registry,ref=$REGISTRY_CACHE")
fi
if [ -n "$LOCAL_CACHE_DIR" ]; then
  mkdir -p "$LOCAL_CACHE_DIR"
  echo "  Using local BuildKit cache: $LOCAL_CACHE_DIR"
  CACHE_ARGS+=(--cache-from "type=local,src=$LOCAL_CACHE_DIR")
  CACHE_ARGS+=(--cache-to "type=local,dest=$LOCAL_CACHE_DIR,mode=max")
fi
if [ -n "$CACHE_TO" ]; then
  echo "  Exporting BuildKit cache to: $CACHE_TO"
  CACHE_ARGS+=(--cache-to "type=registry,ref=$CACHE_TO,mode=max")
fi

# ── Build ────────────────────────────────────────────────────────────────────
echo ""
echo "[2/3] Building emulator for linux/${PLATFORM} in Docker..."

# Concurrency tuning: an explicit flag or BAZEL_JOBS always wins.
if [ -n "$BAZEL_JOBS_CLI" ]; then
  BAZEL_JOBS="$BAZEL_JOBS_CLI"
  echo "  Using BAZEL_JOBS override: $BAZEL_JOBS"
elif [ -z "${BAZEL_JOBS:-}" ]; then
  DOCKER_MEMORY_BYTES=$(docker info --format '{{.MemTotal}}' 2>/dev/null || true)
  case "$DOCKER_MEMORY_BYTES" in
    ''|*[!0-9]*|0*)
      BAZEL_JOBS=2
      echo "  WARN: Could not detect Docker memory; using $BAZEL_JOBS Bazel jobs."
      ;;
    *)
      DOCKER_MEMORY_GIB=$((DOCKER_MEMORY_BYTES / 1073741824))
      if [ "$DOCKER_MEMORY_GIB" -lt 20 ]; then
        BAZEL_JOBS=1
      elif [ "$DOCKER_MEMORY_GIB" -lt 26 ]; then
        BAZEL_JOBS=2
      elif [ "$DOCKER_MEMORY_GIB" -lt 40 ]; then
        BAZEL_JOBS=4
      else
        BAZEL_JOBS=6
      fi
      echo "  Docker memory: ${DOCKER_MEMORY_GIB} GiB; using $BAZEL_JOBS Bazel jobs."
      ;;
  esac
else
  case "$BAZEL_JOBS" in
    ''|*[!0-9]*|0*)
      echo "ERROR: BAZEL_JOBS must be a positive integer, got: $BAZEL_JOBS" >&2
      exit 1
      ;;
  esac
  echo "  Using BAZEL_JOBS environment: $BAZEL_JOBS"
fi

DOCKER_BUILDKIT=1 docker buildx build \
  --builder "$BUILDER_NAME" \
  --platform "linux/${PLATFORM}" \
  --load \
  --progress=plain \
  -f "$DOCKERFILE" \
  "${CACHE_ARGS[@]}" \
  "${BUILD_ARGS[@]}" \
  --build-arg BAZEL_CACHE_NAMESPACE="$BAZEL_CACHE_NAMESPACE" \
  --build-arg BAZEL_REPO_CACHE_NAMESPACE="$BAZEL_REPO_CACHE_NAMESPACE" \
  --build-arg BAZEL_JOBS="$BAZEL_JOBS" \
  --build-arg SOURCE_REVISION="$SOURCE_REVISION" \
  --build-arg 'BAZEL_RAM=HOST_RAM*.8' \
  -t "$IMAGE_TAG" .

# ── Extract binaries ─────────────────────────────────────────────────────────
echo ""
echo "[3/3] Extracting binaries..."
mkdir -p artifacts
CONTAINER=$(docker create "$IMAGE_TAG")
docker cp "$CONTAINER:/emulator_main" "artifacts/spanner-emulator-main-${PLATFORM}" 2>/dev/null || true
docker cp "$CONTAINER:/gateway_main" "artifacts/gateway-main-${PLATFORM}" 2>/dev/null || true
docker rm "$CONTAINER" >/dev/null

BUILD_END=$(date +%s)
echo ""
echo "============================================"
if [ -f "artifacts/spanner-emulator-main-${PLATFORM}" ]; then
  echo "  BUILD SUCCESSFUL!"
  echo "  Platform: linux/${PLATFORM}"
  ls -lh "artifacts/spanner-emulator-main-${PLATFORM}"
  file "artifacts/spanner-emulator-main-${PLATFORM}"
else
  echo "  BUILD FAILED - check Docker logs"
fi
echo ""
echo "  Total time: $((BUILD_END - BUILD_START))s"
echo "  Finished:  $(date)"
echo "============================================"

#!/bin/bash
# Build the Spanner emulator using build/docker/Dockerfile.ubuntu
#
# Usage:
#   ./build.sh                                    # online, arm64 (default)
#   ./build.sh --platform=amd64                   # online, amd64 (emulated on arm64)
#   ./build.sh --offline-dir=bazel-distdir        # offline, arm64
#   ./build.sh --offline-dir=bazel-distdir --platform=amd64  # offline, amd64
#
# When --offline-dir is set, `bazel fetch` is run on the host to populate Bazel's
# repository cache (sha256-addressed, includes ALL transitive deps), then the
# cache is passed into Docker via --build-arg OFFLINE_DIR so Bazel uses the
# cached archives instead of hitting the network.
set -e

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
cd "$SCRIPT_DIR"

# ── Parse arguments ──────────────────────────────────────────────────────────
PLATFORM="arm64"
OFFLINE_DIR=""

for arg in "$@"; do
  case "$arg" in
    --platform=*)    PLATFORM="${arg#*=}" ;;
    --offline-dir=*) OFFLINE_DIR="${arg#*=}" ;;
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
IMAGE_TAG="spanner-emulator-build:${PLATFORM}"
REGISTRY_CACHE="${SPANNER_REGISTRY_CACHE-jaysen2apache/spanner-emulator-extended:buildcache-${PLATFORM}}"

echo "============================================"
echo "  Building Spanner Emulator"
echo "  Platform: linux/${PLATFORM}"
echo "  Cache:    $TOOLCHAIN_CACHE_EPOCH"
if [ -n "$OFFLINE_DIR" ]; then
  echo "  Mode:     offline (repo cache: $OFFLINE_DIR)"
else
  echo "  Mode:     online"
fi
echo "  Started:  $(date)"
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
  echo "[1/3] Populating repository cache in $OFFLINE_DIR/..."

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

  # Use bazel fetch to download ALL deps (including transitive) into the
  # repository cache. This is much more reliable than grepping URLs from
  # MODULE.bazel, which misses transitive deps and template URLs.
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

  BUILD_ARGS+=(--build-arg "OFFLINE_DIR=$OFFLINE_DIR")
  BUILD_ARGS+=(--build-arg "BAZEL_SOURCE=${OFFLINE_DIR}/${bazel_fname}")
else
  echo ""
  echo "[1/3] Skipping repo cache (online mode)..."
fi
if [ -z "$OFFLINE_DIR" ] && [ -n "$REGISTRY_CACHE" ]; then
  echo "  Importing portable BuildKit cache: $REGISTRY_CACHE"
  CACHE_ARGS+=(--cache-from "type=registry,ref=$REGISTRY_CACHE")
fi

# ── Build ────────────────────────────────────────────────────────────────────
echo ""
echo "[2/3] Building emulator for linux/${PLATFORM} in Docker..."

# Use conservative concurrency for memory-heavy GoogleSQL translation units.
# An explicit BAZEL_JOBS value always wins.
if [ -z "${BAZEL_JOBS:-}" ]; then
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
      elif [ "$DOCKER_MEMORY_GIB" -lt 32 ]; then
        BAZEL_JOBS=2
      elif [ "$DOCKER_MEMORY_GIB" -lt 44 ]; then
        BAZEL_JOBS=3
      else
        BAZEL_JOBS=4
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
  echo "  Using BAZEL_JOBS override: $BAZEL_JOBS"
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

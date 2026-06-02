#!/bin/bash
# Build the Spanner emulator using build/docker/Dockerfile.ubuntu
#
# Usage:
#   ./build.sh                                # online build (fetches deps from network)
#   ./build.sh --offline-dir=bazel-distdir    # offline build (uses pre-downloaded deps)
#
# When --offline-dir is set, `bazel fetch` is run on the host to populate Bazel's
# repository cache (sha256-addressed, includes ALL transitive deps), then the
# cache is passed into Docker via --build-arg OFFLINE_DIR so Bazel uses the
# cached archives instead of hitting the network.
set -e

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
cd "$SCRIPT_DIR"

OFFLINE_DIR=""
for arg in "$@"; do
  case "$arg" in
    --offline-dir=*) OFFLINE_DIR="${arg#*=}" ;;
  esac
done

DOCKERFILE="build/docker/Dockerfile.ubuntu"
IMAGE_TAG="spanner-emulator-build"

echo "============================================"
echo "  Building Spanner Emulator"
if [ -n "$OFFLINE_DIR" ]; then
  echo "  Mode: offline (repo cache: $OFFLINE_DIR)"
else
  echo "  Mode: online"
fi
echo "  Started: $(date)"
echo "============================================"
BUILD_START=$(date +%s)

BUILD_ARGS=()

BASE_IMAGE="ubuntu:22.04"

# For offline mode, use bazel fetch to populate the repository cache
if [ -n "$OFFLINE_DIR" ]; then
  DISTDIR="$SCRIPT_DIR/$OFFLINE_DIR"
  mkdir -p "$DISTDIR"

  # Check if pre-built base image exists locally (required for fully offline builds)
  BASE_IMAGE="spanner-emulator-base:latest"
  if ! docker image inspect "$BASE_IMAGE" >/dev/null 2>&1; then
    echo ""
    echo "[0/4] Building pre-baked base image (requires network)..."
    DOCKER_BUILDKIT=1 docker build \
      -f build/docker/Dockerfile.base \
      -t "$BASE_IMAGE" .
  fi

  echo ""
  echo "[1/4] Populating repository cache in $OFFLINE_DIR/..."

  # Pre-download the Bazel binary itself so Docker doesn't need network for it
  BAZEL_VERSION=$(cat .bazelversion | tr -d '[:space:]')
  for arch in amd64 arm64; do
    bazel_fname="bazel-${BAZEL_VERSION}-linux-${arch}"
    if [ ! -f "$DISTDIR/$bazel_fname" ]; then
      echo "  GET: $bazel_fname"
      curl -kL --max-time 600 \
        -o "$DISTDIR/$bazel_fname" \
        "https://releases.bazel.build/${BAZEL_VERSION}/release/${bazel_fname}" 2>/dev/null \
        || echo "  WARN: Failed $bazel_fname"
    fi
  done

  # Use bazel fetch to download ALL deps (including transitive) into the
  # repository cache. This is much more reliable than grepping URLs from
  # WORKSPACE, which misses transitive deps and template URLs.
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

  echo "  Creating BUILD files manifest..."
  find . -name "BUILD*" -o -name "*.bzl" -o -name "WORKSPACE*" -o -name "*.json" -o -name ".bazelversion" -o -name ".bazelrc" \
    | grep -v "bazel-" > build_files.txt
  tar -cf build_files.tar -T build_files.txt
  BUILD_ARGS+=(--build-arg "OFFLINE_DIR=$OFFLINE_DIR")
  BUILD_ARGS+=(--build-arg "BASE_IMAGE=$BASE_IMAGE")
else
  echo ""
  echo "[1/3] Skipping repo cache (online mode)..."
fi

# Build
echo ""
if [ -n "$OFFLINE_DIR" ]; then
  echo "[2/4] Building emulator in Docker..."
else
  echo "[2/3] Building emulator in Docker..."
fi

# Auto-detect cores for parallelism if not set
if [ -z "$BAZEL_JOBS" ]; then
  BAZEL_JOBS=8
fi

DOCKER_BUILDKIT=1 docker build --progress=plain \
  -f "$DOCKERFILE" \
  "${BUILD_ARGS[@]}" \
  --build-arg BAZEL_JOBS="$BAZEL_JOBS" \
  --build-arg BAZEL_RAM="HOST_RAM*.8" \
  -t "$IMAGE_TAG" .

# Extract binaries
echo ""
if [ -n "$OFFLINE_DIR" ]; then
  echo "[3/4] Extracting binaries..."
else
  echo "[3/3] Extracting binaries..."
fi
mkdir -p artifacts
CONTAINER=$(docker create "$IMAGE_TAG")
docker cp "$CONTAINER:/emulator_main" artifacts/spanner-emulator-main 2>/dev/null
docker cp "$CONTAINER:/gateway_main" artifacts/gateway-main 2>/dev/null || true
docker rm "$CONTAINER" >/dev/null

BUILD_END=$(date +%s)
echo ""
echo "============================================"
if [ -f artifacts/spanner-emulator-main ]; then
  echo "  BUILD SUCCESSFUL!"
  ls -lh artifacts/spanner-emulator-main
  file artifacts/spanner-emulator-main
else
  echo "  BUILD FAILED - check Docker logs"
fi
echo ""
echo "  Total time: $((BUILD_END - BUILD_START))s"
echo "  Finished: $(date)"
echo "============================================"

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
BAZEL_CACHE_NAMESPACE="spanner-emulator-${PLATFORM}"

DOCKERFILE="build/docker/Dockerfile.ubuntu"
IMAGE_TAG="spanner-emulator-build:${PLATFORM}"

echo "============================================"
echo "  Building Spanner Emulator"
echo "  Platform: linux/${PLATFORM}"
if [ -n "$OFFLINE_DIR" ]; then
  echo "  Mode:     offline (repo cache: $OFFLINE_DIR)"
else
  echo "  Mode:     online"
fi
echo "  Started:  $(date)"
echo "============================================"
BUILD_START=$(date +%s)

BUILD_ARGS=()

echo ""
echo "Bootstrapping Buildx builder: $BUILDER_NAME"
if docker buildx inspect "$BUILDER_NAME" >/dev/null 2>&1; then
  if ! docker buildx inspect "$BUILDER_NAME" --bootstrap; then
    echo "ERROR: Existing Buildx builder '$BUILDER_NAME' could not bootstrap." >&2
    echo "Set SPANNER_BUILDER to a different builder name and retry." >&2
    exit 1
  fi
else
  if ! docker buildx create --name "$BUILDER_NAME" --driver docker-container --bootstrap; then
    echo "ERROR: Could not create Buildx builder '$BUILDER_NAME'." >&2
    echo "If that name already exists but is unusable, set SPANNER_BUILDER to a different name." >&2
    exit 1
  fi
fi

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
  # Only include Bazel-relevant files, excluding IDE/tool configs and node_modules.
  # The *.json glob is intentionally NOT used — it pulled in node_modules, IDE
  # config, and other irrelevant JSON that poisoned the deps stage Docker cache.
  {
    find . \( -name "BUILD" -o -name "BUILD.bazel" -o -name "*.bzl" \) \
      -not -path "./.opencode/*" \
      -not -path "./.cursor/*" \
      -not -path "./.gemini/*" \
      -not -path "./.claude/*" \
      -not -path "./.kiro/*" \
      -not -path "./.vscode/*" \
      -not -path "./.continue/*" \
      -not -path "./.idea/*" \
      -not -path "./.git/*" \
      -not -path "./bazel-*/*" | sed 's#^\./##'
    echo WORKSPACE
    echo maven_install.json
    echo .bazelversion
    echo .bazelrc
    echo BUILD.bazel
  } | LC_ALL=C sort -u > build_files.txt
  tar -cf build_files.tar -T build_files.txt
  BUILD_ARGS+=(--build-arg "OFFLINE_DIR=$OFFLINE_DIR")
  BUILD_ARGS+=(--build-arg "BAZEL_SOURCE=${OFFLINE_DIR}/${bazel_fname}")
else
  echo ""
  echo "[1/3] Skipping repo cache (online mode)..."
fi

# ── Build ────────────────────────────────────────────────────────────────────
echo ""
echo "[2/3] Building emulator for linux/${PLATFORM} in Docker..."

# Auto-detect cores for parallelism if not set
if [ -z "$BAZEL_JOBS" ]; then
  BAZEL_JOBS=8
fi

DOCKER_BUILDKIT=1 docker buildx build \
  --builder "$BUILDER_NAME" \
  --platform "linux/${PLATFORM}" \
  --load \
  --progress=plain \
  -f "$DOCKERFILE" \
  "${BUILD_ARGS[@]}" \
  --build-arg BAZEL_CACHE_NAMESPACE="$BAZEL_CACHE_NAMESPACE" \
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

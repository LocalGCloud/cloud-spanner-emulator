#!/bin/sh
set -eu

if [ "$#" -ne 1 ]; then
  echo "usage: $0 CACHE_DIRECTORY" >&2
  exit 2
fi

cache_dir=$1
marker="$cache_dir/.spanner-toolchain-fingerprint"
tmp=$(mktemp)
trap 'rm -f "$tmp"' EXIT

dpkg-query -W -f='${binary:Package}=${Version}\n' > "$tmp"
sha256sum /usr/local/bin/bazel >> "$tmp"
printf '%s\n' \
  "BAZEL_CXXOPTS=${BAZEL_CXXOPTS:-}" \
  "EXTRA_BAZEL_ARGS=${EXTRA_BAZEL_ARGS:-}" >> "$tmp"
LC_ALL=C sort -o "$tmp" "$tmp"
fingerprint_line=$(sha256sum "$tmp")
fingerprint=${fingerprint_line%% *}

mkdir -p "$cache_dir"
if [ -f "$marker" ]; then
  previous=$(cat "$marker")
  if [ "$previous" != "$fingerprint" ]; then
    echo "Toolchain changed; invalidating cache at $cache_dir" >&2
    find "$cache_dir" -mindepth 1 -maxdepth 1 \
      ! -name '.spanner-toolchain-fingerprint' -exec rm -rf {} +
  fi
fi
printf '%s\n' "$fingerprint" > "$marker"

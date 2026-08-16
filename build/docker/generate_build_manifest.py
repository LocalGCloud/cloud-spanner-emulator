#!/usr/bin/env python3
"""Create the minimal, reproducible Bazel dependency manifest for Docker builds."""

from __future__ import annotations

import io
import os
from pathlib import Path
import tarfile


WORKSPACE = Path(__file__).resolve().parents[2]
MANIFEST_PATH = WORKSPACE / "build_files.txt"
ARCHIVE_PATH = WORKSPACE / "build_files.tar"

IGNORED_DIRECTORIES = {
    ".git",
    ".opencode",
    ".cursor",
    ".gemini",
    ".claude",
    ".kiro",
    ".vscode",
    ".continue",
    ".idea",
    "node_modules",
}
ROOT_FILES = (
    ".bazelrc",
    ".bazelversion",
    "WORKSPACE",
    "WORKSPACE.bazel",
    "MODULE.bazel",
    "MODULE.bazel.lock",
    "requirements.txt",
    "go.mod",
    "go.sum",
)
BAZEL_GLOB_PLACEHOLDERS = (
    "tests/common/testdata/*.test",
    "tests/conformance/data/schema_changes/**/*.test",
    "tests/conformance/data/schemas/*.test",
    "third_party/spanner_pg/bootstrap_catalog/spanner_pg_data/pg_*.dat",
    "third_party/spanner_pg/ddl/types/testdata/*.json",
    "third_party/spanner_pg/src/backend/snowball/stopwords/*.stop",
    "third_party/spanner_pg/src/include/**/*.h",
    "third_party/spanner_pg/src/include/catalog/pg_*.dat",
)


def bazel_manifest() -> tuple[list[str], set[str]]:
    paths: set[str] = set()

    for directory, directories, filenames in os.walk(WORKSPACE):
        relative_directory = Path(directory).relative_to(WORKSPACE)
        directories[:] = sorted(
            name
            for name in directories
            if name not in IGNORED_DIRECTORIES
            and not (
                relative_directory == Path(".") and name.startswith("bazel-")
            )
        )

        for filename in filenames:
            if filename in {"BUILD", "BUILD.bazel"} or filename.endswith(".bzl"):
                paths.add((relative_directory / filename).as_posix())

    paths.update(name for name in ROOT_FILES if (WORKSPACE / name).exists())
    placeholders = {
        source.relative_to(WORKSPACE).as_posix()
        for pattern in BAZEL_GLOB_PLACEHOLDERS
        for source in WORKSPACE.glob(pattern)
        if source.is_file()
    }
    paths.update(placeholders)
    return sorted(paths), placeholders


def normalized_tarinfo(
    archive: tarfile.TarFile, source: Path, archive_name: str
) -> tarfile.TarInfo:
    info = archive.gettarinfo(str(source), arcname=archive_name)
    info.uid = 0
    info.gid = 0
    info.uname = ""
    info.gname = ""
    info.mtime = 0
    info.mode = 0o777 if info.issym() else 0o644
    return info


def main() -> None:
    paths, placeholders = bazel_manifest()
    manifest_tmp = MANIFEST_PATH.with_suffix(".txt.tmp")
    archive_tmp = ARCHIVE_PATH.with_suffix(".tar.tmp")

    manifest_tmp.write_text("".join(f"{path}\n" for path in paths), encoding="utf-8")

    with tarfile.open(archive_tmp, mode="w", format=tarfile.GNU_FORMAT) as archive:
        for relative_path in paths:
            source = WORKSPACE / relative_path
            info = normalized_tarinfo(archive, source, relative_path)
            if relative_path in placeholders:
                info.size = 0
                archive.addfile(info, io.BytesIO())
            elif info.isfile():
                with source.open("rb") as file:
                    archive.addfile(info, file)
            else:
                archive.addfile(info)

    os.replace(manifest_tmp, MANIFEST_PATH)
    os.replace(archive_tmp, ARCHIVE_PATH)
    print(
        f"Created {ARCHIVE_PATH.name} from {len(paths) - len(placeholders)} "
        f"Bazel metadata files and {len(placeholders)} glob placeholders"
    )


if __name__ == "__main__":
    main()

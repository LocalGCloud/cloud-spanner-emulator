# Internals

How parts of this fork work inside, for maintainers. Each note cites the
source files it describes; the code is the authority where they differ. For
user-facing behavior, see [capabilities](../capabilities.md) and
[known gaps](../known-gaps.md).

| Note | Covers |
|------|--------|
| [Persistent storage](persistent-storage.md) | LevelDB storage, the write queue, `metadata.json` and `backup_catalog.json` formats, marker files, startup, checkpoints, garbage collection |
| [Change streams](change-streams.md) | Internal tables, the commit path, partition rotation, backfill, persistence |

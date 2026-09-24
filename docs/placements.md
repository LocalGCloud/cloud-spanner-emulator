# Geo-partitioning: placements and placement keys

The emulator supports
[geo-partitioning](https://cloud.google.com/spanner/docs/create-manage-data-placements)
DDL, writes, metadata and DML limits in both dialects, as far as a
single-process emulator can. Every row is stored locally whatever its
placement, so physical placement, routing latency and row-move throughput
aren't emulated.

Upstream parses only basic GoogleSQL placement DDL. Three
`ALTER COLUMN ... PLACEMENT KEY` forms crash the upstream emulator, it rejects
the documented `default` placement key value, and `DROP PLACEMENT` can orphan
rows. It has no PostgreSQL syntax, no `INFORMATION_SCHEMA` views and none of
production's placement DML limits.

## Example

```sql
-- GoogleSQL
CREATE PLACEMENT europe OPTIONS (instance_partition = 'eu-partition');
CREATE TABLE Singers (
  SingerId INT64 NOT NULL,
  Location STRING(MAX) NOT NULL PLACEMENT KEY
) PRIMARY KEY (SingerId);

-- PostgreSQL
CREATE PLACEMENT europe WITH (instance_partition = 'eu-partition');
CREATE TABLE singers (
  singerid bigint PRIMARY KEY,
  location varchar(1024) NOT NULL PLACEMENT KEY
);
```

Create the instance partition (`eu-partition`) first, with the instance admin
API.

## What's supported

- **Placements.** `CREATE`, `ALTER` (GoogleSQL only) and `DROP PLACEMENT`, with
  `IF [NOT] EXISTS`, and the `instance_partition`, `default_leader` and
  `read_lease_regions` options. The instance partition must exist.
  `ALTER PLACEMENT` changes only the options it names.
- **Placement keys.** One `NOT NULL STRING` column per table, defined only in
  `CREATE TABLE`. It can't be added, dropped or loosened later.
- **Writes.** A placement key value must name a placement in the database or
  be `default`, the implicit default placement.
- **Safety checks.** `DROP PLACEMENT` fails while any row uses the placement.
  `DROP TABLE` fails on a placement table that still has rows. An instance
  partition can't be deleted while a placement uses it.
- **Routing metadata.** The `per_placement_routing_metadata` database option,
  which can't change once placements exist.
- **Metadata.** `INFORMATION_SCHEMA.PLACEMENTS` (including `default`),
  `PLACEMENT_OPTIONS`, `PLACEMENT KEY` rows in `TABLE_CONSTRAINTS`, the routing
  option in `DATABASE_OPTIONS`, and `GetDatabaseDdl` output that can be
  replayed.
- **Persistence.** Databases persisted with `--data_dir`, and backups, keep
  loading. Replayed DDL skips checks that didn't exist when it was first
  applied.

## DML limits

Production applies these limits to geo-partitioned databases during Preview,
and the emulator enforces them by default. In read-write transactions:

- an `INSERT` or `DELETE` on a placement table must be the only statement in
  its transaction;
- `WHERE` clauses may reference only the primary key columns of placement
  tables.

Partitioned DML, read-only transactions and mutations aren't affected. To turn
the limits off, pass `--enforce_placement_dml_restrictions=false` to
`gateway_main` or `emulator_main`. In Docker, repeat the full command:

```shell
docker run -p 9010:9010 -p 9020:9020 jaysen2apache/spanner-emulator-extended \
  ./gateway_main --hostname 0.0.0.0 --enforce_placement_dml_restrictions=false
```

## Not emulated

- Physical placement of rows. Every row is stored locally.
- Routing latency and row-move throughput.
- Instance partition capacity. Instance partitions are metadata only.

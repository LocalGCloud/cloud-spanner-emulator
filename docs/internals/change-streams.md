# Change streams internals

This document describes how change streams are implemented: the internal
tables, how records are written at commit, how partitions are created and
replaced, how queries are served, and how change streams persist. It is for
people changing the code. For user-facing behavior, see
[Change streams](../change-streams.md).

## Source map

| Area | Source |
|------|--------|
| GoogleSQL DDL parsing | `backend/schema/parser/ddl_parser.cc` |
| PostgreSQL DDL translation | `third_party/spanner_pg/ddl/pg_to_spanner_ddl_translator.cc` |
| Catalog object, defaults, partition mode names | `backend/schema/catalog/change_stream.{h,cc}` |
| Builder and editor | `backend/schema/builders/change_stream_builder.h` |
| Schema validation (name, immutable `partition_mode`) | `backend/schema/validators/change_stream_validator.{h,cc}` |
| `CREATE`, `ALTER`, `DROP`, option and `FOR` clause checks, internal tables, TVF names | `backend/schema/updater/schema_updater.cc` |
| Initial partitions | `backend/schema/backfills/change_stream_backfill.{h,cc}` |
| Record generation at commit | `backend/actions/change_stream.{h,cc}` |
| Commit hook | `backend/transaction/read_write_transaction.cc` (`ProcessChangeStreamWriteOps`) |
| Partition churner | `backend/database/change_stream/change_stream_partition_churner.{h,cc}` |
| TVF definition | `backend/query/change_stream/queryable_change_stream_tvf.{h,cc}` |
| TVF registration and internal table lookup | `backend/query/catalog.cc` |
| Query validation | `backend/query/change_stream/change_stream_query_validator.{h,cc}` |
| Query serving | `frontend/handlers/change_streams.{h,cc}`, dispatched from `frontend/handlers/queries.cc` |
| Result conversion | `frontend/converters/change_streams.{h,cc}` (GoogleSQL `STRUCT` and proto), `frontend/converters/pg_change_streams.{h,cc}` (PostgreSQL `JSONB` and bytes) |
| Output type, TVF prefixes, internal table prefixes | `common/constants.h` |
| Limits | `common/limits.h` |
| Partition table name helpers | `common/change_stream.{h,cc}` |
| `INFORMATION_SCHEMA` views | `backend/query/information_schema_catalog.cc` |

## Internal tables

`CreateChangeStream` in `schema_updater.cc` creates two tables for each change
stream. Both are owned by the change stream (`set_owner_change_stream`) and
are dropped with it. They aren't visible to user SQL: a query that names them
fails with `Table not found`.

### Partition table: `_change_stream_partition_<name>`

| Column | Type | Meaning |
|--------|------|---------|
| `partition_token` | `STRING` | Primary key |
| `start_time` | `TIMESTAMP` | When the partition became active (commit timestamp) |
| `end_time` | `TIMESTAMP` | When it was replaced. `NULL` while active. |
| `parents` | `ARRAY<STRING>` | Tokens it replaced |
| `children` | `ARRAY<STRING>` | Tokens that replaced it |
| `next_churn` | `STRING` | `MOVE`, `SPLIT`, or `MERGE`: what the churner does to it next |

### Data table: `_change_stream_data_<name>`

Primary key: `(partition_token, commit_timestamp, server_transaction_id,
record_sequence)`. The table is interleaved in the partition table with
`ON DELETE NO ACTION`.

The other columns hold one data change record each:
`is_last_record_in_transaction_in_partition`, `table_name`,
`column_types_name`, `column_types_type`, `column_types_is_primary_key`,
`column_types_ordinal_position`, `mods_keys`, `mods_new_values`,
`mods_old_values`, `mod_type`, `value_capture_type`,
`number_of_records_in_transaction`, `number_of_partitions_in_transaction`,
`transaction_tag`, and `is_system_transaction`. The `mods_*` columns are
`ARRAY<STRING>` with one JSON string per mod. `column_types_type` holds JSON
type strings such as `{"code":"INT64"}`.

## Partition tokens and the initial backfill

`CreateChangeStream` registers `BackfillChangeStream` as a statement action.
It inserts two partitions whose `start_time` is the change stream's creation
time, with no parents or children. The first has `next_churn = SPLIT` and the
second has `next_churn = MOVE`.

Tokens come from `CreatePartitionTokenString`: 131 to 170 random alphanumeric
characters, web-safe base64 encoded.

The backfill is idempotent. Before inserting, it reads the partition table at
`absl::InfiniteFuture()` and returns if any row exists. DDL replay at startup
runs `CREATE CHANGE STREAM` again against storage that already has the
partitions, and this check keeps it from adding more.

## Partition churner

`Database::Create` makes one `ChangeStreamPartitionChurner` per database and
calls `Update(schema)` after every schema change. `Update` starts one thread
per change stream and stops threads for dropped change streams.

Each thread loops:

1. Wait `--change_stream_churn_thread_sleep_interval` (default 20 s).
2. In one read-write transaction, read the partition table and pick the
   active partitions (`end_time IS NULL`) whose `start_time` is more than
   `--change_stream_churning_interval` (default 20 s) in the past.
3. Replace them based on `next_churn`:
   - `MOVE`: one new partition with `next_churn = MOVE`. Skipped for
     `MUTABLE_KEY_RANGE` change streams.
   - `SPLIT`: two new partitions with `next_churn = MERGE`.
   - `MERGE`: pairs of partitions become one new partition with
     `next_churn = SPLIT`.

   New partitions get `start_time = spanner.commit_timestamp()` and their
   parents. Replaced partitions get `end_time = spanner.commit_timestamp()`
   and their children.
4. On failure, wait `--change_stream_churn_thread_retry_sleep_interval` (20 ms)
   plus up to `--change_stream_churn_thread_retry_jitter` ms, and retry. Only
   non-`ABORTED` failures are logged.

So a partition lives 20–40 seconds, and a change stream has two or three
active partitions. Churning doesn't depend on load or key ranges.

`--override_change_stream_partition_token_alive_seconds=X` (X > 0) sets the
churning interval, the sleep interval, and the retry sleep interval to X
seconds in the churner constructor. `--enable_change_stream_churning=false`
stops the threads from starting.

The churn transaction competes with user transactions. The emulator runs one
read-write transaction at a time, so churning can abort a user transaction.
See the TODO above `ChurnPartitions`.

## Record generation at commit

`ReadWriteTransaction::Commit` calls `ProcessChangeStreamWriteOps` before it
reserves a commit timestamp. That calls `BuildChangeStreamWriteOps` in
`backend/actions/change_stream.cc` with the transaction's buffered write ops,
then buffers the returned inserts into the data tables through
`TransactionStore::BufferWriteOp`. The records commit in the same transaction
as the user's writes.

`BuildChangeStreamWriteOps`:

1. Maps each table to the change streams that track it
   (`RetrieveTableWithTrackedChangeStreams`).
2. Skips change streams with `allow_txn_exclusion = true` when the transaction
   set `exclude_txn_from_change_streams`.
3. Picks one partition per change stream for the whole transaction: the
   active token that sorts first (`RetrieveChangeStreamWithPartitionToken`).
   Every write goes to that partition.
4. For each buffered op, `LogTableMod` applies `exclude_insert`,
   `exclude_update`, and `exclude_delete`, keeps only tracked columns, and
   computes `new_values` and `old_values` for the value capture type. Old
   values come from `ReadCommitted`. An `UPDATE` with no tracked non-key
   columns produces no mod.
5. Consecutive mods with the same table, mod type, and tracked non-key column
   set go into one `ModGroup`. A change starts a new data change record.
6. `BuildMutation` sets `number_of_records_in_transaction`, marks the last
   record with `is_last_record_in_transaction_in_partition`, and converts each
   record to an `InsertOp` on the data table.

Fixed values: `record_sequence` is `%08d` starting at `00000000`,
`server_transaction_id` is the transaction ID, `commit_timestamp` is the
commit timestamp sentinel (filled in at flush),
`number_of_partitions_in_transaction` is 1, `transaction_tag` is empty, and
`is_system_transaction` is `false`. Empty value maps are written as `{}`.
`CloudValueToJSONValue` encodes `INT64` and `NUMERIC` as strings and `BYTES`
and `PROTO` as base64.

The buffered ops are what the transaction store holds at commit, not the
individual client writes. That is why several writes to one row become one
mod, and why the disabled tests `DISABLED_SingleReplaceExistingRow`,
`DISABLED_ConsecutiveReplace`, and
`DISABLED_DataChangeRecordOrderForMultiTablesSameTransaction` in
`tests/conformance/cases/change_streams_read_write.cc` fail.

`exclude_ttl_deletes` is stored and printed but not read here. The emulator
doesn't run TTL deletions.

## TVFs and query validation

`MakeChangeStreamTvfName` names the TVF: `READ_<name>` in GoogleSQL,
`read_json_<name>` in PostgreSQL, and `read_proto_bytes_<name>` for
PostgreSQL `MUTABLE_KEY_RANGE` change streams. `Catalog` registers one
`QueryableChangeStreamTvf` per change stream. Its one output column,
`ChangeRecord`, has the type in `kChangeStreamTvfOutputFormat` (with `JSON`
fields) in GoogleSQL, the `google.spanner.v1.ChangeStreamRecord` proto for
GoogleSQL `MUTABLE_KEY_RANGE`, `JSONB` in PostgreSQL, or bytes for PostgreSQL
`MUTABLE_KEY_RANGE`. PostgreSQL results name the column after the TVF.

The TVF takes five arguments: `start_timestamp`, `end_timestamp`,
`partition_token`, `heartbeat_milliseconds`, and `read_options`. All are
optional in the signature with `NULL` defaults; the validator enforces the
required ones.

`ChangeStreamQueryValidator` runs after analysis:

- `IsChangeStreamQuery` finds a TVF scan whose name has a change stream TVF
  prefix and a matching change stream.
- `ValidateQuery` requires a query statement over a project scan over the TVF
  scan, with one output column (`ChangeRecord`, or the TVF name in
  PostgreSQL), and no alias.
- Arguments must be literals or parameters.
- `ValidateTimeStamps`: `start_timestamp` is required and must be in
  `[max(now - retention, creation_time), now + 10 min]`. For
  `MUTABLE_KEY_RANGE`, `end_timestamp` is required and at most
  `start + 30 min`. `end_timestamp` must not be before `start_timestamp`.
- `heartbeat_milliseconds` must be in `[100, 300000]`, and `read_options`
  must be `NULL`.

## Query serving

`ExecuteStreamingSql` in `frontend/handlers/queries.cc` hands change stream
queries to `ChangeStreamsHandler::ExecuteChangeStreamQuery`. It rejects any
transaction other than single-use strong read-only, and `PLAN` mode.

The handler reads the internal tables with ordinary queries that set
`Query::change_stream_internal_lookup`. That makes `Catalog` expose the two
internal tables, and the query is analyzed as GoogleSQL even in PostgreSQL
databases.

- **Initial query** (`partition_token` is `NULL`): selects partitions with
  `start_time <= start_timestamp` and `end_time` `NULL` or after it, and
  returns one child partitions record per partition with `start_timestamp`
  set to the requested value. A `RET_CHECK` requires at least one partition.
- **Partition query**: reads the token's `start_time` and `end_time` and
  checks that `start_timestamp` is inside them. It then loops over time slices
  of `--change_streams_partition_query_chop_interval` (100 ms). For each
  slice it opens a read at the slice end, which waits for that time to pass.
  It re-reads the current retention and the token's `end_time`, rejects the
  read if the slice starts before `now - retention`, and streams the data
  rows ordered by `(partition_token, commit_timestamp, server_transaction_id,
  record_sequence)`. When no rows arrive for `heartbeat_milliseconds`, it
  sends a heartbeat. When the slice reaches the token's `end_time`, it sends
  the token's children as one child partitions record and stops. If nothing
  was sent, it ends with one heartbeat at the end time.
- **Mutable key range** change streams use the proto (GoogleSQL) or bytes
  (PostgreSQL) converters, which also produce partition start, event, and end
  records.

Each response carries the placeholder resume token
`kChangeStreamDummyResumeToken`, and resume tokens in requests aren't used.

## Persistence

With `--data_dir`, the internal tables are ordinary tables in the database's
`PersistentStorage`, a LevelDB store at
`<data_dir>/<database resource path>/storage`, for example
`<data_dir>/projects/p/instances/i/databases/d/storage`
(`Database::PersistentStorageDirectory` in `backend/database/database.cc`).
See [Persistent storage internals](persistent-storage.md) for the key format.

Change stream definitions persist as DDL in `metadata.json`. Each database
has `ddlBatches`, and each batch has `statements`, `protoDescriptors`, and
`schemaChangeTimestamp` (`MetadataStore::Save` in
`frontend/persistence/metadata_store.cc`).

### Creation time

`CreateChangeStream` sets `creation_time` to the schema change timestamp.
At startup, `emulator_main.cc` replays each batch with its
`schemaChangeTimestamp` (`replaying_committed_ddl = true`), so the creation
time is restored. The first batch falls back to the database create time when
it has no timestamp. Later batches without a timestamp use the replay time.
`DatabaseManager::Creation::Build` gives the initial schema the database
create time when its timestamp is unset.

`DatabaseManager` picks one create time per database (`clock->Now()` for
`CreateDatabase`, the restore time for `RestoreDatabase`). `Creation::Build`
uses it as the initial batch's timestamp, and `Database::ToProto` in
`frontend/entities/database.cc` returns it as `create_time`. The
`CreateDatabase` handler in `frontend/handlers/databases.cc` saves that
`create_time` as both `createTime` and the first batch's
`schemaChangeTimestamp`, so live and replayed creation times match.

Builds before 2026-09-24 didn't set `create_time` in `ToProto`, so their
metadata holds `1970-01-01T00:00:00+00:00`. Replaying it gives change streams
from `CreateDatabase` a creation time earlier than their first partition. The
initial query in `ChangeStreamsHandler::ExecuteInitialQuery`
(`frontend/handlers/change_streams.cc`) then finds no partition covering
`start_timestamp`, and returns `OUT_OF_RANGE` rather than failing a
`RET_CHECK`.

Tests: `PersistentDatabaseDdlTest.CreateTimeIsReportedPersistedAndReplayedForInitialStatements`
(`frontend/handlers/databases_test.cc`),
`DatabaseTest.ChangeStreamCreationTimePersistsAndMatchesCreateTime`
(`backend/database/database_test.cc`),
`DatabaseManagerTest.InitialSchemaUsesPersistedCreationTimestamp` and
`DatabaseManagerTest.LegacyInitialTimestampDoesNotDuplicatePartitionsOnReplay`
(`frontend/collections/database_manager_test.cc`).

### ID counters

`metadata.json` stores `idCounters` with `tableId`, `columnId`, and
`changeStreamId`. The internal tables take IDs from the table and column
generators, so those two counters keep them from colliding after a restart.
`change_stream_id_generator_` in `Database` is seeded and saved, but nothing
assigns change stream IDs from it.

### Retention

Nothing deletes change stream records after `retention_period`. Retention is
enforced only when queries are validated. `PersistentStorage` removes old
versions of a cell when the cell is written again, and data table rows are
written once, so they stay until the change stream or database is dropped.

## Tests

| Test | Covers |
|------|--------|
| `tests/conformance/cases/change_streams_read_write.cc` | Reads end to end: value capture types, mod types, data types, tracking, partitions, disabled gap tests |
| `tests/conformance/cases/pg_change_streams_read_write.cc` | PostgreSQL reads |
| `tests/conformance/cases/change_streams_exclusion.cc` | `exclude_insert`, `exclude_update`, `exclude_delete` |
| `tests/conformance/cases/change_streams_exclusion_txn.cc` | `allow_txn_exclusion` with `exclude_txn_from_change_streams` |
| `tests/conformance/cases/change_streams_mutable_key_range.cc` | `MUTABLE_KEY_RANGE` |
| `tests/conformance/data/schema_changes/change_streams.test` | GoogleSQL DDL |
| `tests/conformance/data/schema_changes/pg/ddl.{create,alter,drop}_change_stream.test` | PostgreSQL DDL |
| `backend/schema/updater/schema_updater_tests/change_stream_test.cc` | Schema updates |
| `backend/schema/backfills/change_stream_backfill_test.cc` | Initial partitions and token uniqueness |
| `backend/actions/change_stream_test.cc` | Record generation |
| `backend/database/change_stream/change_stream_partition_churner_test.cc` | Churner |
| `backend/query/change_stream/*_test.cc` | TVFs and query validation, both dialects |
| `frontend/handlers/change_streams_test.cc` | Query serving |
| `frontend/converters/change_streams_test.cc`, `frontend/converters/pg_change_streams_test.cc` | Result conversion |

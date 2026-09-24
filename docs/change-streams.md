# Change streams

The emulator supports change streams in GoogleSQL and PostgreSQL databases. You
can create, alter, and drop change streams, read them with the change stream
table-valued functions (TVFs), and get data change, heartbeat, and child
partition records with all four value capture types and the exclusion options.
With `--data_dir`, change streams and their records survive restarts.

Use the emulator to develop and test change stream consumers. It doesn't
reproduce production partitioning: partitions are replaced on a timer (every
20–40 seconds by default), and all writes go to one partition. See
[Differences from production](#differences-from-production) before you rely on
partition behavior, ordering, or record details.

For concepts, see the
[Cloud Spanner change streams documentation](https://cloud.google.com/spanner/docs/change-streams).
For how it's built, see [Change streams internals](internals/change-streams.md).

## Quick start

1. Start the emulator. With `--data_dir`, change streams survive restarts:

   ```shell
   docker run -p 9010:9010 -p 9020:9020 \
     -v /path/to/data:/data \
     jaysen2apache/spanner-emulator-extended \
     ./gateway_main --hostname 0.0.0.0 --data_dir=/data
   ```

   The image has no entrypoint, so repeat the whole command when you add
   flags. Without `--data_dir`, everything is in memory and is lost when the
   emulator stops. See [Persistence](persistence.md) and
   [Configuration](configuration.md).

2. Create a change stream:

   ```sql
   CREATE TABLE Users (
     UserId INT64 NOT NULL,
     Name STRING(100),
     Email STRING(100),
   ) PRIMARY KEY (UserId);

   CREATE CHANGE STREAM Changes FOR Users;
   ```

3. Read it with the flow in [Read a change stream](#read-a-change-stream). A
   query with a `NULL` partition token returns only partition tokens, not
   data.

## Create and alter change streams

### GoogleSQL

```sql
CREATE CHANGE STREAM AllChanges FOR ALL;
CREATE CHANGE STREAM UserChanges FOR Users, Orders;
CREATE CHANGE STREAM ContactChanges FOR Users(Email, PhoneNumber);
CREATE CHANGE STREAM UserKeys FOR Users();          -- key columns only
CREATE CHANGE STREAM Unassigned;                    -- tracks nothing yet
CREATE CHANGE STREAM Audit FOR ALL OPTIONS (
  retention_period = '7d',
  value_capture_type = 'NEW_ROW_AND_OLD_VALUES'
);

ALTER CHANGE STREAM UserChanges SET FOR Orders;
ALTER CHANGE STREAM UserChanges SET OPTIONS (retention_period = '36h');
ALTER CHANGE STREAM UserChanges DROP FOR ALL;       -- stop tracking
DROP CHANGE STREAM UserChanges;
```

### PostgreSQL

PostgreSQL uses `WITH (...)` to create and `SET (...)` or `RESET (...)` to
alter options. `OPTIONS` is a syntax error in PostgreSQL databases.

```sql
CREATE CHANGE STREAM all_changes FOR ALL;
CREATE CHANGE STREAM contact_changes FOR users(email);
CREATE CHANGE STREAM audit FOR ALL
  WITH (retention_period = '7d', value_capture_type = 'NEW_ROW_AND_OLD_VALUES');

ALTER CHANGE STREAM audit SET (retention_period = '36h');
ALTER CHANGE STREAM audit RESET (retention_period);  -- back to the default
ALTER CHANGE STREAM contact_changes SET FOR users;
ALTER CHANGE STREAM contact_changes DROP FOR ALL;
DROP CHANGE STREAM contact_changes;
```

### What a change stream tracks

- The `FOR` clause is optional. Without it, the change stream tracks nothing
  until you run `ALTER CHANGE STREAM ... SET FOR`.
- `FOR ALL` tracks every table, including tables created later.
- `FOR Users` tracks all columns of `Users`. `FOR Users(Email)` tracks the key
  columns and `Email`. `FOR Users()` tracks only the key columns.
- Key columns are always tracked. Listing one in the column list is an error.
- You can't track indexes, views, other change streams, or non-key stored
  generated columns.
- `ALTER CHANGE STREAM ... DROP FOR ALL` fails on a change stream that has no
  `FOR` clause.

### Options

| Option | Values | Default | Notes |
|--------|--------|---------|-------|
| `retention_period` | `<N>s`, `<N>m`, `<N>h`, or `<N>d`, from 24 hours to 7 days | `1d` | Values outside the range fail with `Invalid retention_period`. |
| `value_capture_type` | `OLD_AND_NEW_VALUES`, `NEW_VALUES`, `NEW_ROW`, `NEW_ROW_AND_OLD_VALUES` | `OLD_AND_NEW_VALUES` | See [Value capture types](#value-capture-types). |
| `exclude_insert`, `exclude_update`, `exclude_delete` | `true`, `false` | `false` | Skip that kind of change. |
| `exclude_ttl_deletes` | `true`, `false` | `false` | Accepted and shown in the DDL, but has no effect: the emulator never runs TTL deletions. |
| `allow_txn_exclusion` | `true`, `false` | `false` | When `true`, this change stream skips transactions that set `exclude_txn_from_change_streams`. Other change streams still record them. |
| `partition_mode` | `IMMUTABLE_KEY_RANGE`, `MUTABLE_KEY_RANGE` | `IMMUTABLE_KEY_RANGE` | Can't be changed after creation. See [Mutable key range change streams](#mutable-key-range-change-streams). |

Setting an option to `NULL` doesn't reset it. In GoogleSQL,
`ALTER CHANGE STREAM ... SET OPTIONS (retention_period = NULL)` succeeds and
the DDL then shows `retention_period = NULL`, but the change stream keeps its
earlier value. `INFORMATION_SCHEMA.CHANGE_STREAM_OPTIONS` shows the value that
applies. In PostgreSQL, `SET (option = null)` doesn't reset the option either
(for `value_capture_type` it fails with an error); use `RESET (option)`
instead.

## Read a change stream

### Query rules

- Run change stream queries with `ExecuteStreamingSql` in a single-use,
  strong, read-only transaction. Other transactions and `ExecuteSql` fail. In
  the Python client, `database.snapshot()` with `execute_sql()` meets this
  rule.
- The query must be only the TVF call:
  `SELECT ChangeRecord FROM READ_<name>(...)` (or `SELECT *`) in GoogleSQL, and
  `SELECT * FROM spanner.read_json_<name>(...)` in PostgreSQL. Aliases,
  filters, joins, and extra columns are rejected.
- Arguments must be literals or query parameters. Expressions such as
  `CURRENT_TIMESTAMP()` are rejected. GoogleSQL also accepts named arguments,
  such as `start_timestamp => @start`.

### Arguments

| Argument | Required | Rules |
|----------|----------|-------|
| `start_timestamp` | Yes | Not earlier than the change stream's creation time or `now - retention_period`. Not later than `now + 10 minutes`. |
| `end_timestamp` | No | `NULL` means no end. Must be at or after `start_timestamp`. |
| `partition_token` | No | `NULL` for the initial query, or a token from a child partitions record. |
| `heartbeat_milliseconds` | Yes | 100 to 300000. |
| `read_options` | No | Fifth argument. Must be `NULL` or omitted. |

A query at a future `end_timestamp` waits until that time passes.

### How a read works

1. Run the **initial query** with a `NULL` partition token. It returns one
   child partitions record for each partition that is active at
   `start_timestamp`, then ends. It returns no data. By default there are two
   or three active partitions. Each record's `start_timestamp` is the one you
   passed.
2. Run a **partition query** for each token, using the `start_timestamp` from
   the child partitions record that named it. It returns the data change
   records for that partition in commit timestamp order, plus heartbeats.
3. A partition query ends at `end_timestamp` or when its partition is
   replaced, whichever comes first. When the partition is replaced, the last
   row is a child partitions record with the tokens that replace it: one
   token (a move), two tokens (a split), or one token with two parents (a
   merge). Both parents of a merged partition report the same child token, so
   query each token only once.
4. Keep querying new tokens until you reach your end time.

Partitions are replaced every 20–40 seconds by default, so a partition query
with no `end_timestamp` still returns within about 40 seconds. A reader must
follow child tokens to keep reading.

Records from different partitions aren't ordered relative to each other. Sort
by `commit_timestamp` if you need one order.

A heartbeat record means the partition has no more records up to its
`timestamp`. The emulator sends one when a partition has no records for
`heartbeat_milliseconds`, and a partition query that returns nothing else
ends with one heartbeat.

Reader libraries that follow child partitions for you, such as Apache Beam's
`SpannerIO.readChangeStream`, implement this flow. They haven't been tested
against this emulator.

### Result shape

In GoogleSQL, each row has one column, `ChangeRecord`, of type:

```
ARRAY<STRUCT<
  data_change_record ARRAY<STRUCT<
    commit_timestamp TIMESTAMP,
    record_sequence STRING,
    server_transaction_id STRING,
    is_last_record_in_transaction_in_partition BOOL,
    table_name STRING,
    column_types ARRAY<STRUCT<name STRING, type JSON,
                              is_primary_key BOOL, ordinal_position INT64>>,
    mods ARRAY<STRUCT<keys JSON, new_values JSON, old_values JSON>>,
    mod_type STRING,
    value_capture_type STRING,
    number_of_records_in_transaction INT64,
    number_of_partitions_in_transaction INT64,
    transaction_tag STRING,
    is_system_transaction BOOL>>,
  heartbeat_record ARRAY<STRUCT<timestamp TIMESTAMP>>,
  child_partitions_record ARRAY<STRUCT<
    start_timestamp TIMESTAMP,
    record_sequence STRING,
    child_partitions ARRAY<STRUCT<token STRING,
                                  parent_partition_tokens ARRAY<STRING>>>>>>>
```

Each element of `ChangeRecord` holds one record: one of the three inner arrays
has an element and the other two are empty.

In PostgreSQL, each row has one `JSONB` column named after the TVF, such as
`read_json_changes`. Its value is an object with one key,
`data_change_record`, `heartbeat_record`, or `child_partitions_record`. The
fields are the same as in GoogleSQL, and timestamps are strings.

### Example reader (Python, GoogleSQL)

This reader uses the `google-cloud-spanner` client. The client returns
`STRUCT` values as lists, so the code reads fields by position.

```python
from google.cloud.spanner_v1 import param_types

# Field order of the data change record STRUCT.
DATA_FIELDS = [
    "commit_timestamp", "record_sequence", "server_transaction_id",
    "is_last_record_in_transaction_in_partition", "table_name",
    "column_types", "mods", "mod_type", "value_capture_type",
    "number_of_records_in_transaction", "number_of_partitions_in_transaction",
    "transaction_tag", "is_system_transaction",
]


def query(database, stream, start, end, token):
    """Runs one change stream query and yields (kind, value) pairs."""
    sql = f"SELECT ChangeRecord FROM READ_{stream}(@start, @end, @token, 10000)"
    params = {"start": start, "end": end, "token": token}
    types = {"start": param_types.TIMESTAMP, "end": param_types.TIMESTAMP,
             "token": param_types.STRING}
    # A single-use snapshot is a strong, read-only transaction, and
    # execute_sql() streams results, as change stream queries require.
    with database.snapshot() as snapshot:
        for row in snapshot.execute_sql(sql, params=params, param_types=types):
            for data_records, heartbeats, child_records in row[0]:
                for record in data_records:
                    yield "data", dict(zip(DATA_FIELDS, record))
                for (timestamp,) in heartbeats:
                    yield "heartbeat", timestamp
                for start_ts, _sequence, children in child_records:
                    for child_token, _parents in children:
                        yield "child", (child_token, start_ts)


def read_changes(database, stream, start, end):
    """Yields the data change records committed from start to end."""
    # The initial query (NULL token) returns only partition tokens.
    pending = [value for kind, value in query(database, stream, start, end, None)
               if kind == "child"]
    seen = {token for token, _ in pending}
    while pending:
        token, token_start = pending.pop()
        for kind, value in query(database, stream, token_start, end, token):
            if kind == "data":
                yield value
            elif kind == "child" and value[0] not in seen:
                # A merged partition is reported by both of its parents.
                seen.add(value[0])
                pending.append(value)
```

Usage, with the `Users` table and `Changes` change stream from the quick
start:

```python
import os
os.environ["SPANNER_EMULATOR_HOST"] = "localhost:9010"
from google.cloud import spanner

database = spanner.Client(project="my-project").instance(
    "my-instance").database("my-database")

with database.batch() as batch:
    batch.insert("Users", ["UserId", "Name"], [(1, "Alice"), (2, "Bob")])
start = batch.committed
with database.batch() as batch:
    batch.update("Users", ["UserId", "Email"], [(1, "a@example.com")])
end = batch.committed

records = sorted(read_changes(database, "Changes", start, end),
                 key=lambda r: r["commit_timestamp"])
for record in records:
    for keys, new_values, old_values in record["mods"]:
        print(record["mod_type"], dict(keys), dict(new_values), dict(old_values))
```

Output:

```
INSERT {'UserId': '1'} {'Email': None, 'Name': 'Alice'} {}
INSERT {'UserId': '2'} {'Email': None, 'Name': 'Bob'} {}
UPDATE {'UserId': '1'} {'Email': 'a@example.com'} {'Email': None}
```

The example takes `start` and `end` from commit timestamps that the emulator
returns. Timestamps from your own machine can differ from the emulator's
clock, for example when the emulator runs in Docker.

For a PostgreSQL database, the flow is the same with these changes:

- Query `SELECT * FROM spanner.read_json_<name>($1, $2, $3, 10000)` and pass
  the parameters as `p1`, `p2`, and `p3`.
- `row[0]` is a JSON object. Check which key it has: `data_change_record`,
  `heartbeat_record`, or `child_partitions_record`.
- A child token is
  `row[0]["child_partitions_record"]["child_partitions"][i]["token"]`. Its
  start time is the string `row[0]["child_partitions_record"]["start_timestamp"]`;
  convert it with `datetime.fromisoformat()` before you pass it back as a
  `TIMESTAMP` parameter.

## Record contents

### Data change records

- **Grouping.** Within a transaction, the emulator puts consecutive changes
  into one data change record while they have the same table, the same change
  type, and the same set of tracked columns. Each change is one entry in
  `mods`. Inserting two rows in one mutation gives one record with two mods.
- **One mod per row per transaction.** Several writes to the same row in one
  transaction reach the change stream as one mod. For example, an `INSERT`
  followed by an `UPDATE` of the same row is reported as one `INSERT` with the
  final values.
- **`record_sequence`** numbers the records of one transaction for one change
  stream, starting at `00000000`.
- **`number_of_records_in_transaction`** counts this change stream's records
  for the transaction. The last one has
  `is_last_record_in_transaction_in_partition = true`.
- **Fixed fields.** `number_of_partitions_in_transaction` is always `1`,
  `transaction_tag` is always empty, and `is_system_transaction` is always
  `false`.
- **Empty values** are `{}`, not `null`.
- **`keys`** always holds the primary key columns.
- **`column_types`** lists the key columns and the tracked columns that appear
  in the record. `type` is JSON such as `{"code": "INT64"}`.
- **Untracked updates.** An `UPDATE` that changes no tracked non-key column
  produces no record. A change stream that tracks only key columns records
  inserts and deletes, not updates.
- **Value encoding.** `INT64` and `NUMERIC` values are JSON strings (`"30"`).
  `BYTES` values are base64 strings. `DATE` and `TIMESTAMP` values are
  strings. `FLOAT64` and `BOOL` values are JSON numbers and booleans. Arrays
  are JSON arrays.

### Value capture types

Inserts and deletes:

| Change | `new_values` | `old_values` |
|--------|--------------|--------------|
| `INSERT`, any type | Every tracked column. Columns the write didn't set are `null`. | `{}` |
| `DELETE`, `OLD_AND_NEW_VALUES` or `NEW_ROW_AND_OLD_VALUES` | `{}` | Every tracked column |
| `DELETE`, `NEW_VALUES` or `NEW_ROW` | `{}` | `{}` |

Updates, for a row `(1, 'John', 'old@example.com', 30)` in
`Users(UserId, Name, Email, Age)` after
`UPDATE Users SET Email = 'new@example.com' WHERE UserId = 1`:

| Type | `new_values` | `old_values` |
|------|--------------|--------------|
| `OLD_AND_NEW_VALUES` | `{"Email": "new@example.com"}` | `{"Email": "old@example.com"}` |
| `NEW_VALUES` | `{"Email": "new@example.com"}` | `{}` |
| `NEW_ROW` | `{"Age": "30", "Email": "new@example.com", "Name": "John"}` | `{}` |
| `NEW_ROW_AND_OLD_VALUES` | `{"Age": "30", "Email": "new@example.com", "Name": "John"}` | `{"Email": "old@example.com"}` |

## Mutable key range change streams

A change stream created with `partition_mode = 'MUTABLE_KEY_RANGE'` returns
records in the `google.spanner.v1.ChangeStreamRecord` proto format: data
change, heartbeat, partition start, partition event, and partition end
records.

- GoogleSQL: `READ_<name>` returns a `PROTO` column of that type.
- PostgreSQL: the TVF is `spanner.read_proto_bytes_<name>` and returns
  `BYTES`.
- `end_timestamp` is required and can be at most 30 minutes after
  `start_timestamp`.
- `partition_mode` can't be changed with `ALTER`.

## Limits

| Limit | Value |
|-------|-------|
| Change streams per database | 10 |
| Change streams tracking the same table or column (`FOR ALL` counts) | 3 |
| `retention_period` | 24 hours to 7 days |
| `heartbeat_milliseconds` | 100 to 300000 |
| `start_timestamp` | At most 10 minutes in the future |
| `end_timestamp`, mutable key range change streams | Required, at most 30 minutes after `start_timestamp` |
| Partition lifetime | 20–40 seconds by default |

## Flags

| Flag | Binary | Default | Effect |
|------|--------|---------|--------|
| `--override_change_stream_partition_token_alive_seconds=X` | `gateway_main`, `emulator_main` | `-1` (off) | If X > 0, partitions live X to 2X seconds instead of 20–40. It also sets the wait before retrying a failed partition change to X seconds. The `CHANGE_STREAM_PARTITION_TOKEN_ALIVE_SECONDS` environment variable overrides the `gateway_main` flag. |
| `--enable_change_stream_churning` | `emulator_main` only | `true` | If `false`, partitions are never replaced, and a partition query with no `end_timestamp` keeps running. |
| `--change_stream_churning_interval` | `emulator_main` only | `20s` | Minimum partition age before it's replaced. |
| `--change_stream_churn_thread_sleep_interval` | `emulator_main` only | `20s` | How often each change stream checks for partitions to replace. |
| `--change_streams_partition_query_chop_interval` | `emulator_main` only | `100ms` | Size of each time slice a partition query reads. |
| `--cloud_spanner_emulator_disable_cs_retention_check` | `emulator_main` only | `false` | Skip the 24 hours to 7 days check on `retention_period` in `CREATE` and `ALTER`. |

To use an `emulator_main`-only flag in Docker, run `emulator_main` directly.
This serves gRPC only, without the REST gateway:

```shell
docker run -p 9010:9010 jaysen2apache/spanner-emulator-extended \
  ./emulator_main --host_port=0.0.0.0:9010 --enable_change_stream_churning=false
```

## Restarts and persistence

With `--data_dir`, change stream definitions, options, records, and partition
history are stored like any other schema and data, and survive restarts. After
a restart you can read from a `start_timestamp` before the restart, and
partition tokens from before the restart stay valid.

- **Open queries fail** when the emulator stops. Run them again after it
  starts.
- **Partitions** that were active at shutdown are replaced on the first pass
  after startup, about 20 seconds later.
- **Creation times** are restored by replaying each DDL batch at its original
  commit timestamp, with two exceptions:
  - A change stream created in the `CreateDatabase` request (in
    `extra_statements`) gets a creation time of 1970-01-01 after a restart,
    because the database create time is saved as 1970-01-01. After that, a
    `start_timestamp` earlier than the change stream's real creation time
    fails with an `INTERNAL` error (`RET_CHECK failure ...
    !IsQueryResultEmpty(partition_results)`) instead of `OUT_OF_RANGE`.
    Create change streams with a separate `UpdateDatabaseDdl` request to
    avoid this.
  - Metadata written by builds before commit `b8bf6521` (2026-08-16) has no
    DDL batch timestamps. A change stream from a later DDL batch in that
    metadata gets the restart time as its creation time, so you can't read
    from before the restart.
- **No retention cleanup.** Records older than `retention_period` aren't
  deleted. They can no longer be queried, but they stay on disk until you drop
  the change stream or the database.

Without `--data_dir`, change streams and their records are lost when the
emulator stops. See [Persistence](persistence.md) for the storage layout.

## INFORMATION_SCHEMA

These views list change streams:

- `INFORMATION_SCHEMA.CHANGE_STREAMS`
- `INFORMATION_SCHEMA.CHANGE_STREAM_TABLES`
- `INFORMATION_SCHEMA.CHANGE_STREAM_COLUMNS`
- `INFORMATION_SCHEMA.CHANGE_STREAM_OPTIONS`: `retention_period` and
  `value_capture_type`, when they were set explicitly.

## Differences from production

| Difference | Impact |
|------------|--------|
| All writes go to one partition: the active token that sorts first. | You can't test how work spreads across partitions. The other partitions return only heartbeats and child partitions records. |
| Partitions are replaced on a timer (20–40 seconds), not by load. | Readers must follow child tokens often. A partition query with no end returns within about 40 seconds. |
| `REPLACE` on an existing row gives one `INSERT` record, not a `DELETE` and an `INSERT`. | Consumers see an `INSERT` for replaced rows. Columns that the `REPLACE` didn't set show as `null`. |
| Records for writes to several tables in one transaction may not follow the order of the writes. | Don't rely on record order within a transaction. |
| A DML `INSERT` writes `null` to the columns it doesn't list; production doesn't write them. | Records for rows inserted by DML can differ from production (disabled test `DISABLED_MultipleDMLVerifyDataChangeRecordContent`). |
| `transaction_tag` is always empty, `number_of_partitions_in_transaction` is always 1, and `is_system_transaction` is always `false`. | You can't test logic that uses these fields. |
| Resume tokens are placeholders, and the emulator ignores resume tokens in requests. | If a client library retries a broken change stream query, the query starts over and records can repeat. |
| `exclude_ttl_deletes` has no effect, and TTL deletions never run. | You can't test TTL filtering. |
| Setting an option to `NULL` doesn't reset it. | The old value still applies. Set an explicit value, or use `RESET` in PostgreSQL. |
| The process that replaces partitions uses read-write transactions, and the emulator runs one read-write transaction at a time. | User transactions can abort now and then. Retry them, as the client libraries do. |
| Records aren't deleted after `retention_period`. | Disk use grows with change volume. |
| After a restart, change streams created in `CreateDatabase` lose their creation time. | An early `start_timestamp` returns `INTERNAL` instead of `OUT_OF_RANGE`. See [Restarts and persistence](#restarts-and-persistence). |

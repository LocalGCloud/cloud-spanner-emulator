# Change Streams

Cloud Spanner change streams track data changes in your database. This emulator implements change streams **identically to Google Cloud Spanner** — applications using change streams against the emulator will work without modification against production Cloud Spanner.

Change streams support both **GoogleSQL** and **PostgreSQL** dialects.

For detailed concepts and use cases, see the [official Cloud Spanner change streams documentation](https://cloud.google.com/spanner/docs/change-streams).

---

## Quick Start

### 1. Start the Emulator

Start the emulator with persistence enabled to ensure change stream data survives restarts:

```bash
docker run -p 9010:9010 -p 9020:9020 \
  -v /path/to/data:/data \
  jaysen2apache/spanner-emulator-extended \
  --data_dir=/data
```

Without `--data_dir`, the emulator runs in-memory mode and change stream data is lost on restart.

### 2. Create a Change Stream

Change streams are created using DDL statements:

```sql
-- Track all tables in the database
CREATE CHANGE STREAM all_changes FOR ALL;

-- Track specific tables
CREATE CHANGE STREAM user_changes FOR Users, Orders;

-- Track specific columns only
CREATE CHANGE STREAM sensitive_data FOR Users(Email, PhoneNumber), Orders(TotalAmount);

-- Track only primary keys (no column values)
CREATE CHANGE STREAM key_changes FOR Users();

-- With options
CREATE CHANGE STREAM audit_log FOR ALL OPTIONS (
  retention_period = '7d',
  value_capture_type = 'OLD_AND_NEW_VALUES'
);
```

### 3. Query the Change Stream

Change streams are queried using table-valued functions (TVFs):

**GoogleSQL:**
```python
from google.cloud import spanner

client = spanner.Client()
instance = client.instance('my-instance')
database = instance.database('my-database')

# Query change stream from a specific timestamp
query = """
  SELECT * FROM READ_user_changes(
    '2024-01-01T00:00:00Z',  -- start_timestamp
    NULL,                     -- end_timestamp (NULL = unbounded/live)
    NULL,                     -- partition_token (NULL = initial query)
    10000                     -- heartbeat_milliseconds
  )
"""

with database.snapshot() as snapshot:
    results = snapshot.execute_sql(query)
    for row in results:
        change_record = row[0]
        # Parse change_record structure (see below)
```

**PostgreSQL:**
```python
# Same API, different TVF name
query = """
  SELECT * FROM spanner.read_json_user_changes(
    '2024-01-01T00:00:00Z',
    NULL,
    NULL,
    10000
  )
"""
```

### 4. Parse the Results

Each row contains a change record with one of three types:

- **DataChangeRecord** - actual data changes (INSERT, UPDATE, DELETE)
- **HeartbeatRecord** - sent periodically when no changes occur
- **ChildPartitionsRecord** - partition management information

See [Record Types](#record-types) below for detailed structure.

---

## DDL Reference

### CREATE CHANGE STREAM

**Syntax:**
```sql
CREATE CHANGE STREAM change_stream_name
  FOR { ALL | table_spec [, table_spec ...] }
  [OPTIONS (option = value [, ...])]
```

**Tracking Specifications:**

| Syntax | What it tracks |
|--------|----------------|
| `FOR ALL` | All tables in the database |
| `FOR Users` | All columns in the Users table |
| `FOR Users(Name, Email)` | Only Name and Email columns in Users |
| `FOR Users()` | Only primary key columns in Users |
| `FOR Users, Orders` | All columns in both Users and Orders |
| `FOR Users(Name), Orders()` | Mixed: Name column from Users, keys from Orders |

**Options:**

| Option | Type | Default | Description |
|--------|------|---------|-------------|
| `retention_period` | STRING | `'1d'` | How long to retain change records. Format: `'Nd'` (days). Tested up to `'7d'`. |
| `value_capture_type` | STRING | `'OLD_AND_NEW_VALUES'` | What data to capture. See [Value Capture Types](#value-capture-types). |
| `exclude_insert` | BOOL | `false` | Skip INSERT operations |
| `exclude_update` | BOOL | `false` | Skip UPDATE operations |
| `exclude_delete` | BOOL | `false` | Skip DELETE operations |
| `exclude_ttl_deletes` | BOOL | `false` | Skip TTL-driven deletions |
| `allow_txn_exclusion` | BOOL | `false` | Allow transaction-level exclusion |

**Examples:**

```sql
-- Audit trail with 7-day retention
CREATE CHANGE STREAM audit_trail FOR ALL OPTIONS (
  retention_period = '7d'
);

-- Track only new values (efficient for replication)
CREATE CHANGE STREAM replication_feed FOR Orders, Inventory OPTIONS (
  value_capture_type = 'NEW_VALUES'
);

-- Track updates only (skip inserts and deletes)
CREATE CHANGE STREAM update_tracker FOR Users OPTIONS (
  exclude_insert = true,
  exclude_delete = true
);
```

### ALTER CHANGE STREAM

Change the tables/columns tracked by an existing change stream:

```sql
-- Change tracking scope
ALTER CHANGE STREAM user_changes SET FOR Orders, Inventory;

-- Update options
ALTER CHANGE STREAM user_changes SET OPTIONS (
  retention_period = '3d'
);

-- Stop tracking all tables
ALTER CHANGE STREAM user_changes DROP FOR ALL;
```

### DROP CHANGE STREAM

Delete a change stream and all its data:

```sql
DROP CHANGE STREAM user_changes;
```

---

## Query Reference

### TVF Signature

**GoogleSQL:**
```sql
READ_<change_stream_name>(
  start_timestamp TIMESTAMP,
  end_timestamp TIMESTAMP,
  partition_token STRING,
  heartbeat_milliseconds INT64
)
```

**PostgreSQL:**
```sql
spanner.read_json_<change_stream_name>(
  start_timestamp TIMESTAMP,
  end_timestamp TIMESTAMP,
  partition_token STRING,
  heartbeat_milliseconds INT64
)
```

### Parameters

| Parameter | Required | Description |
|-----------|----------|-------------|
| `start_timestamp` | Yes | Start reading changes from this timestamp. Must be within retention period and not before change stream creation time. |
| `end_timestamp` | No | Stop reading at this timestamp. `NULL` means unbounded (live query). Must be >= start_timestamp. |
| `partition_token` | No | Read from a specific partition. `NULL` for initial query (returns ChildPartitionsRecord with active tokens). |
| `heartbeat_milliseconds` | Yes | Interval for heartbeat records when no data changes occur. Range: 10-60000 ms. |

### Query Patterns

**Initial Query (get partition tokens):**
```sql
-- Returns ChildPartitionsRecord with active partition tokens
SELECT * FROM READ_user_changes('2024-01-01T00:00:00Z', NULL, NULL, 10000);
```

**Partition Query (read from specific partition):**
```sql
-- Use token from ChildPartitionsRecord
SELECT * FROM READ_user_changes(
  '2024-01-01T00:00:00Z',
  NULL,
  'PARTITION_TOKEN_STRING',
  10000
);
```

**Bounded Historical Query:**
```sql
-- Query specific time range
SELECT * FROM READ_user_changes(
  '2024-01-01T00:00:00Z',
  '2024-01-07T00:00:00Z',
  NULL,
  10000
);
```

### Validation

The emulator validates:
- `start_timestamp` must be within retention period
- `start_timestamp` must not be before change stream creation time
- `start_timestamp` must not be more than 1 hour in the future
- `end_timestamp` must be >= `start_timestamp`
- `heartbeat_milliseconds` must be in range 10-60000
- Partition tokens must be valid and active

---

## Record Types

Change stream queries return one of three record types per row.

### DataChangeRecord

Contains actual data modifications (INSERT, UPDATE, DELETE).

**Structure (GoogleSQL):**
```
STRUCT<
  commit_timestamp TIMESTAMP,
  record_sequence STRING,
  transaction_id STRING,
  is_last_record_in_transaction_in_partition BOOL,
  table_name STRING,
  column_types ARRAY<STRUCT<name STRING, type JSON>>,
  mods ARRAY<STRUCT<
    keys JSON,
    new_values JSON,
    old_values JSON
  >>,
  mod_type STRING,
  value_capture_type STRING,
  number_of_records_in_transaction INT64,
  number_of_partitions_in_transaction INT64,
  transaction_tag STRING,
  is_system_transaction BOOL
>
```

**Key Fields:**
- `commit_timestamp` - When the transaction committed
- `mod_type` - `"INSERT"`, `"UPDATE"`, or `"DELETE"`
- `table_name` - Which table was modified
- `mods[].keys` - Primary key values (always included)
- `mods[].new_values` - New column values (depends on value_capture_type)
- `mods[].old_values` - Old column values (depends on value_capture_type)

### HeartbeatRecord

Sent periodically when no data changes occur, at the interval specified by `heartbeat_milliseconds`.

**Structure:**
```
STRUCT<
  timestamp TIMESTAMP
>
```

Heartbeat records confirm the change stream is active and provide a checkpoint timestamp.

### ChildPartitionsRecord

Returned by the initial query (when `partition_token` is NULL) and contains active partition tokens.

**Structure:**
```
STRUCT<
  start_timestamp TIMESTAMP,
  record_sequence STRING,
  child_partitions ARRAY<STRUCT<token STRING, parent_partition_tokens ARRAY<STRING>>>
>
```

For high-throughput scenarios, query each partition token in parallel.

---

## Value Capture Types

The `value_capture_type` option controls what data is captured in change records:

| Type | Keys | New Values | Old Values | Use Case |
|------|------|------------|------------|----------|
| `OLD_AND_NEW_VALUES` (default) | ✅ | ✅ | ✅ | Full audit trail |
| `NEW_VALUES` | ✅ | ✅ | ❌ | Replication to another system |
| `NEW_ROW` | ✅ | ✅ All columns | ❌ | Need complete row after change |
| `NEW_ROW_AND_OLD_VALUES` | ✅ | ✅ All columns | ✅ | Full audit with complete new row |

**Key Differences:**

- **`NEW_VALUES`** - Only captures modified columns in `new_values`
- **`NEW_ROW`** - Captures all columns (even unchanged) in `new_values`
- **Old values** - Only captured by `OLD_AND_NEW_VALUES` and `NEW_ROW_AND_OLD_VALUES`

**Example:**

Given table: `Users(UserId INT64, Name STRING, Email STRING, Age INT64)`

When updating only `Email`:

```sql
UPDATE Users SET Email = 'new@example.com' WHERE UserId = 1;
```

| Type | new_values | old_values |
|------|-----------|------------|
| `OLD_AND_NEW_VALUES` | `{"Email": "new@example.com"}` | `{"Email": "old@example.com"}` |
| `NEW_VALUES` | `{"Email": "new@example.com"}` | `null` |
| `NEW_ROW` | `{"Name": "John", "Email": "new@example.com", "Age": 30}` | `null` |
| `NEW_ROW_AND_OLD_VALUES` | `{"Name": "John", "Email": "new@example.com", "Age": 30}` | `{"Email": "old@example.com"}` |

---

## Persistence Integration

When running with `--data_dir`, change streams survive emulator restarts:

### What Persists

✅ **Change stream definitions** - DDL statements (CREATE CHANGE STREAM)  
✅ **Change stream options** - retention_period, value_capture_type, exclusions  
✅ **Change stream data** - Historical records within retention period  
✅ **Partition state** - Active and stale partitions  
✅ **ID counters** - Prevents change_stream_id collisions after restart  

### What Doesn't Persist

❌ **Data outside retention period** - Automatically garbage collected  
❌ **In-progress queries** - Must be restarted after emulator restart  

### Recovery Behavior

On startup with `--data_dir`:
1. Metadata loaded from `{data_dir}/metadata.json`
2. Change stream schema reconstructed from DDL
3. Change stream data recovered from LevelDB
4. Partition churner threads restarted
5. ID generator seeded to prevent collisions

### In-Memory Mode

Without `--data_dir`:
- Change streams work normally during emulator runtime
- All change stream data lost on restart
- Suitable for unit testing
- Not suitable for long-running development environments

---

## Known Limitations

The emulator has a few edge cases where behavior differs from production Cloud Spanner:

### 1. REPLACE Mutation Behavior

**Issue:** `REPLACE` mutations generate 1 change record instead of 2.

**Production Spanner:** `REPLACE` generates both DELETE and INSERT records.  
**Emulator:** `REPLACE` generates only INSERT record.

**Impact:** Minimal. Most applications use INSERT/UPDATE/DELETE directly. REPLACE is uncommon.

**Tests disabled:** `DISABLED_SingleReplaceExistingRow`, `DISABLED_ConsecutiveReplace` in `change_streams_read_write.cc`

### 2. Multi-Table Transaction Ordering

**Issue:** DataChangeRecords from multi-table transactions may not preserve commit sequence order.

**Production Spanner:** Records ordered by commit sequence within transaction.  
**Emulator:** Ordering can be disrupted by internal buffering.

**Impact:** Low. Most applications don't depend on intra-transaction record ordering.

**Test disabled:** `DISABLED_DataChangeRecordOrderForMultiTablesSameTransaction` in `change_streams_read_write.cc`

### 3. PostgreSQL NULL Options

**Issue:** Cannot set options to NULL in ALTER CHANGE STREAM.

**Example:**
```sql
-- This fails in PostgreSQL dialect
ALTER CHANGE STREAM cs SET OPTIONS (value_capture_type = null);
```

**Workaround:** Drop and recreate the change stream, or avoid clearing options.

**Impact:** Low. Rarely need to clear options to defaults.

---

## Best Practices

### Start with FOR ALL

For audit trails and replication, `FOR ALL` is simplest and captures all future tables automatically:

```sql
CREATE CHANGE STREAM audit_trail FOR ALL OPTIONS (
  retention_period = '7d'
);
```

### Use Appropriate Value Capture Type

- **Audit trails** → `OLD_AND_NEW_VALUES` (see what changed)
- **Replication** → `NEW_VALUES` (smaller records, less overhead)
- **Analytics** → `NEW_ROW` (complete row for downstream processing)

### Set Retention Based on Consumer Lag

If your consumer processes changes every hour, 1-day retention is sufficient. For weekly batch jobs, use 7-day retention:

```sql
CREATE CHANGE STREAM weekly_sync FOR Orders OPTIONS (
  retention_period = '7d'
);
```

### Query from Checkpoint Timestamps

Store the last processed `commit_timestamp` and use it as `start_timestamp` in the next query to avoid reprocessing:

```python
last_checkpoint = load_checkpoint()  # e.g., '2024-01-01T12:00:00Z'
query = f"SELECT * FROM READ_changes('{last_checkpoint}', NULL, NULL, 10000)"
```

### Handle All Record Types

Your consumer should handle DataChangeRecord, HeartbeatRecord, and ChildPartitionsRecord:

```python
for row in results:
    record = row[0]
    if 'data_change_record' in record:
        # Process data change
        pass
    elif 'heartbeat_record' in record:
        # Update checkpoint, confirm liveness
        pass
    elif 'child_partitions_record' in record:
        # Store partition tokens for parallel processing
        pass
```

### Use Partitions for High Throughput

For production workloads with high change volume, query each partition in parallel:

1. Initial query to get partition tokens
2. Spawn workers to query each partition
3. Merge results ordered by commit_timestamp

See [change-streams-examples.md](./change-streams-examples.md) for implementation.

---

## Next Steps

- **Examples:** See [change-streams-examples.md](./change-streams-examples.md) for complete working code
- **Architecture:** See [change-streams-architecture.md](./change-streams-architecture.md) for implementation details
- **Official Docs:** [Cloud Spanner Change Streams](https://cloud.google.com/spanner/docs/change-streams)

# Change Streams Architecture

Technical reference for the change streams implementation in the Cloud Spanner Emulator.

---

## Architecture Overview

Change streams are implemented across multiple layers of the emulator stack:

```
┌─────────────────────────────────────────────────────────────────┐
│                         Frontend Layer                          │
│  ┌──────────────────┐  ┌─────────────────┐  ┌───────────────┐ │
│  │ change_streams.cc│  │ ExecuteStreamingSql │  │ converters/  │ │
│  │  (handlers)      │──│   API endpoint    │──│ change_streams│ │
│  └──────────────────┘  └─────────────────┘  └───────────────┘ │
└─────────────────────────────────────────────────────────────────┘
                                ▼
┌─────────────────────────────────────────────────────────────────┐
│                         Query Layer                             │
│  ┌──────────────────┐  ┌─────────────────┐  ┌───────────────┐ │
│  │ queryable_change │  │ change_stream_  │  │ TVF resolution│ │
│  │   _stream_tvf.cc │──│ query_validator │──│  & validation │ │
│  └──────────────────┘  └─────────────────┘  └───────────────┘ │
└─────────────────────────────────────────────────────────────────┘
                                ▼
┌─────────────────────────────────────────────────────────────────┐
│                         Schema Layer                            │
│  ┌──────────────────┐  ┌─────────────────┐  ┌───────────────┐ │
│  │ change_stream.h  │  │ change_stream_  │  │ change_stream_│ │
│  │  (catalog)       │──│   validator.cc  │──│   builder.h   │ │
│  └──────────────────┘  └─────────────────┘  └───────────────┘ │
└─────────────────────────────────────────────────────────────────┘
                                ▼
┌─────────────────────────────────────────────────────────────────┐
│                        Action Layer                             │
│  ┌──────────────────────────────────────────────────────────┐  │
│  │  change_stream.cc (833 lines)                            │  │
│  │  - Generates DataChangeRecords from mutations            │  │
│  │  - Applies value_capture_type rules                      │  │
│  │  - Applies exclusion filters                             │  │
│  │  - Writes to _cs_<name>_data table                       │  │
│  └──────────────────────────────────────────────────────────┘  │
└─────────────────────────────────────────────────────────────────┘
                                ▼
┌─────────────────────────────────────────────────────────────────┐
│                       Database Layer                            │
│  ┌──────────────────────────────────────────────────────────┐  │
│  │  change_stream_partition_churner.cc                      │  │
│  │  - Background thread per change stream                   │  │
│  │  - Splits/merges/moves partitions                        │  │
│  │  - Manages partition lifecycle                           │  │
│  └──────────────────────────────────────────────────────────┘  │
└─────────────────────────────────────────────────────────────────┘
                                ▼
┌─────────────────────────────────────────────────────────────────┐
│                        Storage Layer                            │
│  ┌──────────────────┐  ┌─────────────────┐  ┌───────────────┐ │
│  │ _cs_<name>_data  │  │_cs_<name>_      │  │ LevelDB       │ │
│  │   (change records)│  │  partition      │──│ (persistent)  │ │
│  └──────────────────┘  └─────────────────┘  └───────────────┘ │
└─────────────────────────────────────────────────────────────────┘
```

### Key Source Files

| Component | Location | Purpose |
|-----------|----------|---------|
| **Catalog** | `backend/schema/catalog/change_stream.{h,cc}` | Schema representation of change streams |
| **Validator** | `backend/schema/validators/change_stream_validator.cc` | DDL validation |
| **Builder** | `backend/schema/builders/change_stream_builder.h` | Schema construction |
| **Query** | `backend/query/change_stream/queryable_change_stream_tvf.{h,cc}` | TVF resolution |
| **Query Validator** | `backend/query/change_stream/change_stream_query_validator.{h,cc}` | Query parameter validation |
| **Actions** | `backend/actions/change_stream.{h,cc}` | Record generation from mutations |
| **Partition Churner** | `backend/database/change_stream/change_stream_partition_churner.{h,cc}` | Background partition management |
| **Handlers** | `frontend/handlers/change_streams.{h,cc}` | Streaming API implementation |
| **Converters** | `frontend/converters/change_streams.{h,cc}` | STRUCT/JSON format conversion |

---

## Internal Tables

Change streams create two internal tables for each stream:

### Data Table: `_cs_<change_stream_name>_data`

Stores the actual change records.

**Schema (simplified):**
```
_cs_<name>_data (
  partition_token STRING,
  commit_timestamp TIMESTAMP,
  server_transaction_id STRING,
  record_sequence STRING,
  is_last_record_in_transaction_in_partition BOOL,
  table_name STRING,
  column_types_name ARRAY<STRING>,
  column_types_type ARRAY<STRING>,
  column_types_is_primary_key ARRAY<BOOL>,
  column_types_ordinal_position ARRAY<INT64>,
  mods_keys STRING,
  mods_old_values STRING,
  mods_new_values STRING,
  mod_type STRING,
  value_capture_type STRING,
  number_of_records_in_transaction INT64,
  number_of_partitions_in_transaction INT64,
  transaction_tag STRING,
  is_system_transaction BOOL
) PRIMARY KEY (partition_token, commit_timestamp, record_sequence)
```

**Key Fields:**
- **partition_token** - Which partition this record belongs to
- **commit_timestamp** - When the transaction committed (microsecond precision)
- **record_sequence** - Unique sequence within transaction for ordering
- **mods_keys / mods_old_values / mods_new_values** - JSON-encoded change data
- **mod_type** - `INSERT`, `UPDATE`, or `DELETE`

**Access Pattern:**
- Scanned by partition_token prefix
- Filtered by commit_timestamp range
- Ordered by record_sequence within transaction

### Partition Table: `_cs_<change_stream_name>_partition`

Stores partition metadata for partition management.

**Schema:**
```
_cs_<name>_partition (
  partition_token STRING,
  start_time TIMESTAMP,
  end_time TIMESTAMP,
  parents ARRAY<STRING>,
  children ARRAY<STRING>
) PRIMARY KEY (partition_token)
```

**Key Fields:**
- **partition_token** - Unique identifier for this partition
- **start_time** - When this partition became active
- **end_time** - When this partition became stale (NULL if still active)
- **parents** - Parent partition tokens (for split/merge tracking)
- **children** - Child partition tokens (for split/merge tracking)

**Partition Lifecycle:**
1. **Created** - partition_token generated, start_time set, end_time = NULL
2. **Active** - Used for storing new change records
3. **Stale** - end_time set, no longer accepting new records
4. **Split/Merge** - parents/children relationships tracked

**Note:** These tables are internal implementation details. Clients should never query them directly; always use the TVF (`READ_<name>` or `spanner.read_json_<name>`).

---

## How Records Are Generated

Change records are created during transaction commit:

### 1. Transaction Commit Flow

```
User mutation
    ↓
TransactionStore::BufferWriteOp()
    ↓
Transaction::Commit()
    ↓
ChangeStreamActions::GenerateDataChangeRecords()
    ↓
Write to _cs_<name>_data table
```

### 2. Record Generation Logic

**Source:** `backend/actions/change_stream.cc` (833 lines)

For each mutation in the transaction:
1. Check if table is tracked by any change stream
2. For each applicable change stream:
   - Check if column is tracked (if stream tracks specific columns)
   - Apply exclusion filters (exclude_insert, exclude_update, exclude_delete)
   - Apply value_capture_type rules:
     - `OLD_AND_NEW_VALUES` → capture modified columns in both old/new
     - `NEW_VALUES` → capture modified columns in new only
     - `NEW_ROW` → capture all columns in new (even unchanged)
     - `NEW_ROW_AND_OLD_VALUES` → capture all columns in new + modified columns in old
   - Generate mods JSON:
     - `keys` → always include primary key columns
     - `new_values` → based on value_capture_type
     - `old_values` → based on value_capture_type
   - Assign record_sequence (unique within transaction)
   - Write DataChangeRecord to `_cs_<name>_data` table

### 3. Sequence Numbering

Record sequences ensure ordering within transactions:

```
Transaction commits with 3 mutations → 3 DataChangeRecords

Record 1: record_sequence = "00000001"
Record 2: record_sequence = "00000002"
Record 3: record_sequence = "00000003"
```

Sequences are zero-padded strings for lexicographic sorting.

### 4. Value Capture Examples

Given table `Users(UserId INT64, Name STRING, Email STRING, Age INT64)`

**UPDATE** statement: `UPDATE Users SET Email = 'new@example.com' WHERE UserId = 1`

| value_capture_type | keys | new_values | old_values |
|--------------------|------|------------|------------|
| `OLD_AND_NEW_VALUES` | `{UserId: 1}` | `{Email: "new@example.com"}` | `{Email: "old@example.com"}` |
| `NEW_VALUES` | `{UserId: 1}` | `{Email: "new@example.com"}` | `null` |
| `NEW_ROW` | `{UserId: 1}` | `{Name: "Alice", Email: "new@example.com", Age: 30}` | `null` |
| `NEW_ROW_AND_OLD_VALUES` | `{UserId: 1}` | `{Name: "Alice", Email: "new@example.com", Age: 30}` | `{Email: "old@example.com"}` |

---

## Partition Management

Each change stream has a background thread managing partitions.

### Partition Churner

**Source:** `backend/database/change_stream/change_stream_partition_churner.{h,cc}`

**Responsibilities:**
- Automatically create initial partition on change stream creation
- Periodically split partitions when they grow too large
- Merge partitions when they become too small
- Move partition boundaries based on key distribution
- Update partition_table with parent/child relationships
- Set end_time on stale partitions

**Threading Model:**
- One thread per change stream
- Runs in background during emulator lifetime
- Configurable churning interval (default: 10 seconds)

### Initial Query Pattern

When `partition_token = NULL`, the TVF returns `ChildPartitionsRecord` containing active partition tokens:

```python
# Phase 1: Initial query
query = "SELECT * FROM READ_changes('2024-01-01T00:00:00Z', NULL, NULL, 10000)"

# Returns:
ChildPartitionsRecord {
  start_timestamp: "2024-01-01T00:00:00Z",
  child_partitions: [
    {token: "PARTITION_001", parent_partition_tokens: []},
    {token: "PARTITION_002", parent_partition_tokens: []},
    ...
  ]
}

# Phase 2: Query each partition
for token in partition_tokens:
    query = f"SELECT * FROM READ_changes('...', NULL, '{token}', 10000)"
    # Returns DataChangeRecords from this partition only
```

### Partition Lifecycle States

```
┌────────────┐   ChurnPartitions()   ┌────────────┐
│   Created  │──────────────────────→│   Active   │
│ end_time=  │                       │ end_time=  │
│   NULL     │                       │   NULL     │
└────────────┘                       └────────────┘
                                            │
                                            │ Split/Merge
                                            ↓
                                     ┌────────────┐
                                     │   Stale    │
                                     │ end_time   │
                                     │   SET      │
                                     └────────────┘
```

**Active partitions:** Accept new change records  
**Stale partitions:** Frozen, can still be queried for historical data within retention period

### Known Issue

**Source:** `change_stream_partition_churner.cc:155`

```cpp
// TODO: Change stream churn transactions can potentially cause
// aborts in user transactions (needs optimization)
```

Partition churning uses transactions that can conflict with user transactions. This is a known limitation; production Spanner has optimizations to minimize this.

---

## Persistence Integration

Change streams are fully integrated with the emulator's persistent storage layer.

### What Persists

When running with `--data_dir`:

#### 1. Metadata (`metadata.json`)

**Location:** `{data_dir}/metadata.json`

**Content:**
```json
{
  "databases": [
    {
      "name": "my-database",
      "ddl_statements": [
        "CREATE TABLE Users (...)",
        "CREATE CHANGE STREAM audit_trail FOR ALL OPTIONS (retention_period = '7d')"
      ],
      "change_streams": [
        {
          "name": "audit_trail",
          "creation_time": "2024-01-01T00:00:00.000000Z",
          "options": {
            "retention_period": "7d",
            "value_capture_type": "OLD_AND_NEW_VALUES"
          }
        }
      ]
    }
  ]
}
```

**Persistence Flow:**
- **Write:** Atomic write-tmp-then-rename pattern (crash-safe)
- **Read:** Once on startup to reconstruct schema

#### 2. Change Stream Data (LevelDB)

**Location:** `{data_dir}/leveldb/`

**Key Format:**
```
┌─────────────────┬──────────┬───────────────────┬────────────┬─────────────────┬──────────┬──────────────┐
│ table_id_len:4  │ table_id │ encoded_key_len:4 │ encod'd_key│ column_id_len:4  │ column_id│ timestamp:8  │
└─────────────────┴──────────┴───────────────────┴────────────┴─────────────────┴──────────┴──────────────┘
```

**Change stream tables:**
- `_cs_<name>_data` → table_id assigned by UniqueIdGenerator
- `_cs_<name>_partition` → separate table_id

**Multi-Version Storage:**
- Each write gets commit_timestamp (microsecond precision)
- Historical versions preserved within retention period
- Garbage collection removes data outside retention window

#### 3. ID Counters

**Source:** `backend/common/ids.h`

The `change_stream_id` counter is persisted to prevent ID collisions after restart:

```json
{
  "id_counters": {
    "table_id": 42,
    "column_id": 128,
    "change_stream_id": 5,  ← Prevents reusing IDs
    "sequence_id": 10,
    "named_schema_id": 2
  }
}
```

**Why this matters:**
- Each change stream gets unique internal ID
- After restart, new change streams must not reuse IDs
- Prevents data corruption in LevelDB (ID used in table_id field)

### Recovery on Startup

When emulator starts with `--data_dir`:

1. **Load metadata** from `{data_dir}/metadata.json`
2. **Recreate schema** from DDL statements
3. **Seed ID generators** from persisted counters
4. **Open LevelDB** connection to `{data_dir}/leveldb/`
5. **Backfill change streams:**
   - Change stream definitions reconstructed from schema
   - Internal tables (`_cs_<name>_data`, `_cs_<name>_partition`) mapped to LevelDB
   - Historical data accessible via queries
6. **Start partition churner threads** for each change stream

**Result:** Change streams work as if emulator never restarted. Queries can access historical data within retention period.

### Garbage Collection

**Trigger:** Periodic background task (runs every retention_period / 10)

**Process:**
1. For each change stream:
   - Calculate cutoff timestamp: `now() - retention_period`
   - Delete records from `_cs_<name>_data` where `commit_timestamp < cutoff`
   - Delete stale partitions from `_cs_<name>_partition` where `end_time < cutoff`

**LevelDB Impact:**
- Deleted keys remain in LSM tree until compaction
- LevelDB compaction merges levels and removes tombstones
- Disk space reclaimed asynchronously

---

## Implementation Status

### ✅ Fully Implemented

| Feature | Status | Notes |
|---------|--------|-------|
| CREATE CHANGE STREAM | ✅ | All tracking modes (FOR ALL, specific tables, specific columns) |
| ALTER CHANGE STREAM | ✅ | SET FOR, DROP FOR ALL, SET OPTIONS |
| DROP CHANGE STREAM | ✅ | Removes stream and all data |
| value_capture_type | ✅ | All 4 types: OLD_AND_NEW_VALUES, NEW_VALUES, NEW_ROW, NEW_ROW_AND_OLD_VALUES |
| Exclusion filters | ✅ | exclude_insert, exclude_update, exclude_delete, exclude_ttl_deletes, allow_txn_exclusion |
| retention_period | ✅ | Default 1d, tested up to 7d, parsed in seconds |
| TVF queries | ✅ | Both GoogleSQL (READ_<name>) and PostgreSQL (spanner.read_json_<name>) |
| Partition management | ✅ | Automatic churning, split/merge/move operations |
| DataChangeRecord | ✅ | Full metadata: commit_timestamp, transaction_id, record_sequence, mods, etc. |
| HeartbeatRecord | ✅ | Sent at heartbeat_milliseconds interval |
| ChildPartitionsRecord | ✅ | Initial query returns active partition tokens |
| Query validation | ✅ | Timestamp ranges, retention window, heartbeat interval, partition token lifecycle |
| Both dialects | ✅ | GoogleSQL and PostgreSQL fully supported |
| Persistence | ✅ | Metadata + data + ID counters survive restarts |

### ⚠️ Known Limitations

| Issue | Impact | Workaround |
|-------|--------|------------|
| REPLACE mutation semantics | Generates 1 record instead of 2 (DELETE + INSERT) | Use explicit DELETE + INSERT instead of REPLACE |
| Multi-table transaction ordering | Records within transaction may not preserve commit sequence order | Don't depend on strict ordering within transaction |
| PostgreSQL NULL options | Cannot set options to NULL in ALTER CHANGE STREAM | Drop and recreate stream, or avoid clearing options |
| Partition churning aborts | Churner transactions can abort user transactions | Rare in practice; retry logic handles it |

**Tests Disabled:**
- `DISABLED_SingleReplaceExistingRow` (line 3425)
- `DISABLED_ConsecutiveReplace` (line 3606)
- `DISABLED_DataChangeRecordOrderForMultiTablesSameTransaction` (line 3924)

---

## Test Coverage

Change streams have extensive test coverage across multiple test suites:

### Main Test Files

| Test File | Lines | Coverage |
|-----------|-------|----------|
| `tests/conformance/cases/change_streams_read_write.cc` | 4884 | Core functionality, all value capture types, all modification types, scalar types, arrays, multi-table, interleaved tables |
| `tests/conformance/cases/change_streams_exclusion.cc` | ~500 | Exclusion filters (exclude_insert, exclude_update, exclude_delete) |
| `tests/conformance/cases/pg_change_streams_read_write.cc` | ~800 | PostgreSQL dialect-specific tests |
| `backend/schema/updater/schema_updater_tests/change_stream_test.cc` | ~600 | DDL operations (CREATE, ALTER, DROP) |
| `backend/schema/backfills/change_stream_backfill_test.cc` | ~400 | Partition initialization and backfilling |
| `backend/database/change_stream/change_stream_partition_churner_test.cc` | ~300 | Partition churner logic |
| `backend/query/change_stream/change_stream_query_validator_test.cc` | ~200 | Query parameter validation |

### Conformance Tests

**Location:** `tests/conformance/data/schema_changes/`

- `change_streams.test` - GoogleSQL DDL parsing (697 lines)
- `pg/ddl.create_change_stream.test` - PostgreSQL CREATE syntax
- `pg/ddl.alter_change_stream.test` - PostgreSQL ALTER syntax
- `pg/ddl.drop_change_stream.test` - PostgreSQL DROP syntax

### Test Scenarios Covered

✅ All value capture types  
✅ All modification types (INSERT, UPDATE, DELETE)  
✅ All scalar types (INT64, STRING, BOOL, FLOAT64, NUMERIC, TIMESTAMP, DATE, JSON, BYTES)  
✅ Array types  
✅ Multi-table transactions  
✅ Generated columns (excluded from tracking)  
✅ Interleaved tables  
✅ Column-level tracking  
✅ Key-only tracking  
✅ Exclusion filters  
✅ Retention period validation  
✅ Timestamp range validation  
✅ Heartbeat records  
✅ Partition token lifecycle  
✅ Both dialects  

---

## Comparison with Production Spanner

### Identical Behavior

✅ DDL syntax and semantics  
✅ TVF signature and parameters  
✅ Record structure and format  
✅ Value capture type behavior  
✅ Exclusion filter logic  
✅ Retention period enforcement  
✅ Partition-based reading model  
✅ Heartbeat mechanism  

### Implementation Differences

| Aspect | Production Spanner | Emulator |
|--------|-------------------|----------|
| Storage | Distributed across many servers | Local LevelDB |
| Partition count | Dynamic, scales with load | Fixed small number |
| Partition churning | Optimized to minimize aborts | Can cause occasional aborts |
| Performance | Production-grade throughput | Suitable for testing/development |
| REPLACE semantics | Generates DELETE + INSERT | Generates INSERT only |
| Multi-table txn ordering | Strictly preserved | May be disrupted |

### When to Use Emulator vs Production

**Use Emulator for:**
- Local development and unit testing
- CI/CD integration tests
- Learning change streams API
- Prototyping change stream consumers
- Cost-free experimentation

**Use Production for:**
- Production workloads
- Performance testing
- Scale testing (high partition counts)
- Benchmarking

---

## Next Steps

- **User Guide:** [change-streams.md](./change-streams.md) for API reference and quick start
- **Examples:** [change-streams-examples.md](./change-streams-examples.md) for practical code patterns
- **Official Docs:** [Cloud Spanner Change Streams](https://cloud.google.com/spanner/docs/change-streams)

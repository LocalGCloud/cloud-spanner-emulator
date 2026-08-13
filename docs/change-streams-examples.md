# Change Streams Examples

Practical examples and common patterns for using change streams in the Cloud Spanner Emulator.

---

## Complete Example: Audit Trail

This example shows end-to-end usage: create tables, create a change stream, make changes, and query the audit trail.

### Setup

```sql
-- Create a users table
CREATE TABLE Users (
  UserId INT64 NOT NULL,
  Name STRING(100),
  Email STRING(100),
  CreatedAt TIMESTAMP NOT NULL OPTIONS (allow_commit_timestamp=true),
) PRIMARY KEY (UserId);

-- Create change stream with 7-day retention for compliance
CREATE CHANGE STREAM audit_trail FOR ALL OPTIONS (
  retention_period = '7d',
  value_capture_type = 'OLD_AND_NEW_VALUES'
);
```

### Make Changes

```python
from google.cloud import spanner

client = spanner.Client()
instance = client.instance('my-instance')
database = instance.database('my-database')

# Insert a user
with database.batch() as batch:
    batch.insert(
        table='Users',
        columns=['UserId', 'Name', 'Email', 'CreatedAt'],
        values=[(1, 'Alice', 'alice@example.com', spanner.COMMIT_TIMESTAMP)]
    )

# Update the user
with database.batch() as batch:
    batch.update(
        table='Users',
        columns=['UserId', 'Email'],
        values=[(1, 'alice.new@example.com')]
    )

# Delete the user
with database.batch() as batch:
    batch.delete(
        table='Users',
        keyset=spanner.KeySet(keys=[(1,)])
    )
```

### Query the Audit Trail

```python
import datetime

# Query changes from 1 hour ago to now
start_time = datetime.datetime.utcnow() - datetime.timedelta(hours=1)
start_timestamp = start_time.strftime('%Y-%m-%dT%H:%M:%S.%fZ')

query = f"""
  SELECT * FROM READ_audit_trail(
    '{start_timestamp}',
    NULL,
    NULL,
    10000
  )
"""

with database.snapshot() as snapshot:
    results = snapshot.execute_sql(query)
    
    for row in results:
        change_record = row[0]
        
        # Check record type
        if change_record.get('data_change_record'):
            dcr = change_record['data_change_record']
            
            print(f"Timestamp: {dcr['commit_timestamp']}")
            print(f"Table: {dcr['table_name']}")
            print(f"Operation: {dcr['mod_type']}")
            
            for mod in dcr['mods']:
                print(f"  Keys: {mod['keys']}")
                
                if mod.get('new_values'):
                    print(f"  New values: {mod['new_values']}")
                
                if mod.get('old_values'):
                    print(f"  Old values: {mod['old_values']}")
            
            print()
        
        elif change_record.get('heartbeat_record'):
            hb = change_record['heartbeat_record']
            print(f"Heartbeat at {hb['timestamp']}")
        
        elif change_record.get('child_partitions_record'):
            cp = change_record['child_partitions_record']
            print(f"Partitions: {len(cp['child_partitions'])} active")
```

### Expected Output

```
Partitions: 1 active
Timestamp: 2024-01-01T10:00:00.123456Z
Table: Users
Operation: INSERT
  Keys: {"UserId": "1"}
  New values: {"Name": "Alice", "Email": "alice@example.com"}
  Old values: null

Timestamp: 2024-01-01T10:00:15.789012Z
Table: Users
Operation: UPDATE
  Keys: {"UserId": "1"}
  New values: {"Email": "alice.new@example.com"}
  Old values: {"Email": "alice@example.com"}

Timestamp: 2024-01-01T10:00:30.345678Z
Table: Users
Operation: DELETE
  Keys: {"UserId": "1"}
  New values: null
  Old values: {"Name": "Alice", "Email": "alice.new@example.com"}

Heartbeat at 2024-01-01T10:00:40.000000Z
```

---

## Pattern: Replication to Another System

Track specific tables and stream changes to a downstream system (e.g., Elasticsearch, BigQuery).

### Setup

```sql
CREATE TABLE Orders (
  OrderId INT64 NOT NULL,
  CustomerId INT64,
  TotalAmount FLOAT64,
  Status STRING(50),
) PRIMARY KEY (OrderId);

CREATE TABLE Inventory (
  ProductId INT64 NOT NULL,
  Quantity INT64,
  LastUpdated TIMESTAMP,
) PRIMARY KEY (ProductId);

-- Track only these tables, capture new values only
CREATE CHANGE STREAM replication_feed FOR Orders, Inventory OPTIONS (
  value_capture_type = 'NEW_VALUES'
);
```

### Stream Changes

```python
def stream_to_downstream():
    """Continuously stream changes to downstream system."""
    
    # Load last checkpoint (stored in downstream system)
    last_checkpoint = load_checkpoint()  # e.g., '2024-01-01T00:00:00Z'
    
    query = f"""
      SELECT * FROM READ_replication_feed(
        '{last_checkpoint}',
        NULL,  -- unbounded query (live streaming)
        NULL,
        10000  -- 10 second heartbeats
      )
    """
    
    with database.snapshot() as snapshot:
        results = snapshot.execute_sql(query)
        
        for row in results:
            change_record = row[0]
            
            if change_record.get('data_change_record'):
                dcr = change_record['data_change_record']
                
                # Send to downstream system
                for mod in dcr['mods']:
                    document = {
                        'table': dcr['table_name'],
                        'operation': dcr['mod_type'],
                        'timestamp': dcr['commit_timestamp'],
                        'id': mod['keys'],
                        'data': mod.get('new_values', {})
                    }
                    
                    # Push to downstream (e.g., Kafka, Pub/Sub)
                    send_to_downstream(document)
                
                # Update checkpoint
                save_checkpoint(dcr['commit_timestamp'])
            
            elif change_record.get('heartbeat_record'):
                # Heartbeat confirms we're up to date
                hb = change_record['heartbeat_record']
                save_checkpoint(hb['timestamp'])
                log.info(f"Caught up to {hb['timestamp']}")
```

---

## Pattern: Analytics (Column Subset)

Track only sensitive columns for compliance analytics.

### Setup

```sql
CREATE TABLE Users (
  UserId INT64 NOT NULL,
  Name STRING(100),
  Email STRING(100),
  PhoneNumber STRING(20),
  Address STRING(500),
) PRIMARY KEY (UserId);

-- Track only PII columns
CREATE CHANGE STREAM pii_changes FOR Users(Email, PhoneNumber) OPTIONS (
  retention_period = '7d'
);
```

### Query for Compliance Report

```python
def generate_pii_access_report(start_date, end_date):
    """Generate report of all PII changes in date range."""
    
    query = f"""
      SELECT * FROM READ_pii_changes(
        '{start_date}',
        '{end_date}',
        NULL,
        10000
      )
    """
    
    pii_changes = []
    
    with database.snapshot() as snapshot:
        results = snapshot.execute_sql(query)
        
        for row in results:
            change_record = row[0]
            
            if change_record.get('data_change_record'):
                dcr = change_record['data_change_record']
                
                for mod in dcr['mods']:
                    pii_changes.append({
                        'timestamp': dcr['commit_timestamp'],
                        'user_id': mod['keys']['UserId'],
                        'operation': dcr['mod_type'],
                        'changed_fields': list(mod.get('new_values', {}).keys()),
                        'transaction_id': dcr['transaction_id']
                    })
    
    return pii_changes

# Generate weekly report
import datetime
end = datetime.datetime.utcnow()
start = end - datetime.timedelta(days=7)

report = generate_pii_access_report(
    start.strftime('%Y-%m-%dT%H:%M:%SZ'),
    end.strftime('%Y-%m-%dT%H:%M:%SZ')
)

print(f"Total PII changes: {len(report)}")
for change in report:
    print(f"{change['timestamp']}: User {change['user_id']} - {change['operation']} {change['changed_fields']}")
```

---

## Pattern: Bounded Historical Query

Query a specific time range for backfilling analytics or debugging.

### Query Last Week's Changes

```python
def backfill_last_week():
    """Backfill analytics from last week's changes."""
    
    import datetime
    
    end = datetime.datetime.utcnow()
    start = end - datetime.timedelta(days=7)
    
    query = f"""
      SELECT * FROM READ_audit_trail(
        '{start.strftime('%Y-%m-%dT%H:%M:%SZ')}',
        '{end.strftime('%Y-%m-%dT%H:%M:%SZ')}',  -- bounded query
        NULL,
        10000
      )
    """
    
    changes_by_table = {}
    
    with database.snapshot() as snapshot:
        results = snapshot.execute_sql(query)
        
        for row in results:
            change_record = row[0]
            
            if change_record.get('data_change_record'):
                dcr = change_record['data_change_record']
                table = dcr['table_name']
                
                if table not in changes_by_table:
                    changes_by_table[table] = {'INSERT': 0, 'UPDATE': 0, 'DELETE': 0}
                
                changes_by_table[table][dcr['mod_type']] += len(dcr['mods'])
    
    # Print summary
    print("Changes last 7 days:")
    for table, counts in changes_by_table.items():
        print(f"  {table}:")
        print(f"    INSERTs: {counts['INSERT']}")
        print(f"    UPDATEs: {counts['UPDATE']}")
        print(f"    DELETEs: {counts['DELETE']}")

backfill_last_week()
```

---

## Pattern: Partition-Based Reading

For high-throughput scenarios, query each partition in parallel.

### Two-Phase Query

```python
import concurrent.futures
from datetime import datetime, timedelta

def get_partition_tokens(change_stream_name, start_timestamp):
    """Phase 1: Get active partition tokens."""
    
    query = f"""
      SELECT * FROM READ_{change_stream_name}(
        '{start_timestamp}',
        NULL,
        NULL,  -- NULL partition_token returns ChildPartitionsRecord
        10000
      )
    """
    
    tokens = []
    
    with database.snapshot() as snapshot:
        results = snapshot.execute_sql(query)
        
        for row in results:
            change_record = row[0]
            
            if change_record.get('child_partitions_record'):
                cp = change_record['child_partitions_record']
                for partition in cp['child_partitions']:
                    tokens.append(partition['token'])
    
    return tokens


def query_partition(change_stream_name, start_timestamp, end_timestamp, token):
    """Phase 2: Query a specific partition."""
    
    query = f"""
      SELECT * FROM READ_{change_stream_name}(
        '{start_timestamp}',
        '{end_timestamp}',
        '{token}',  -- specific partition token
        10000
      )
    """
    
    partition_changes = []
    
    with database.snapshot() as snapshot:
        results = snapshot.execute_sql(query)
        
        for row in results:
            change_record = row[0]
            
            if change_record.get('data_change_record'):
                partition_changes.append(change_record['data_change_record'])
    
    return partition_changes


def parallel_read_change_stream(change_stream_name, hours_back=1):
    """Read change stream in parallel across all partitions."""
    
    # Calculate time range
    end = datetime.utcnow()
    start = end - timedelta(hours=hours_back)
    
    start_ts = start.strftime('%Y-%m-%dT%H:%M:%SZ')
    end_ts = end.strftime('%Y-%m-%dT%H:%M:%SZ')
    
    # Phase 1: Get partition tokens
    print("Getting partition tokens...")
    tokens = get_partition_tokens(change_stream_name, start_ts)
    print(f"Found {len(tokens)} partitions")
    
    # Phase 2: Query each partition in parallel
    print("Querying partitions in parallel...")
    all_changes = []
    
    with concurrent.futures.ThreadPoolExecutor(max_workers=len(tokens)) as executor:
        futures = [
            executor.submit(query_partition, change_stream_name, start_ts, end_ts, token)
            for token in tokens
        ]
        
        for future in concurrent.futures.as_completed(futures):
            partition_changes = future.result()
            all_changes.extend(partition_changes)
    
    # Sort by commit timestamp
    all_changes.sort(key=lambda x: x['commit_timestamp'])
    
    print(f"Total changes: {len(all_changes)}")
    return all_changes


# Example usage
changes = parallel_read_change_stream('audit_trail', hours_back=24)
```

---

## PostgreSQL Dialect Example

Change streams work identically in PostgreSQL dialect with minor syntax differences.

### Setup (PostgreSQL)

```sql
-- Table creation (PostgreSQL syntax)
CREATE TABLE users (
  user_id BIGINT NOT NULL PRIMARY KEY,
  name VARCHAR(100),
  email VARCHAR(100),
  created_at TIMESTAMPTZ
);

-- Change stream (same syntax as GoogleSQL)
CREATE CHANGE STREAM audit_trail FOR ALL OPTIONS (
  retention_period = '7d',
  value_capture_type = 'OLD_AND_NEW_VALUES'
);
```

### Query (PostgreSQL)

```python
# Use spanner.read_json_<name> instead of READ_<name>
query = f"""
  SELECT * FROM spanner.read_json_audit_trail(
    '{start_timestamp}',
    NULL,
    NULL,
    10000
  )
"""

with database.snapshot() as snapshot:
    results = snapshot.execute_sql(query)
    
    for row in results:
        # Result is JSON (not STRUCT)
        change_record = row[0]
        
        if 'data_change_record' in change_record:
            dcr = change_record['data_change_record']
            
            # Parse JSON format
            print(f"Timestamp: {dcr['commit_timestamp']}")
            print(f"Table: {dcr['table_name']}")
            print(f"Operation: {dcr['mod_type']}")
            
            for mod in dcr['mods']:
                print(f"  Keys: {mod['keys']}")
                print(f"  New: {mod.get('new_values')}")
                print(f"  Old: {mod.get('old_values')}")
```

### Key Differences

| Aspect | GoogleSQL | PostgreSQL |
|--------|-----------|------------|
| DDL Syntax | Same | Same |
| TVF Name | `READ_<name>` | `spanner.read_json_<name>` |
| Result Format | STRUCT | JSON (string) |
| Parsing | Access struct fields | Parse JSON |

---

## Working with Both Dialects

You can create databases in either dialect; change stream functionality is identical.

### Side-by-Side Comparison

```python
# GoogleSQL database
def query_googlesql_change_stream(database):
    query = "SELECT * FROM READ_changes('2024-01-01T00:00:00Z', NULL, NULL, 10000)"
    
    with database.snapshot() as snapshot:
        results = snapshot.execute_sql(query)
        for row in results:
            change_record = row[0]  # STRUCT type
            if change_record.get('data_change_record'):
                process_struct(change_record['data_change_record'])


# PostgreSQL database
def query_postgresql_change_stream(database):
    query = "SELECT * FROM spanner.read_json_changes('2024-01-01T00:00:00Z', NULL, NULL, 10000)"
    
    with database.snapshot() as snapshot:
        results = snapshot.execute_sql(query)
        for row in results:
            change_record = row[0]  # JSON string
            if 'data_change_record' in change_record:
                process_json(change_record['data_change_record'])
```

### Unified Parser

```python
def parse_change_record(change_record, dialect):
    """Parse change record from either dialect."""
    
    # Both dialects have same structure, different format
    if dialect == 'GOOGLE_STANDARD_SQL':
        # STRUCT: access via dict keys
        if change_record.get('data_change_record'):
            return change_record['data_change_record']
    
    elif dialect == 'POSTGRESQL':
        # JSON: same access pattern
        if 'data_change_record' in change_record:
            return change_record['data_change_record']
    
    return None


# Works with both
dcr = parse_change_record(change_record, database_dialect)
if dcr:
    print(f"Table: {dcr['table_name']}")
    print(f"Operation: {dcr['mod_type']}")
```

---

## Tips and Best Practices

### 1. Store Checkpoints

Always save the last processed timestamp to avoid reprocessing:

```python
def process_changes_with_checkpoint():
    checkpoint_file = '/tmp/change_stream_checkpoint.txt'
    
    # Load last checkpoint
    try:
        with open(checkpoint_file, 'r') as f:
            last_timestamp = f.read().strip()
    except FileNotFoundError:
        last_timestamp = '2024-01-01T00:00:00Z'  # Start from beginning
    
    query = f"SELECT * FROM READ_changes('{last_timestamp}', NULL, NULL, 10000)"
    
    with database.snapshot() as snapshot:
        results = snapshot.execute_sql(query)
        
        for row in results:
            change_record = row[0]
            
            if change_record.get('data_change_record'):
                dcr = change_record['data_change_record']
                process_change(dcr)
                
                # Save checkpoint after processing
                with open(checkpoint_file, 'w') as f:
                    f.write(dcr['commit_timestamp'])
```

### 2. Handle Heartbeats Gracefully

Heartbeats confirm the stream is active but contain no data:

```python
heartbeat_count = 0

for row in results:
    change_record = row[0]
    
    if change_record.get('heartbeat_record'):
        heartbeat_count += 1
        if heartbeat_count % 10 == 0:
            print(f"Received {heartbeat_count} heartbeats (stream is active)")
        continue  # Skip processing
    
    # Process data change records...
```

### 3. Set Appropriate Heartbeat Interval

- **10 seconds (10000 ms)** - Real-time monitoring
- **30 seconds (30000 ms)** - Standard polling
- **60 seconds (60000 ms)** - Low-priority batch jobs

```python
# Real-time
query = "SELECT * FROM READ_changes('...', NULL, NULL, 10000)"

# Batch processing
query = "SELECT * FROM READ_changes('...', NULL, NULL, 60000)"
```

### 4. Use Bounded Queries for Backfills

Unbounded queries (end_timestamp=NULL) run indefinitely. For backfills, always set an end time:

```python
# Backfill last 7 days (bounded)
query = f"""
  SELECT * FROM READ_changes(
    '{seven_days_ago}',
    '{now}',  -- bounded
    NULL,
    10000
  )
"""
```

---

## Next Steps

- **Main Guide:** [change-streams.md](./change-streams.md) for complete API reference
- **Architecture:** [change-streams-architecture.md](./change-streams-architecture.md) for implementation details

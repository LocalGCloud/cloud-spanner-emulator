<!-- generated-by: gsd-doc-writer -->
# Configuration

The emulator ships as two binaries:

| Binary | Serves | Notes |
|--------|--------|-------|
| `gateway_main` | REST (default port 9020) and gRPC (default port 9010) | Starts `emulator_main` as a child process and forwards a fixed set of flags to it. The Docker image runs this. |
| `emulator_main` | gRPC only (default `localhost:10007`) | Accepts emulator flags directly, including ones the gateway doesn't forward. |

## Docker

The image `agentcloud/localcloud-spanner-emulator` has no `ENTRYPOINT`. Its
default command is `./gateway_main --hostname 0.0.0.0`. To pass flags, repeat
the whole command:

```shell
# In-memory (default)
docker run -p 9010:9010 -p 9020:9020 agentcloud/localcloud-spanner-emulator

# Persistent storage
docker run -p 9010:9010 -p 9020:9020 -v /path/to/data:/data \
  agentcloud/localcloud-spanner-emulator \
  ./gateway_main --hostname 0.0.0.0 --data_dir=/data

# Quarantine databases that fail to restore
docker run -p 9010:9010 -p 9020:9020 -v /path/to/data:/data \
  agentcloud/localcloud-spanner-emulator \
  ./gateway_main --hostname 0.0.0.0 --data_dir=/data --repair_corrupted_databases

# gRPC only, with an emulator_main-only flag
docker run -p 9010:9010 \
  agentcloud/localcloud-spanner-emulator \
  ./emulator_main --host_port=0.0.0.0:9010 --abort_current_transaction_probability=0
```

Appending only flags, such as `... localcloud-spanner-emulator --data_dir=/data`,
fails with `exec: "--data_dir=/data": no such file or directory`.

## `gateway_main` flags

| Flag | Default | Effect | Forwarded to `emulator_main` |
|------|---------|--------|:---:|
| `--hostname` | `localhost` (`0.0.0.0` in Docker) | Address both servers bind to | as `--host_port` |
| `--grpc_port` | `9010` | gRPC port | as `--host_port` |
| `--http_port` | `9020` | REST port | — |
| `--grpc_binary` | `emulator_main` | Path to the `emulator_main` binary | — |
| `--remote_functions_host_port` | empty | Forward a `localhost:PORT` remote-function backend to `emulator_main`. | yes |
| `--data_dir` | empty (in-memory) | Persistent storage directory. `emulator_main` locks it (`<data_dir>/.lock`), so a second emulator on the same directory exits at startup. See [Persistence](persistence.md). | yes |
| `--repair_corrupted_databases` | `false` | Move a database that fails to restore under `<data_dir>/.quarantine/` instead of leaving it unavailable. See [Persistence](persistence.md#quarantine). | yes |
| `--enforce_placement_dml_restrictions` | `true` | Production's placement DML limits. See [Placements](placements.md). | yes |
| `--override_max_databases_per_instance` | `100` | Raises the per-instance database limit when greater than 100; lower values cannot reduce it. | yes |
| `--override_change_stream_partition_token_alive_seconds` | `-1` (20–40 s nominal token life) | A positive X sets the churn age threshold, worker sleep, and churn retry sleep to X seconds (nominal token life X–2X seconds). | yes |
| `--row_deletion_policy_sweep_interval_seconds` | `60` | How often each database deletes rows that its row deletion policies (TTL) have expired. `0` or a negative value disables the sweeper. Production deletes within about 72 hours. | yes |
| `--lock_wait_timeout_ms` | `10000` | How long a read-write transaction waits for a lock that an older transaction holds before it aborts. An older transaction never waits for a younger one; it aborts (wounds) the younger holder. `0` aborts a conflicting request at once, the behavior before 2026-09-28. Waits ignore the RPC deadline. | yes |
| `--spanner_sys_expose_open_interval` | `false` | Also show the `SPANNER_SYS` statistics interval that is still in progress, so statistics are visible right after an operation. Production shows only ended intervals. | yes |
| `--log_requests` | `false` | Log gRPC requests and responses at INFO level. | yes |
| `--enable_fault_injection` | `false` | Randomly abort commits, to test retry logic | yes |
| `--disable_query_null_filtered_index_check` | `false` | Answer queries that use `NULL_FILTERED` indexes | yes |
| `--copy_emulator_stdout` | `false` | Copy the emulator's stdout to the gateway's | — |
| `--copy_emulator_stderr` | `true` | Copy the emulator's stderr to the gateway's | — |
| `--notices` | `false` | Print third-party notices and exit | — |

Neither binary requires environment variables. `gateway_main` reads these
optional overrides; `emulator_main` does not read them:

| Variable | Default | Overrides |
|----------|---------|-----------|
| `MAX_DATABASES_PER_INSTANCE` | Unset | `--override_max_databases_per_instance`; only values greater than 100 raise the limit. |
| `CHANGE_STREAM_PARTITION_TOKEN_ALIVE_SECONDS` | Unset | `--override_change_stream_partition_token_alive_seconds`; only positive values take effect. |

Both overrides must be integers when set; an invalid value prevents
`gateway_main` from starting.

If `emulator_main` exits, `gateway_main` exits with the same code. Both ports
open only after the emulator has finished restoring persisted data.

On `SIGINT` (Ctrl-C) or `SIGTERM` (`docker stop`, process managers),
`gateway_main` sends `SIGTERM` to `emulator_main`, waits up to 5 seconds for
it to exit, kills it if it hasn't, and then exits with status 0. With
`--data_dir`, `emulator_main` saves each database's `SPANNER_SYS` statistics
when it gets `SIGINT` or `SIGTERM`. `SIGKILL`
can't be caught: killing `gateway_main` that way leaves `emulator_main`
running.

## `emulator_main`-only flags

These have no `gateway_main` equivalent. Run `emulator_main` directly to use
them; see the Docker example above.

| Flag | Default | Effect |
|------|---------|--------|
| `--host_port` | `localhost:10007` | gRPC listen address |
| `--abort_current_transaction_probability` | `20` | Percentage chance that a transaction requesting a lock held by an older, idle transaction aborts that holder instead of waiting (see `--lock_wait_timeout_ms`). `0` gives Cloud Spanner's wound-wait behavior: a younger transaction always waits. Requests that can't wait, such as schema changes, abort the holder with this probability or abort themselves. |
| `--enable_change_stream_churning` | `true` | Rotate change stream partitions |
| `--change_stream_churning_interval` | `20s` | Minimum age of an active partition before churning. |
| `--change_stream_churn_thread_sleep_interval` | `20s` | How long the churn worker waits between scans. |
| `--change_stream_churn_thread_retry_sleep_interval` | `20ms` | Base sleep after each churn attempt, including before a retry. |
| `--change_stream_churn_thread_retry_jitter` | `100` | Upper bound, in milliseconds, of random delay added after each churn attempt. |
| `--change_streams_partition_query_chop_interval` | `100ms` | Timestamp span added when advancing the partition-query scan window; the initial scan may extend to now. |
| `--cloud_spanner_emulator_disable_cs_retention_check` | `false` | Skip the minimum/maximum change stream retention-period check; invalid duration formats remain rejected. |

## Client setup

Set `SPANNER_EMULATOR_HOST` in the client process to the gRPC address, for
example `localhost:9010`. This is a client setting, not an emulator server
setting. The server uses insecure gRPC and does not check credentials.

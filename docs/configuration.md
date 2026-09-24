# Configuration

The emulator ships as two binaries:

| Binary | Serves | Notes |
|--------|--------|-------|
| `gateway_main` | REST (default port 9020) and gRPC (default port 9010) | Starts `emulator_main` as a child process and forwards a fixed set of flags to it. The Docker image runs this. |
| `emulator_main` | gRPC only (default `localhost:10007`) | Accepts every emulator flag, including ones the gateway doesn't forward. |

## Docker

The image `jaysen2apache/spanner-emulator-extended` has no `ENTRYPOINT`. Its
default command is `./gateway_main --hostname 0.0.0.0`. To pass flags, repeat
the whole command:

```shell
# In-memory (default)
docker run -p 9010:9010 -p 9020:9020 jaysen2apache/spanner-emulator-extended

# Persistent storage
docker run -p 9010:9010 -p 9020:9020 -v /path/to/data:/data \
  jaysen2apache/spanner-emulator-extended \
  ./gateway_main --hostname 0.0.0.0 --data_dir=/data

# Quarantine databases that fail to restore
docker run -p 9010:9010 -p 9020:9020 -v /path/to/data:/data \
  jaysen2apache/spanner-emulator-extended \
  ./gateway_main --hostname 0.0.0.0 --data_dir=/data --repair_corrupted_databases

# gRPC only, with an emulator_main-only flag
docker run -p 9010:9010 \
  jaysen2apache/spanner-emulator-extended \
  ./emulator_main --host_port=0.0.0.0:9010 --abort_current_transaction_probability=0
```

Appending only flags, such as `... spanner-emulator-extended --data_dir=/data`,
fails with `exec: "--data_dir=/data": no such file or directory`.

## `gateway_main` flags

| Flag | Default | Effect | Forwarded to `emulator_main` |
|------|---------|--------|:---:|
| `--hostname` | `localhost` (`0.0.0.0` in Docker) | Address both servers bind to | as `--host_port` |
| `--grpc_port` | `9010` | gRPC port | as `--host_port` |
| `--http_port` | `9020` | REST port | — |
| `--grpc_binary` | `emulator_main` | Path to the `emulator_main` binary | — |
| `--data_dir` | empty (in-memory) | Persistent storage directory. See [Persistence](persistence.md). | yes |
| `--repair_corrupted_databases` | `false` | Move a database that fails to restore under `<data_dir>/.quarantine/` instead of leaving it unavailable. See [Persistence](persistence.md#quarantine). | yes |
| `--enforce_placement_dml_restrictions` | `true` | Production's placement DML limits. See [Placements](placements.md). | yes |
| `--override_max_databases_per_instance` | `100` | Raises the per-instance database limit; values at or below 100 are ignored | yes |
| `--override_change_stream_partition_token_alive_seconds` | `-1` (20–40 s) | Change stream partition tokens live X–2X seconds | yes |
| `--log_requests` | `false` | Log gRPC requests and responses | yes |
| `--enable_fault_injection` | `false` | Randomly abort commits, to test retry logic | yes |
| `--disable_query_null_filtered_index_check` | `false` | Answer queries that use `NULL_FILTERED` indexes | yes |
| `--copy_emulator_stdout` | `false` | Copy the emulator's stdout to the gateway's | — |
| `--copy_emulator_stderr` | `true` | Copy the emulator's stderr to the gateway's | — |
| `--notices` | `false` | Print third-party notices and exit | — |

Environment variables read by `gateway_main`:

| Variable | Overrides |
|----------|-----------|
| `MAX_DATABASES_PER_INSTANCE` | `--override_max_databases_per_instance` |
| `CHANGE_STREAM_PARTITION_TOKEN_ALIVE_SECONDS` | `--override_change_stream_partition_token_alive_seconds` |

If `emulator_main` exits, `gateway_main` exits with the same code. Both ports
open only after the emulator has finished restoring persisted data.

On `SIGINT` (Ctrl-C) or `SIGTERM` (`docker stop`, process managers),
`gateway_main` sends `SIGTERM` to `emulator_main`, waits up to 5 seconds for
it to exit, kills it if it hasn't, and then exits with status 0. `SIGKILL`
can't be caught: killing `gateway_main` that way leaves `emulator_main`
running.

## `emulator_main`-only flags

These have no `gateway_main` equivalent. Run `emulator_main` directly to use
them; see the Docker example above.

| Flag | Default | Effect |
|------|---------|--------|
| `--host_port` | `localhost:10007` | gRPC listen address |
| `--abort_current_transaction_probability` | `20` | Chance (0–100) that a new read-write transaction aborts the one in progress. `0` never aborts it. |
| `--remote_functions_host_port` | empty | Backend address for remote functions |
| `--enable_change_stream_churning` | `true` | Rotate change stream partitions |
| `--change_stream_churning_interval` | `20s` | How often partitions rotate |
| `--change_stream_churn_thread_sleep_interval` | `20s` | Partition rotation thread sleep |
| `--change_stream_churn_thread_retry_sleep_interval` | `20ms` | Wait before retrying a failed rotation |
| `--change_stream_churn_thread_retry_jitter` | `100` | Retry jitter for rotations |
| `--change_streams_partition_query_chop_interval` | `100ms` | How often a change stream query flushes results |
| `--cloud_spanner_emulator_disable_cs_retention_check` | `false` | Skip the change stream retention-period check on `CREATE` and `ALTER` |

## Client setup

Client libraries connect to the emulator when `SPANNER_EMULATOR_HOST` is set to
the gRPC address, for example `localhost:9010`. The emulator doesn't check
credentials.

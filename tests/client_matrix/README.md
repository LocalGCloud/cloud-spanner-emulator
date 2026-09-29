# Client compatibility matrix

Smoke programs that drive a running emulator through its public endpoints with
external client libraries and drivers. They are not Bazel targets; run them by
hand against `gateway_main` (gRPC and REST) when you change the API surface.

Each program creates its own instance (override with `INSTANCE`), then checks
instance and database creation, DDL, mutations, read-write DML with commit,
parameterized queries, snapshot and stale reads, batch DML, error codes, and
the PostgreSQL dialect. It prints one `PASS`/`FAIL` line per step and exits
non-zero if any step fails.

| Directory | Client | Last passing versions (2026-09-28) |
|-----------|--------|------------------------------------|
| `python/` | google-cloud-spanner | 3.71.0 |
| `go/` | cloud.google.com/go/spanner | v1.95.1 |
| `node/` | @google-cloud/spanner | 9.0.0 |
| `java/` | com.google.cloud:google-cloud-spanner | 6.123.0 |
| `jdbc/` | com.google.cloud:google-cloud-spanner-jdbc | 2.45.0 |
| `pgadapter/` | PGAdapter + org.postgresql:postgresql, and psql | 0.55.3 + 42.7.13, psql 17.5 |
| `rest/` | curl and jq against the REST gateway | curl 8.7.1, jq 1.8.1 |

## Running

```shell
# Emulator (gRPC 19410, REST 19420)
./gateway_main --grpc_binary=./emulator_main --hostname=localhost \
  --grpc_port=19410 --http_port=19420 &
export SPANNER_EMULATOR_HOST=localhost:19410

# Python
python3 -m venv /tmp/py && /tmp/py/bin/pip install google-cloud-spanner
/tmp/py/bin/python python/smoke.py

# Go
(cd go && go build -o smoke . && ./smoke)

# Node
(cd node && npm ci && node smoke.js)

# Java client and JDBC (EMU=host:port for JDBC)
(cd java && mvn -q dependency:build-classpath -Dmdep.outputFile=cp.txt \
  && javac -d classes -cp "$(cat cp.txt)" Smoke.java \
  && java -cp "classes:$(cat cp.txt)" Smoke)
(cd jdbc && mvn -q dependency:build-classpath -Dmdep.outputFile=cp.txt \
  && javac -d classes -cp "$(cat cp.txt)" JdbcSmoke.java \
  && EMU=localhost:19410 java -cp "classes:$(cat cp.txt)" JdbcSmoke)

# PGAdapter (port 19432) with pgJDBC and psql
(cd pgadapter && mvn -q dependency:build-classpath -Dmdep.outputFile=cp.txt)
java -cp "$(cat pgadapter/cp.txt)" com.google.cloud.spanner.pgadapter.Server \
  -p p -i pga-inst -s 19432 --dir "" -e localhost:19410 \
  -r autoConfigEmulator=true &
PGJ=$(tr ':' '\n' < pgadapter/cp.txt | grep postgresql-42)
(cd pgadapter && javac -d classes -cp "$PGJ" PgSmoke.java \
  && PGA_PORT=19432 java -cp "classes:$PGJ" PgSmoke)
psql -h localhost -p 19432 -d pgadb -f pgadapter/psql_smoke.sql

# REST (checks Google's {"error": {"code", "message", "status"}} envelope)
REST=http://localhost:19420 bash rest/rest_smoke.sh
```

JavaScript numbers such as `1.0` are integers, so the Node program passes
`Spanner.float(...)` for FLOAT64 mutation values, as the Node client requires.

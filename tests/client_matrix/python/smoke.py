"""Python google-cloud-spanner smoke test against the emulator."""
import datetime
import decimal
import json
import os
import sys
import time
import traceback

from google.api_core import exceptions as gexc
from google.cloud import spanner
from google.cloud.spanner_admin_database_v1 import DatabaseDialect
from google.cloud.spanner_v1 import param_types, JsonObject

PROJECT = "p"
INSTANCE = os.environ.get("INSTANCE", "py-inst")
DB = "pydb"
PGDB = "pypgdb"

results = []


def step(name):
    def deco(fn):
        def run():
            try:
                detail = fn()
                results.append((name, "PASS", detail or ""))
                print(f"[PASS] {name} {detail or ''}", flush=True)
            except Exception as e:  # noqa
                msg = f"{type(e).__name__}: {e}"
                results.append((name, "FAIL", msg))
                print(f"[FAIL] {name}: {msg}", flush=True)
                traceback.print_exc()
        run.__name__ = name
        return run
    return deco


client = spanner.Client(project=PROJECT)
instance = client.instance(
    INSTANCE,
    configuration_name=f"projects/{PROJECT}/instanceConfigs/emulator-config",
    display_name="py smoke",
    node_count=1,
)
database = instance.database(
    DB,
    ddl_statements=[
        """CREATE TABLE Items (
             Id INT64 NOT NULL,
             Name STRING(MAX),
             Data JSON,
             Amount NUMERIC,
             UpdatedAt TIMESTAMP OPTIONS (allow_commit_timestamp=true),
             Scores ARRAY<FLOAT64>
           ) PRIMARY KEY (Id)"""
    ],
)
COLS = ("Id", "Name", "Data", "Amount", "UpdatedAt", "Scores")


@step("create_instance")
def s_instance():
    op = instance.create()
    op.result(60)
    instance.reload()
    return f"config={instance.configuration_name}"


@step("create_database")
def s_db():
    op = database.create()
    op.result(120)
    return ""


@step("update_ddl_index")
def s_ddl():
    op = database.update_ddl(["CREATE INDEX ItemsByName ON Items(Name)"])
    op.result(120)
    database.reload()
    return f"ddl_count={len(database.ddl_statements)}"


@step("insert_mutations")
def s_mut():
    with database.batch() as b:
        b.insert(
            "Items",
            COLS,
            [
                (1, "one", JsonObject({"a": 1}), decimal.Decimal("1.50"), spanner.COMMIT_TIMESTAMP, [1.0, 2.5]),
                (2, "two", JsonObject({"b": [1, 2]}), decimal.Decimal("2.25"), spanner.COMMIT_TIMESTAMP, [3.0]),
            ],
        )
    return f"commit_ts={b.committed}"


@step("rw_txn_dml")
def s_rw():
    def work(tx):
        n1 = tx.execute_update(
            "INSERT INTO Items (Id, Name, Data, Amount, UpdatedAt, Scores) "
            "VALUES (@id, @name, @data, @amt, PENDING_COMMIT_TIMESTAMP(), @scores)",
            params={"id": 3, "name": "three", "data": JsonObject({"c": True}),
                    "amt": decimal.Decimal("3.125"), "scores": [0.5, 0.25]},
            param_types={"id": param_types.INT64, "name": param_types.STRING,
                         "data": param_types.JSON, "amt": param_types.NUMERIC,
                         "scores": param_types.Array(param_types.FLOAT64)},
        )
        n2 = tx.execute_update(
            "UPDATE Items SET Name = @name WHERE Id = @id",
            params={"id": 1, "name": "one-updated"},
            param_types={"id": param_types.INT64, "name": param_types.STRING},
        )
        return n1, n2
    n1, n2 = database.run_in_transaction(work)
    assert (n1, n2) == (1, 1), (n1, n2)
    return f"insert={n1} update={n2}"


@step("param_query")
def s_q():
    with database.snapshot() as snap:
        rows = list(snap.execute_sql(
            "SELECT Id, Name, Data, Amount, UpdatedAt, Scores FROM Items WHERE Id >= @min ORDER BY Id",
            params={"min": 1}, param_types={"min": param_types.INT64}))
    assert len(rows) == 3, rows
    r = rows[0]
    assert r[1] == "one-updated", r
    assert isinstance(r[3], decimal.Decimal), type(r[3])
    assert isinstance(r[4], datetime.datetime), type(r[4])
    assert list(rows[2][5]) == [0.5, 0.25], rows[2][5]
    return f"rows={len(rows)} row3={rows[2][1]},{rows[2][2]},{rows[2][3]}"


@step("readonly_snapshot_query")
def s_ro():
    with database.snapshot(multi_use=True) as snap:
        a = list(snap.execute_sql("SELECT COUNT(*) FROM Items"))[0][0]
        b = list(snap.read("Items", ("Id", "Name"), spanner.KeySet(all_=True)))
        c = list(snap.read("Items", ("Id", "Name"), spanner.KeySet(keys=[["one-updated"]]), index="ItemsByName"))
    return f"count={a} read_rows={len(b)} index_read={c}"


@step("stale_read_exact_staleness")
def s_stale():
    time.sleep(2)
    with database.snapshot(exact_staleness=datetime.timedelta(seconds=1)) as snap:
        n = list(snap.execute_sql("SELECT COUNT(*) FROM Items"))[0][0]
    assert n == 3, n
    return f"count={n}"


@step("batch_dml")
def s_bdml():
    def work(tx):
        status, counts = tx.batch_update([
            ("INSERT INTO Items (Id, Name) VALUES (@id, @name)",
             {"id": 10, "name": "ten"}, {"id": param_types.INT64, "name": param_types.STRING}),
            "UPDATE Items SET Amount = Amount + 1 WHERE Id IN (1, 2)",
        ])
        return status, counts
    status, counts = database.run_in_transaction(work)
    assert status.code == 0, status
    assert list(counts) == [1, 2], counts
    return f"counts={list(counts)}"


@step("duplicate_key_error")
def s_dup():
    def work(tx):
        tx.execute_update("INSERT INTO Items (Id, Name) VALUES (1, 'dup')")
    try:
        database.run_in_transaction(work)
    except gexc.AlreadyExists as e:
        return f"AlreadyExists grpc_code={e.grpc_status_code} msg={e.message!r}"
    raise AssertionError("expected AlreadyExists")


@step("duplicate_key_mutation_error")
def s_dup_mut():
    try:
        with database.batch() as b:
            b.insert("Items", ("Id", "Name"), [(2, "dup")])
    except gexc.AlreadyExists as e:
        return f"AlreadyExists grpc_code={e.grpc_status_code} msg={e.message!r}"
    raise AssertionError("expected AlreadyExists")


pgdb = instance.database(PGDB, database_dialect=DatabaseDialect.POSTGRESQL)


@step("pg_create_database")
def s_pg_create():
    pgdb.create().result(120)
    pgdb.reload()
    return f"dialect={pgdb.database_dialect}"


@step("pg_ddl")
def s_pg_ddl():
    pgdb.update_ddl([
        "CREATE TABLE users (id bigint NOT NULL PRIMARY KEY, name varchar, amount numeric, data jsonb, "
        "ts timestamptz, tags text[])"
    ]).result(120)
    return ""


@step("pg_insert")
def s_pg_ins():
    def work(tx):
        return tx.execute_update(
            "INSERT INTO users (id, name, amount, data, ts, tags) VALUES ($1, $2, $3, $4, $5, $6)",
            params={"p1": 1, "p2": "alice", "p3": decimal.Decimal("9.99"),
                    "p4": JsonObject({"k": "v"}),
                    "p5": datetime.datetime(2024, 1, 2, 3, 4, 5, tzinfo=datetime.timezone.utc),
                    "p6": ["x", "y"]},
            param_types={"p1": param_types.INT64, "p2": param_types.STRING,
                         "p3": param_types.PG_NUMERIC, "p4": param_types.PG_JSONB,
                         "p5": param_types.TIMESTAMP, "p6": param_types.Array(param_types.STRING)},
        )
    n = pgdb.run_in_transaction(work)
    with pgdb.batch() as b:
        b.insert("users", ("id", "name"), [(2, "bob")])
    return f"dml_rows={n} + 1 mutation"


@step("pg_param_query")
def s_pg_q():
    with pgdb.snapshot() as snap:
        rows = list(snap.execute_sql("SELECT id, name, amount, data, ts, tags FROM users WHERE id = $1",
                                     params={"p1": 1}, param_types={"p1": param_types.INT64}))
    assert len(rows) == 1 and rows[0][1] == "alice", rows
    return f"row={rows[0]}"


@step("pg_duplicate_key_error")
def s_pg_dup():
    def work(tx):
        tx.execute_update("INSERT INTO users (id, name) VALUES (1, 'dup')")
    try:
        pgdb.run_in_transaction(work)
    except gexc.AlreadyExists as e:
        return f"AlreadyExists msg={e.message!r}"
    raise AssertionError("expected AlreadyExists")


if __name__ == "__main__":
    for fn in [s_instance, s_db, s_ddl, s_mut, s_rw, s_q, s_ro, s_stale, s_bdml, s_dup, s_dup_mut,
               s_pg_create, s_pg_ddl, s_pg_ins, s_pg_q, s_pg_dup]:
        fn()
    print("\nSUMMARY")
    for n, s, d in results:
        print(f"{s}\t{n}\t{d}")
    sys.exit(0 if all(s == "PASS" for _, s, _ in results) else 1)

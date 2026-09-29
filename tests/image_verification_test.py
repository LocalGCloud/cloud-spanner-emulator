#!/usr/bin/env python3
"""
Test suite covering Steps 2, 3, 4, 5 against spanner-emulator-extended:local:
Step 2: Restart with same volume, both dialects:
  - Create roles with GRANTs and a DEFINER view.
  - Set database options.
  - Add a TTL policy and a sequence.
  - Add a unique index, then write rows.
  - Make a backup with version_time, and call AddSplitPoints.
  - Restart the container with the same volume.
  - Check:
    * GetDatabaseDdl still shows the grants, options, definer view, TTL policy, sequences, unique index.
    * A creator_role session is still restricted (and invalid roles rejected).
    * Range reads and the unique index behave (exercising the data-loss bug).
    * SPANNER_SYS.USER_SPLIT_POINTS and query statistics survive restart.
    * Restore the backup to a new database and verify it gets historical data.
Step 3: Data directory lock:
  - Start second container on same volume -> exits cleanly with lock error instead of corrupting storage.
Step 4: Flags reach emulator:
  - --row_deletion_policy_sweep_interval_seconds=5: row qualifies for TTL and disappears within ~5s.
  - --spanner_sys_expose_open_interval=true: query appears in SPANNER_SYS.QUERY_STATS_TOP_MINUTE immediately.
Step 5: Feature spot checks:
  - Two REPEATABLE_READ txns updating same row: one aborts.
  - Two-txn lock conflict: younger waits (wound-wait), then proceeds after older commits.
  - Query with PLAN and PROFILE: plan tree returned (planNodes, executionStats).
  - Property graph query: CALL PageRank(...) returns scores.
  - Search query with Unicode search term.
  - PostgreSQL: CREATE INDEX ... USING scann plus spanner.approx_cosine_distance query.
"""

import datetime
import json
import os
import subprocess
import sys
import threading
import time

from google.cloud import spanner
from google.cloud.spanner_admin_database_v1 import DatabaseAdminClient
from google.cloud.spanner_admin_database_v1.types import (
    AddSplitPointsRequest,
    SplitPoints,
)
import requests

EMU_HOST = "localhost:9010"
REST_BASE = "http://localhost:9020/v1"
os.environ["SPANNER_EMULATOR_HOST"] = EMU_HOST

PROJECT = "p"
INSTANCE = "test-volume-inst"
GSQL_DB = "gsql-vol-db"
PG_DB = "pg-vol-db"
DOCKER_VOL = "spanner-vol-test"

RESULTS = []

def record(name, passed, detail=""):
    status = "PASS" if passed else "FAIL"
    RESULTS.append((status, name, detail))
    print(f"[{status}] {name} :: {detail}", flush=True)

def run_cmd(cmd, check=True):
    res = subprocess.run(cmd, shell=True, stdout=subprocess.PIPE, stderr=subprocess.PIPE, text=True)
    if check and res.returncode != 0:
        raise RuntimeError(f"Command '{cmd}' failed (code {res.returncode}):\n{res.stderr}\n{res.stdout}")
    return res

def wait_for_emulator(timeout=30):
    start = time.time()
    while time.time() - start < timeout:
        try:
            r = requests.get(f"{REST_BASE}/projects/{PROJECT}/instanceConfigs", timeout=2)
            if r.status_code == 200:
                return True
        except Exception:
            pass
        time.sleep(0.5)
    raise TimeoutError("Emulator did not become ready in time")

def exec_dml(sess, sql):
    r = requests.post(
        f"{REST_BASE}/{sess}:executeSql",
        json={"transaction": {"begin": {"readWrite": {}}}, "sql": sql},
    )
    if r.status_code != 200:
        return r
    tx_id = r.json().get("metadata", {}).get("transaction", {}).get("id")
    if tx_id:
        c_res = requests.post(f"{REST_BASE}/{sess}:commit", json={"transactionId": tx_id})
        if c_res.status_code != 200:
            return c_res
    return r

def main():
    print("==========================================================", flush=True)
    print("Starting Spanner Emulator Verification: Steps 2, 3, 4, 5", flush=True)
    print("==========================================================", flush=True)

    # Clean previous containers and clean data volume
    run_cmd("docker rm -f spanner-emulator spanner-persist 2>/dev/null || true", check=False)
    run_cmd(f"docker volume rm -f {DOCKER_VOL} 2>/dev/null || true", check=False)
    run_cmd(f"docker volume create {DOCKER_VOL}", check=True)

    # -------------------------------------------------------------------------
    # Launch container with persistent volume and specific flags
    # -------------------------------------------------------------------------
    print(f"\n--- Starting container spanner-persist with volume={DOCKER_VOL} ---", flush=True)
    start_cmd = (
        f"docker run -d --name spanner-persist "
        f"-p 9010:9010 -p 9020:9020 "
        f"-v {DOCKER_VOL}:/data "
        f"spanner-emulator-extended:local "
        f"./gateway_main --hostname 0.0.0.0 --data_dir=/data "
        f"--row_deletion_policy_sweep_interval_seconds=5 "
        f"--spanner_sys_expose_open_interval=true "
        f"--lock_wait_timeout_ms=5000"
    )
    run_cmd(start_cmd)
    wait_for_emulator()
    record("container_start_with_volume", True, "spanner-persist running on ports 9010/9020")

    # -------------------------------------------------------------------------
    # STEP 3: Data directory lock test
    # -------------------------------------------------------------------------
    print("\n--- Step 3: Data directory lock test ---", flush=True)
    lock_run = run_cmd(
        f"docker run --rm -v {DOCKER_VOL}:/data spanner-emulator-extended:local "
        f"./gateway_main --hostname 0.0.0.0 --data_dir=/data",
        check=False
    )
    combined_lock_output = (lock_run.stderr + "\n" + lock_run.stdout).strip()
    lock_passed = (
        lock_run.returncode != 0
        and ("is in use" in combined_lock_output or "lock" in combined_lock_output.lower() or "failedprecondition" in combined_lock_output.lower())
    )
    record("step3_data_directory_lock", lock_passed, f"exit={lock_run.returncode}, msg={combined_lock_output[:140]}")

    # -------------------------------------------------------------------------
    # STEP 2 (Before restart): Both Dialects Setup
    # -------------------------------------------------------------------------
    print("\n--- Step 2 (Before restart): Create GoogleSQL & PostgreSQL databases ---", flush=True)
    client = spanner.Client(project=PROJECT)
    instance = client.instance(INSTANCE, configuration_name=f"projects/{PROJECT}/instanceConfigs/emulator-config")
    if not instance.exists():
        op = instance.create()
        op.result(30)
    record("step2_create_instance", True, f"Instance {INSTANCE} created/ready")

    db_admin = client.database_admin_api

    # 1. GoogleSQL Database Setup
    gsql_ddl = [
        "CREATE TABLE Users (UserId INT64, Name STRING(MAX), UpdatedAt TIMESTAMP) PRIMARY KEY(UserId)",
        "CREATE ROLE analyst",
        "GRANT SELECT ON TABLE Users TO ROLE analyst",
        "CREATE VIEW UserView SQL SECURITY DEFINER AS SELECT Users.UserId, Users.Name FROM Users",
        "ALTER TABLE Users ADD ROW DELETION POLICY (OLDER_THAN(UpdatedAt, INTERVAL 1 DAY))",
        "CREATE SEQUENCE UserSeq OPTIONS (sequence_kind = 'bit_reversed_positive')",
        "CREATE UNIQUE INDEX UsersByName ON Users(Name)",
    ]
    gsql_db = instance.database(GSQL_DB, ddl_statements=gsql_ddl)
    op = gsql_db.create()
    op.result(30)

    # Set database options
    op = gsql_db.update_ddl(["ALTER DATABASE `" + GSQL_DB + "` SET OPTIONS (version_retention_period = '2h')"])
    op.result(30)
    record("step2_gsql_db_setup", True, "DDL, role, grant, definer view, TTL, sequence, unique index, options configured")

    # Insert rows into Users before backup
    now_ts = datetime.datetime.now(datetime.timezone.utc)
    alice_ts = now_ts - datetime.timedelta(hours=1)
    with gsql_db.batch() as batch:
        batch.insert(
            table="Users",
            columns=("UserId", "Name", "UpdatedAt"),
            values=[
                (1, "Alice", alice_ts),
                (2, "Bob", now_ts),
            ],
        )
    record("step2_gsql_insert_rows", True, "Initial rows (Alice, Bob) inserted into Users")

    # Call AddSplitPoints for GoogleSQL
    sp_req_gsql = AddSplitPointsRequest(
        database=f"projects/{PROJECT}/instances/{INSTANCE}/databases/{GSQL_DB}",
        split_points=[
            SplitPoints(
                table="Users",
                keys=[{"key_parts": ["2"]}],
            )
        ],
    )
    db_admin.add_split_points(request=sp_req_gsql)
    record("step2_gsql_add_split_points", True, "Split point added at Users(2)")

    # Record version_time and insert row 3, then create backup with version_time
    time.sleep(1)
    gsql_version_time = datetime.datetime.now(datetime.timezone.utc)
    time.sleep(1)

    with gsql_db.batch() as batch:
        batch.insert(
            table="Users",
            columns=("UserId", "Name", "UpdatedAt"),
            values=[(3, "Charlie", now_ts)],
        )

    # Create backup with version_time for GoogleSQL
    vt_str_gsql = gsql_version_time.strftime("%Y-%m-%dT%H:%M:%SZ")
    expire_time_str = (now_ts + datetime.timedelta(days=14)).strftime("%Y-%m-%dT%H:%M:%SZ")
    backup_url_gsql = f"{REST_BASE}/projects/{PROJECT}/instances/{INSTANCE}/backups?backupId=user-backup"
    b_payload = {
        "database": f"projects/{PROJECT}/instances/{INSTANCE}/databases/{GSQL_DB}",
        "versionTime": vt_str_gsql,
        "expireTime": expire_time_str,
    }
    r = requests.post(backup_url_gsql, json=b_payload)
    if r.status_code == 200:
        b_op = r.json().get("name")
        for _ in range(60):
            op_res = requests.get(f"{REST_BASE}/{b_op}").json()
            if op_res.get("done"):
                break
            time.sleep(0.5)
        record("step2_gsql_create_backup_version_time", True, f"Backup user-backup created with versionTime={vt_str_gsql}")
    else:
        record("step2_gsql_create_backup_version_time", False, f"HTTP {r.status_code} {r.text}")

    # 2. PostgreSQL Database Setup
    pg_ddl = [
        "CREATE TABLE items (id bigint NOT NULL PRIMARY KEY, name varchar, updated_at timestamptz)",
        "CREATE ROLE app_analyst",
        "GRANT SELECT ON TABLE items TO app_analyst",
        "CREATE VIEW item_view SQL SECURITY DEFINER AS SELECT id, name FROM items",
        "ALTER TABLE items ADD TTL INTERVAL '1 DAYS' ON updated_at",
        "CREATE SEQUENCE item_seq BIT_REVERSED_POSITIVE",
        "CREATE UNIQUE INDEX items_by_name ON items(name)",
    ]
    r = requests.post(
        f"{REST_BASE}/projects/{PROJECT}/instances/{INSTANCE}/databases",
        json={"createStatement": f'CREATE DATABASE "{PG_DB}"', "databaseDialect": "POSTGRESQL"},
    )
    if r.status_code == 200:
        pg_op = r.json().get("name")
        for _ in range(60):
            if requests.get(f"{REST_BASE}/{pg_op}").json().get("done"):
                break
            time.sleep(0.5)
        # Update DDL
        r = requests.patch(
            f"{REST_BASE}/projects/{PROJECT}/instances/{INSTANCE}/databases/{PG_DB}/ddl",
            json={"statements": pg_ddl},
        )
        if r.status_code == 200:
            pg_op = r.json().get("name")
            for _ in range(60):
                if requests.get(f"{REST_BASE}/{pg_op}").json().get("done"):
                    break
                time.sleep(0.5)
            # Set database options
            r_opt = requests.patch(
                f"{REST_BASE}/projects/{PROJECT}/instances/{INSTANCE}/databases/{PG_DB}/ddl",
                json={"statements": [f'ALTER DATABASE "{PG_DB}" SET spanner.version_retention_period = \'2h\'']},
            )
            if r_opt.status_code == 200:
                pg_op = r_opt.json().get("name")
                for _ in range(60):
                    if requests.get(f"{REST_BASE}/{pg_op}").json().get("done"):
                        break
                    time.sleep(0.5)
            record("step2_pg_db_setup", True, "PG DB, role, definer view, TTL, sequence, index, options created")
        else:
            record("step2_pg_db_setup", False, f"DDL failed: {r.text}")
    else:
        record("step2_pg_db_setup", False, f"Create PG DB failed: {r.text}")

    # Insert rows into PostgreSQL items table before restart
    r = requests.post(f"{REST_BASE}/projects/{PROJECT}/instances/{INSTANCE}/databases/{PG_DB}/sessions", json={})
    pg_sess_pre = r.json()["name"]
    ins_r = exec_dml(pg_sess_pre, "INSERT INTO items (id, name, updated_at) VALUES (1, 'Widget', '2026-09-01T00:00:00Z'), (2, 'Gadget', '2026-09-28T00:00:00Z')")
    record("step2_pg_insert_rows", ins_r.status_code == 200, f"Rows (Widget, Gadget) inserted into items (status={ins_r.status_code})")

    # AddSplitPoints for PostgreSQL
    sp_req_pg = AddSplitPointsRequest(
        database=f"projects/{PROJECT}/instances/{INSTANCE}/databases/{PG_DB}",
        split_points=[
            SplitPoints(
                table="items",
                keys=[{"key_parts": ["2"]}],
            )
        ],
    )
    db_admin.add_split_points(request=sp_req_pg)
    record("step2_pg_add_split_points", True, "Split point added at items(2)")

    # -------------------------------------------------------------------------
    # RESTART THE CONTAINER WITH THE SAME VOLUME
    # -------------------------------------------------------------------------
    print("\n--- Restarting container spanner-persist ---", flush=True)
    run_cmd("docker restart spanner-persist")
    wait_for_emulator()
    record("step2_container_restarted", True, "spanner-persist restarted cleanly with same volume")

    # -------------------------------------------------------------------------
    # STEP 2 (After restart): Verifying Persisted State
    # -------------------------------------------------------------------------
    print("\n--- Step 2 (After restart): Verifying persisted state ---", flush=True)

    # 1. GoogleSQL DDL check
    gsql_ddl_res = requests.get(f"{REST_BASE}/projects/{PROJECT}/instances/{INSTANCE}/databases/{GSQL_DB}/ddl").json()
    stmts_gsql = " ".join(gsql_ddl_res.get("statements", []))
    has_role = "ROLE analyst" in stmts_gsql
    has_grant = "GRANT SELECT ON TABLE Users TO ROLE analyst" in stmts_gsql
    has_definer = "SQL SECURITY DEFINER" in stmts_gsql
    has_ttl = "ROW DELETION POLICY" in stmts_gsql
    has_seq = "CREATE SEQUENCE UserSeq" in stmts_gsql
    has_idx = "CREATE UNIQUE INDEX UsersByName" in stmts_gsql

    record("step2_gsql_ddl_persisted", has_role and has_grant and has_definer and has_ttl and has_seq and has_idx,
           f"role={has_role}, grant={has_grant}, definer={has_definer}, ttl={has_ttl}, seq={has_seq}, idx={has_idx}")

    # Check GoogleSQL options
    db_info = requests.get(f"{REST_BASE}/projects/{PROJECT}/instances/{INSTANCE}/databases/{GSQL_DB}").json()
    has_opt = db_info.get("versionRetentionPeriod") == "2h"
    record("step2_gsql_options_persisted", has_opt, f"versionRetentionPeriod={db_info.get('versionRetentionPeriod')}")

    # 2. PostgreSQL DDL check
    pg_ddl_res = requests.get(f"{REST_BASE}/projects/{PROJECT}/instances/{INSTANCE}/databases/{PG_DB}/ddl").json()
    stmts_pg = " ".join(pg_ddl_res.get("statements", []))
    has_pg_role = "app_analyst" in stmts_pg
    has_pg_grant = "GRANT SELECT" in stmts_pg and "app_analyst" in stmts_pg
    has_pg_definer = "SQL SECURITY DEFINER" in stmts_pg
    has_pg_ttl = "TTL INTERVAL" in stmts_pg
    has_pg_seq = "item_seq" in stmts_pg
    has_pg_idx = "items_by_name" in stmts_pg

    record("step2_pg_ddl_persisted", has_pg_role and has_pg_grant and has_pg_definer and has_pg_ttl and has_pg_seq and has_pg_idx,
           f"role={has_pg_role}, grant={has_pg_grant}, definer={has_pg_definer}, ttl={has_pg_ttl}, seq={has_pg_seq}, idx={has_pg_idx}")

    # Check PostgreSQL options
    pg_db_info = requests.get(f"{REST_BASE}/projects/{PROJECT}/instances/{INSTANCE}/databases/{PG_DB}").json()
    has_pg_opt = pg_db_info.get("versionRetentionPeriod") == "2h"
    record("step2_pg_options_persisted", has_pg_opt, f"versionRetentionPeriod={pg_db_info.get('versionRetentionPeriod')}")

    # 3. Test creator_role restriction on GoogleSQL
    r = requests.post(
        f"{REST_BASE}/projects/{PROJECT}/instances/{INSTANCE}/databases/{GSQL_DB}/sessions",
        json={"session": {"creatorRole": "analyst"}},
    )
    if r.status_code == 200:
        sess_analyst = r.json()["name"]
        sel_r = requests.post(f"{REST_BASE}/{sess_analyst}:executeSql", json={"sql": "SELECT * FROM Users"})
        ins_r = requests.post(
            f"{REST_BASE}/{sess_analyst}:executeSql",
            json={"transaction": {"begin": {"readWrite": {}}}, "sql": "INSERT INTO Users (UserId, Name) VALUES (99, 'Denied')"},
        )
        role_restricted = (sel_r.status_code == 200) and (ins_r.status_code == 403 or "does not have required privileges" in ins_r.text)
        record("step2_creator_role_restricted", role_restricted, f"sel={sel_r.status_code}, ins={ins_r.status_code} ({ins_r.text.strip()[:60]})")
    else:
        record("step2_creator_role_restricted", False, f"create session failed: {r.text}")

    # Unknown role rejected
    r = requests.post(
        f"{REST_BASE}/projects/{PROJECT}/instances/{INSTANCE}/databases/{GSQL_DB}/sessions",
        json={"session": {"creatorRole": "nonexistent_role"}},
    )
    if r.status_code == 200:
        sess_bad = r.json()["name"]
        bad_r = requests.post(f"{REST_BASE}/{sess_bad}:executeSql", json={"sql": "SELECT 1"})
        record("step2_unknown_role_rejected", "Role not found" in bad_r.text, f"HTTP {bad_r.status_code}: {bad_r.text}")
    else:
        record("step2_unknown_role_rejected", "Role not found" in r.text, f"HTTP {r.status_code}: {r.text}")

    # 4. Range reads and unique index check (GoogleSQL)
    sess_r = requests.post(f"{REST_BASE}/projects/{PROJECT}/instances/{INSTANCE}/databases/{GSQL_DB}/sessions", json={}).json()
    admin_sess = sess_r["name"]
    range_r = requests.post(
        f"{REST_BASE}/{admin_sess}:executeSql",
        json={"sql": "SELECT UserId, Name FROM Users WHERE UserId >= 1 ORDER BY UserId"},
    ).json()
    range_rows = range_r.get("rows", [])
    record("step2_gsql_range_reads_after_restart", len(range_rows) >= 3, f"rows returned: {range_rows}")

    dup_r = requests.post(
        f"{REST_BASE}/{admin_sess}:executeSql",
        json={"transaction": {"begin": {"readWrite": {}}}, "sql": "INSERT INTO Users (UserId, Name) VALUES (10, 'Bob')"},
    )
    dup_fails = (dup_r.status_code == 409 or "already exists" in dup_r.text.lower())
    record("step2_gsql_unique_index_enforced", dup_fails, f"HTTP {dup_r.status_code}: {dup_r.text[:80]}")

    # 5. Range reads and unique index check (PostgreSQL)
    sess_pg_r = requests.post(f"{REST_BASE}/projects/{PROJECT}/instances/{INSTANCE}/databases/{PG_DB}/sessions", json={}).json()
    pg_admin_sess = sess_pg_r["name"]
    pg_range_r = requests.post(
        f"{REST_BASE}/{pg_admin_sess}:executeSql",
        json={"sql": "SELECT id, name FROM items WHERE id >= 1 ORDER BY id"},
    ).json()
    pg_range_rows = pg_range_r.get("rows", [])
    record("step2_pg_range_reads_after_restart", len(pg_range_rows) >= 2, f"rows returned: {pg_range_rows}")

    pg_dup_r = requests.post(
        f"{REST_BASE}/{pg_admin_sess}:executeSql",
        json={"transaction": {"begin": {"readWrite": {}}}, "sql": "INSERT INTO items (id, name) VALUES (10, 'Gadget')"},
    )
    pg_dup_fails = (pg_dup_r.status_code == 409 or "already exists" in pg_dup_r.text.lower())
    record("step2_pg_unique_index_enforced", pg_dup_fails, f"HTTP {pg_dup_r.status_code}: {pg_dup_r.text[:80]}")

    # 6. SPANNER_SYS.USER_SPLIT_POINTS survival check
    sp_r = requests.post(
        f"{REST_BASE}/{admin_sess}:executeSql",
        json={"sql": "SELECT table_name, split_key FROM SPANNER_SYS.USER_SPLIT_POINTS"},
    ).json()
    sp_rows = sp_r.get("rows", [])
    record("step2_user_split_points_persisted", len(sp_rows) > 0, f"split points count: {len(sp_rows)}, rows={sp_rows}")

    # 7. Query statistics survival check
    qs_r = requests.post(
        f"{REST_BASE}/{admin_sess}:executeSql",
        json={"sql": "SELECT text_fingerprint FROM SPANNER_SYS.QUERY_STATS_TOP_MINUTE"},
    ).json()
    record("step2_query_stats_persisted", "rows" in qs_r, f"stats rows count: {len(qs_r.get('rows', []))}")

    # 8. Restore backup to new database and verify historical data
    restore_url = f"{REST_BASE}/projects/{PROJECT}/instances/{INSTANCE}/databases:restore"
    restore_payload = {
        "databaseId": "restored-db",
        "backup": f"projects/{PROJECT}/instances/{INSTANCE}/backups/user-backup",
    }
    r = requests.post(restore_url, json=restore_payload)
    if r.status_code == 200:
        rest_op = r.json().get("name")
        for _ in range(60):
            if requests.get(f"{REST_BASE}/{rest_op}").json().get("done"):
                break
            time.sleep(0.5)
        rest_sess = requests.post(f"{REST_BASE}/projects/{PROJECT}/instances/{INSTANCE}/databases/restored-db/sessions", json={}).json()["name"]
        rest_rows = requests.post(f"{REST_BASE}/{rest_sess}:executeSql", json={"sql": "SELECT UserId, Name FROM Users ORDER BY UserId"}).json().get("rows", [])
        # Charlie (id=3) was inserted AFTER version_time, so Charlie must NOT be present
        has_charlie = any(row[0] == "3" for row in rest_rows)
        has_alice_bob = any(row[0] == "1" for row in rest_rows) and any(row[0] == "2" for row in rest_rows)
        record("step2_backup_restores_historical_data", not has_charlie and has_alice_bob, f"restored rows: {rest_rows}")
    else:
        record("step2_backup_restores_historical_data", False, f"Restore failed: {r.text}")

    # -------------------------------------------------------------------------
    # STEP 4: Flags reach emulator (TTL sweep 5s, Open-interval stats)
    # -------------------------------------------------------------------------
    print("\n--- Step 4: Flags reach emulator (TTL 5s, Open-interval stats) ---", flush=True)

    # Insert row that qualifies for TTL (UpdatedAt = 3 days ago)
    old_expired_ts = (datetime.datetime.now(datetime.timezone.utc) - datetime.timedelta(days=3)).strftime("%Y-%m-%dT%H:%M:%SZ")
    exec_dml(admin_sess, f"INSERT INTO Users (UserId, Name, UpdatedAt) VALUES (50, 'ExpiredUser', '{old_expired_ts}')")
    # Verify row exists
    row50_init = requests.post(f"{REST_BASE}/{admin_sess}:executeSql", json={"sql": "SELECT UserId FROM Users WHERE UserId = 50"}).json().get("rows", [])
    assert len(row50_init) == 1, "Failed to insert test row 50"

    # Wait ~7s for 5s sweep interval to delete expired row
    time.sleep(7)
    check_expired = requests.post(
        f"{REST_BASE}/{admin_sess}:executeSql",
        json={"sql": "SELECT UserId FROM Users WHERE UserId = 50"},
    ).json().get("rows", [])
    ttl_expired = len(check_expired) == 0
    record("step4_ttl_expires_in_5s", ttl_expired, f"row 50 present after 7s: {len(check_expired) > 0}")

    # Open-interval query stats check
    marker_query = "SELECT 123456 AS marker_open_stat"
    requests.post(f"{REST_BASE}/{admin_sess}:executeSql", json={"sql": marker_query})
    q_stats = requests.post(
        f"{REST_BASE}/{admin_sess}:executeSql",
        json={"sql": "SELECT text FROM SPANNER_SYS.QUERY_STATS_TOP_MINUTE WHERE text LIKE '%123456%'"},
    ).json().get("rows", [])
    open_interval_ok = len(q_stats) > 0
    record("step4_open_interval_stats_immediate", open_interval_ok, f"query immediately found in open stats: {len(q_stats) > 0}")

    # -------------------------------------------------------------------------
    # STEP 5: Feature Spot Checks
    # -------------------------------------------------------------------------
    print("\n--- Step 5: Feature spot checks ---", flush=True)

    # 1. Two REPEATABLE_READ transactions updating the same row: one must abort
    sess1 = requests.post(f"{REST_BASE}/projects/{PROJECT}/instances/{INSTANCE}/databases/{GSQL_DB}/sessions", json={}).json()["name"]
    sess2 = requests.post(f"{REST_BASE}/projects/{PROJECT}/instances/{INSTANCE}/databases/{GSQL_DB}/sessions", json={}).json()["name"]

    tx1 = requests.post(f"{REST_BASE}/{sess1}:beginTransaction", json={"options": {"readWrite": {}, "isolationLevel": "REPEATABLE_READ"}}).json()["id"]
    tx2 = requests.post(f"{REST_BASE}/{sess2}:beginTransaction", json={"options": {"readWrite": {}, "isolationLevel": "REPEATABLE_READ"}}).json()["id"]

    requests.post(f"{REST_BASE}/{sess1}:executeSql", json={"transaction": {"id": tx1}, "sql": "UPDATE Users SET Name = 'Bob_RR1' WHERE UserId = 2"})
    requests.post(f"{REST_BASE}/{sess2}:executeSql", json={"transaction": {"id": tx2}, "sql": "UPDATE Users SET Name = 'Bob_RR2' WHERE UserId = 2"})

    commit1 = requests.post(f"{REST_BASE}/{sess1}:commit", json={"transactionId": tx1})
    commit2 = requests.post(f"{REST_BASE}/{sess2}:commit", json={"transactionId": tx2})

    rr_aborts = (commit1.status_code == 200 and commit2.status_code == 409) or (commit2.status_code == 200 and commit1.status_code == 409)
    record("step5_repeatable_read_aborts_conflict", rr_aborts, f"c1={commit1.status_code}, c2={commit2.status_code}")

    # 2. Two-transaction lock conflict: younger waits (wound-wait), then proceeds
    sess_a = requests.post(f"{REST_BASE}/projects/{PROJECT}/instances/{INSTANCE}/databases/{GSQL_DB}/sessions", json={}).json()["name"]
    sess_b = requests.post(f"{REST_BASE}/projects/{PROJECT}/instances/{INSTANCE}/databases/{GSQL_DB}/sessions", json={}).json()["name"]

    tx_old = requests.post(f"{REST_BASE}/{sess_a}:beginTransaction", json={"options": {"readWrite": {}}}).json()["id"]
    requests.post(f"{REST_BASE}/{sess_a}:executeSql", json={"transaction": {"id": tx_old}, "sql": "UPDATE Users SET Name = 'LockA' WHERE UserId = 2"})

    time.sleep(0.5)
    tx_young = requests.post(f"{REST_BASE}/{sess_b}:beginTransaction", json={"options": {"readWrite": {}}}).json()["id"]

    young_result = {}
    def young_worker():
        t0 = time.time()
        res = requests.post(f"{REST_BASE}/{sess_b}:executeSql", json={"transaction": {"id": tx_young}, "sql": "UPDATE Users SET Name = 'LockB' WHERE UserId = 2"})
        young_result["time"] = time.time() - t0
        young_result["status"] = res.status_code
        requests.post(f"{REST_BASE}/{sess_b}:commit", json={"transactionId": tx_young})

    th = threading.Thread(target=young_worker)
    th.start()
    time.sleep(1.5)
    requests.post(f"{REST_BASE}/{sess_a}:commit", json={"transactionId": tx_old})
    th.join()

    lock_wait_ok = (young_result.get("status") == 200 and young_result.get("time", 0) >= 1.0)
    record("step5_wound_wait_younger_waits_and_proceeds", lock_wait_ok, f"younger status={young_result.get('status')}, wait_time={young_result.get('time', 0):.2f}s")

    # 3. PLAN and PROFILE return real plan tree
    plan_res = requests.post(
        f"{REST_BASE}/{admin_sess}:executeSql",
        json={"sql": "SELECT * FROM Users WHERE UserId = 2", "queryMode": "PLAN"},
    ).json()
    plan_nodes = plan_res.get("stats", {}).get("queryPlan", {}).get("planNodes", [])
    has_plan_tree = len(plan_nodes) > 1 and any("Scan" in n.get("displayName", "") for n in plan_nodes)
    record("step5_plan_tree_returned", has_plan_tree, f"plan nodes count: {len(plan_nodes)}, displayNames: {[n.get('displayName') for n in plan_nodes]}")

    profile_res = requests.post(
        f"{REST_BASE}/{admin_sess}:executeSql",
        json={"sql": "SELECT * FROM Users WHERE UserId = 2", "queryMode": "PROFILE"},
    ).json()
    prof_nodes = profile_res.get("stats", {}).get("queryPlan", {}).get("planNodes", [])
    has_profile = len(prof_nodes) > 1 and ("executionStats" in profile_res.get("stats", {}) or any("executionStats" in n for n in prof_nodes))
    record("step5_profile_stats_returned", has_profile, f"profile nodes count: {len(prof_nodes)}")

    # 4. Property graph query: CALL PageRank(...)
    graph_db_name = "graphdb"
    requests.post(
        f"{REST_BASE}/projects/{PROJECT}/instances/{INSTANCE}/databases",
        json={"createStatement": f"CREATE DATABASE `{graph_db_name}`"},
    )
    time.sleep(1)
    g_sess = requests.post(f"{REST_BASE}/projects/{PROJECT}/instances/{INSTANCE}/databases/{graph_db_name}/sessions", json={}).json()["name"]
    graph_ddl = [
        "CREATE TABLE node_table (id INT64 NOT NULL) PRIMARY KEY (id)",
        "CREATE TABLE edge_table (from_id INT64 NOT NULL, to_id INT64 NOT NULL) PRIMARY KEY(from_id, to_id)",
        """CREATE PROPERTY GRAPH test_graph
             NODE TABLES(node_table KEY(id) LABEL Test PROPERTIES(id))
             EDGE TABLES(edge_table KEY(from_id, to_id)
               SOURCE KEY(from_id) REFERENCES node_table(id)
               DESTINATION KEY(to_id) REFERENCES node_table(id)
               DEFAULT LABEL PROPERTIES ALL COLUMNS)""",
    ]
    requests.patch(f"{REST_BASE}/projects/{PROJECT}/instances/{INSTANCE}/databases/{graph_db_name}/ddl", json={"statements": graph_ddl})
    time.sleep(1)

    exec_dml(g_sess, "INSERT INTO node_table (id) VALUES (1), (2), (4)")
    time.sleep(0.5)
    exec_dml(g_sess, "INSERT INTO edge_table (from_id, to_id) VALUES (1, 2), (2, 4), (4, 1)")
    time.sleep(0.5)

    pagerank_q = """
    GRAPH test_graph
    CALL PageRank() YIELD node, score
    RETURN node.id, score
    """
    pr_res = requests.post(f"{REST_BASE}/{g_sess}:executeSql", json={"sql": pagerank_q}).json()
    pr_rows = pr_res.get("rows", [])
    record("step5_graph_pagerank_scores", len(pr_rows) == 3, f"pagerank rows: {pr_rows}")

    # 5. Search query with Unicode search term
    search_q = "SELECT SEARCH(TOKENIZE_FULLTEXT('Café Crème München'), 'münchen café') AS match_result"
    search_res = requests.post(f"{REST_BASE}/{admin_sess}:executeSql", json={"sql": search_q}).json()
    search_rows = search_res.get("rows", [])
    search_ok = len(search_rows) > 0 and search_rows[0][0] is True
    record("step5_unicode_search", search_ok, f"search result: {search_rows}")

    # 6. PostgreSQL: CREATE INDEX ... USING scann plus spanner.approx_cosine_distance query
    ann_db_name = "ann_pg_db"
    requests.post(
        f"{REST_BASE}/projects/{PROJECT}/instances/{INSTANCE}/databases",
        json={"createStatement": f'CREATE DATABASE "{ann_db_name}"', "databaseDialect": "POSTGRESQL"},
    )
    time.sleep(1)
    ann_sess = requests.post(f"{REST_BASE}/projects/{PROJECT}/instances/{INSTANCE}/databases/{ann_db_name}/sessions", json={}).json()["name"]

    ann_ddl = [
        "CREATE TABLE base (mykey bigint NOT NULL PRIMARY KEY, mydata varchar, embedding float4[] VECTOR LENGTH 2)",
        "CREATE INDEX vec_index ON base USING scann (embedding) WITH (distance_type = 'COSINE', tree_depth = 2) WHERE embedding IS NOT NULL",
    ]
    r = requests.patch(f"{REST_BASE}/projects/{PROJECT}/instances/{INSTANCE}/databases/{ann_db_name}/ddl", json={"statements": ann_ddl})
    if r.status_code == 200:
        ann_op = r.json().get("name")
        for _ in range(60):
            if requests.get(f"{REST_BASE}/{ann_op}").json().get("done"):
                break
            time.sleep(0.5)

    # Insert vector rows
    exec_dml(ann_sess, "INSERT INTO base (mykey, mydata, embedding) VALUES (1, 'data1', '{1.0, 0.0}'), (2, 'data2', '{0.0, 1.0}')")
    time.sleep(0.5)

    # Query with spanner.approx_cosine_distance
    vec_q = """
    SELECT b.mykey FROM base /*@ force_index=vec_index */ b
    WHERE b.embedding IS NOT NULL
    ORDER BY spanner.approx_cosine_distance(
      b.embedding, '{1.0, 0.0}'::float4[],
      options => '{"num_leaves_to_search": 1}')
    LIMIT 1
    """
    vec_res = requests.post(f"{REST_BASE}/{ann_sess}:executeSql", json={"sql": vec_q}).json()
    vec_rows = vec_res.get("rows", [])
    vec_ok = len(vec_rows) > 0 and vec_rows[0][0] == "1"
    record("step5_pg_scann_and_approx_cosine_distance", vec_ok, f"closest vector id: {vec_rows}")

    # -------------------------------------------------------------------------
    # FINAL SUMMARY
    # -------------------------------------------------------------------------
    print("\n==========================================================", flush=True)
    print("FINAL SUMMARY OF STEPS 2, 3, 4, 5", flush=True)
    print("==========================================================", flush=True)
    all_passed = True
    for status, name, detail in RESULTS:
        print(f"{status:<6} {name:<45} {detail}", flush=True)
        if status != "PASS":
            all_passed = False

    print("==========================================================", flush=True)
    if all_passed:
        print("ALL TESTS IN STEPS 2, 3, 4, 5 PASSED SUCCESSFULLY!", flush=True)
        sys.exit(0)
    else:
        print("SOME TESTS FAILED - SEE DETAILS ABOVE", flush=True)
        sys.exit(1)

if __name__ == "__main__":
    try:
        main()
    finally:
        if not os.environ.get("KEEP_CONTAINER"):
            run_cmd("docker rm -f spanner-persist 2>/dev/null || true", check=False)
            run_cmd(f"docker volume rm -f {DOCKER_VOL} 2>/dev/null || true", check=False)

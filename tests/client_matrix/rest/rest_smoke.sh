#!/bin/bash
# REST (gateway) smoke test against the emulator's HTTP port.
B=${REST:-http://localhost:19020}/v1
I=${INSTANCE:-rest-inst}
DBP=projects/p/instances/$I/databases/restdb
PGP=projects/p/instances/$I/databases/restpgdb
RESULTS=()
LAST_BODY=""; LAST_CODE=""

# call METHOD PATH [JSON]
call() {
  local m=$1 p=$2 d=$3 out
  if [ -n "$d" ]; then
    out=$(curl -s -w $'\n%{http_code}' -X "$m" -H 'Content-Type: application/json' "$B/$p" -d "$d")
  else
    out=$(curl -s -w $'\n%{http_code}' -X "$m" "$B/$p")
  fi
  LAST_CODE=${out##*$'\n'}
  LAST_BODY=${out%$'\n'*}
  echo ">>> $m /v1/$p ${d:0:300}" >&2
  echo "<<< HTTP $LAST_CODE $LAST_BODY" | head -c 1500 >&2; echo >&2
}
rec() { # name status detail
  RESULTS+=("$2	$1	$3"); echo "[$2] $1 $3"
}
expect200() { # name detail-jq
  if [ "$LAST_CODE" = "200" ]; then rec "$1" PASS "$(echo "$LAST_BODY" | jq -c "$2" 2>/dev/null)"; else rec "$1" FAIL "HTTP $LAST_CODE $LAST_BODY"; fi
}
expect_err() { # name http_code status_name
  local ok=FAIL
  if [ "$LAST_CODE" = "$2" ] && [ "$(echo "$LAST_BODY" | jq -r '.error.code' 2>/dev/null)" = "$2" ] \
     && [ "$(echo "$LAST_BODY" | jq -r '.error.status' 2>/dev/null)" = "$3" ] \
     && [ -n "$(echo "$LAST_BODY" | jq -r '.error.message // empty' 2>/dev/null)" ]; then ok=PASS; fi
  rec "$1" $ok "HTTP $LAST_CODE body=$LAST_BODY"
}
wait_op() { # op name
  for _ in $(seq 1 60); do
    call GET "$1"
    [ "$(echo "$LAST_BODY" | jq -r '.done // false')" = "true" ] && return 0
    sleep 0.5
  done
  return 1
}

call GET "projects/p/instanceConfigs"
expect200 list_instance_configs '[.instanceConfigs[].name]'

call POST "projects/p/instances" '{"instanceId":"'$I'","instance":{"config":"projects/p/instanceConfigs/emulator-config","displayName":"rest smoke","nodeCount":1}}'
if [ "$LAST_CODE" = "200" ]; then op=$(echo "$LAST_BODY" | jq -r .name); wait_op "$op"; fi
expect200 create_instance '{done, error, name: .response.name}'

call GET "projects/p/instances"
expect200 list_instances '[.instances[].name]'

DDL='CREATE TABLE Items (Id INT64 NOT NULL, Name STRING(MAX), Data JSON, Amount NUMERIC, UpdatedAt TIMESTAMP OPTIONS (allow_commit_timestamp=true), Scores ARRAY<FLOAT64>) PRIMARY KEY (Id)'
call POST "projects/p/instances/$I/databases" "$(jq -nc --arg d "$DDL" '{createStatement:"CREATE DATABASE restdb", extraStatements:[$d]}')"
if [ "$LAST_CODE" = "200" ]; then op=$(echo "$LAST_BODY" | jq -r .name); wait_op "$op"; fi
expect200 create_database '{done, error, dialect: .response.databaseDialect}'

call PATCH "$DBP/ddl" '{"statements":["CREATE INDEX ItemsByName ON Items(Name)"]}'
if [ "$LAST_CODE" = "200" ]; then op=$(echo "$LAST_BODY" | jq -r .name); wait_op "$op"; fi
expect200 update_ddl_index '{done, error}'
call GET "$DBP/ddl"
expect200 get_ddl '.statements|length'

call POST "$DBP/sessions" '{}'
expect200 create_session '.name'
SESS=$(echo "$LAST_BODY" | jq -r .name)

# commit with mutations (single-use transaction)
call POST "$SESS:commit" '{"singleUseTransaction":{"readWrite":{}},"mutations":[{"insert":{"table":"Items","columns":["Id","Name","Data","Amount","UpdatedAt","Scores"],"values":[["1","one","{\"a\":1}","1.50","spanner.commit_timestamp()",[1.0,2.5]],["2","two","{\"b\":[1,2]}","2.25","spanner.commit_timestamp()",[3.0]]]}}]}'
expect200 commit_mutations '.commitTimestamp'

# RW txn: executeSql with inline begin + params, second DML, commit
call POST "$SESS:executeSql" '{"transaction":{"begin":{"readWrite":{}}},"seqno":"1","sql":"INSERT INTO Items (Id, Name, Data, Amount, UpdatedAt, Scores) VALUES (@id, @name, @data, @amt, PENDING_COMMIT_TIMESTAMP(), @scores)","params":{"id":"3","name":"three","data":"{\"c\":true}","amt":"3.125","scores":[0.5,0.25]},"paramTypes":{"id":{"code":"INT64"},"name":{"code":"STRING"},"data":{"code":"JSON"},"amt":{"code":"NUMERIC"},"scores":{"code":"ARRAY","arrayElementType":{"code":"FLOAT64"}}}}'
TX=$(echo "$LAST_BODY" | jq -r '.metadata.transaction.id // empty')
expect200 executeSql_dml_inline_begin '{rows: .stats.rowCountExact, txid: .metadata.transaction.id}'
call POST "$SESS:executeSql" '{"transaction":{"id":"'$TX'"},"seqno":"2","sql":"UPDATE Items SET Name = @name WHERE Id = @id","params":{"id":"1","name":"one-updated"},"paramTypes":{"id":{"code":"INT64"}}}'
expect200 executeSql_dml_update '.stats.rowCountExact'
call POST "$SESS:commit" '{"transactionId":"'$TX'"}'
expect200 commit_txn '.commitTimestamp'

call POST "$SESS:executeSql" '{"sql":"SELECT Id, Name, Data, Amount, UpdatedAt, Scores FROM Items WHERE Id >= @min ORDER BY Id","params":{"min":"1"},"paramTypes":{"min":{"code":"INT64"}}}'
if [ "$LAST_CODE" = "200" ] && [ "$(echo "$LAST_BODY" | jq '.rows|length')" = "3" ]; then rec executeSql_param_query PASS "$(echo "$LAST_BODY" | jq -c '{types:[.metadata.rowType.fields[]|.type.code], row3:.rows[2]}')"; else rec executeSql_param_query FAIL "HTTP $LAST_CODE $LAST_BODY"; fi

call POST "$SESS:beginTransaction" '{"options":{"readOnly":{"strong":true,"returnReadTimestamp":true}}}'
ROTX=$(echo "$LAST_BODY" | jq -r .id)
expect200 begin_readonly_txn '.readTimestamp'
call POST "$SESS:executeSql" '{"transaction":{"id":"'$ROTX'"},"sql":"SELECT COUNT(*) FROM Items"}'
expect200 readonly_snapshot_query '.rows'
call POST "$SESS:read" '{"transaction":{"id":"'$ROTX'"},"table":"Items","index":"ItemsByName","columns":["Id","Name"],"keySet":{"keys":[["one-updated"]]}}'
expect200 readonly_index_read '.rows'

sleep 2
call POST "$SESS:executeSql" '{"transaction":{"singleUse":{"readOnly":{"exactStaleness":"1s","returnReadTimestamp":true}}},"sql":"SELECT COUNT(*) FROM Items"}'
if [ "$LAST_CODE" = "200" ] && [ "$(echo "$LAST_BODY" | jq -r '.rows[0][0]')" = "3" ]; then rec stale_read_exact_staleness PASS "$(echo "$LAST_BODY" | jq -c '{rows, ts: .metadata.transaction.readTimestamp}')"; else rec stale_read_exact_staleness FAIL "HTTP $LAST_CODE $LAST_BODY"; fi

call POST "$SESS:beginTransaction" '{"options":{"readWrite":{}}}'
BTX=$(echo "$LAST_BODY" | jq -r .id)
call POST "$SESS:executeBatchDml" '{"transaction":{"id":"'$BTX'"},"seqno":"3","statements":[{"sql":"INSERT INTO Items (Id, Name) VALUES (@id, @name)","params":{"id":"10","name":"ten"},"paramTypes":{"id":{"code":"INT64"}}},{"sql":"UPDATE Items SET Amount = Amount + 1 WHERE Id IN (1, 2)"}]}'
BD="$LAST_BODY"; BDC=$LAST_CODE
call POST "$SESS:commit" '{"transactionId":"'$BTX'"}'
if [ "$BDC" = "200" ] && [ "$LAST_CODE" = "200" ]; then rec executeBatchDml PASS "$(echo "$BD" | jq -c '{counts:[.resultSets[].stats.rowCountExact], status}')"; else rec executeBatchDml FAIL "HTTP $BDC $BD / commit $LAST_CODE $LAST_BODY"; fi

# --- errors ---
call POST "$SESS:commit" '{"singleUseTransaction":{"readWrite":{}},"mutations":[{"insert":{"table":"Items","columns":["Id","Name"],"values":[["1","dup"]]}}]}'
expect_err error_duplicate_key_mutation 409 ALREADY_EXISTS
call POST "$SESS:executeSql" '{"transaction":{"begin":{"readWrite":{}}},"seqno":"9","sql":"INSERT INTO Items (Id, Name) VALUES (1, '"'dup'"')"}'
expect_err error_duplicate_key_dml 409 ALREADY_EXISTS
call POST "$SESS:executeSql" '{"sql":"SELECT NoSuchColumn FROM Items"}'
expect_err error_invalid_sql 400 INVALID_ARGUMENT
call POST "$DBP/sessions/doesnotexist:executeSql" '{"sql":"SELECT 1"}'
expect_err error_session_not_found 404 NOT_FOUND
call POST "$SESS:executeSql" '{"sql": '
expect_err error_malformed_json 400 INVALID_ARGUMENT
call GET "projects/p/instances/nope"
expect_err error_instance_not_found 404 NOT_FOUND

# --- PostgreSQL dialect ---
call POST "projects/p/instances/$I/databases" '{"createStatement":"CREATE DATABASE \"restpgdb\"","databaseDialect":"POSTGRESQL"}'
if [ "$LAST_CODE" = "200" ]; then op=$(echo "$LAST_BODY" | jq -r .name); wait_op "$op"; fi
expect200 pg_create_database '{done, error, dialect: .response.databaseDialect}'
call PATCH "$PGP/ddl" '{"statements":["CREATE TABLE users (id bigint NOT NULL PRIMARY KEY, name varchar, amount numeric, data jsonb)"]}'
if [ "$LAST_CODE" = "200" ]; then op=$(echo "$LAST_BODY" | jq -r .name); wait_op "$op"; fi
expect200 pg_ddl '{done, error}'
call POST "$PGP/sessions" '{}'
PS=$(echo "$LAST_BODY" | jq -r .name)
call POST "$PS:executeSql" '{"transaction":{"begin":{"readWrite":{}}},"seqno":"1","sql":"INSERT INTO users (id, name, amount, data) VALUES ($1, $2, $3, $4)","params":{"p1":"1","p2":"alice","p3":"9.99","p4":"{\"k\":\"v\"}"},"paramTypes":{"p1":{"code":"INT64"},"p2":{"code":"STRING"},"p3":{"code":"NUMERIC","typeAnnotation":"PG_NUMERIC"},"p4":{"code":"JSON","typeAnnotation":"PG_JSONB"}}}'
PTX=$(echo "$LAST_BODY" | jq -r '.metadata.transaction.id // empty'); PC=$LAST_CODE; PB=$LAST_BODY
call POST "$PS:commit" '{"transactionId":"'$PTX'"}'
if [ "$PC" = "200" ] && [ "$LAST_CODE" = "200" ]; then rec pg_insert_dml_params PASS "rows=$(echo "$PB" | jq -r .stats.rowCountExact)"; else rec pg_insert_dml_params FAIL "HTTP $PC $PB / commit $LAST_CODE $LAST_BODY"; fi
call POST "$PS:executeSql" '{"sql":"SELECT id, name, amount, data FROM users WHERE id = $1","params":{"p1":"1"},"paramTypes":{"p1":{"code":"INT64"}}}'
expect200 pg_param_query '{types:[.metadata.rowType.fields[]|.type], rows}'

call DELETE "$SESS"
expect200 delete_session '.'

echo; echo SUMMARY
printf '%s\n' "${RESULTS[@]}"
printf '%s\n' "${RESULTS[@]}" | grep -q '^FAIL' && exit 1 || exit 0

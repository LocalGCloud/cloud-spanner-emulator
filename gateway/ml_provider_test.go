//
// Copyright 2026 Google LLC
//
// Licensed under the Apache License, Version 2.0 (the "License");
// you may not use this file except in compliance with the License.
// You may obtain a copy of the License at
//
//      http://www.apache.org/licenses/LICENSE-2.0
//
// Unless required by applicable law or agreed to in writing, software
// distributed under the License is distributed on an "AS IS" BASIS,
// WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
// See the License for the specific language governing permissions and
// limitations under the License.
//

package gateway

import (
	"context"
	"encoding/json"
	"fmt"
	"net"
	"net/http"
	"net/http/httptest"
	"os"
	"path/filepath"
	"strings"
	"testing"
	"time"

	databasepb "cloud.google.com/go/spanner/admin/database/apiv1/databasepb"
	instancepb "cloud.google.com/go/spanner/admin/instance/apiv1/instancepb"
	spannerpb "cloud.google.com/go/spanner/apiv1/spannerpb"
	"google.golang.org/grpc"
	"google.golang.org/protobuf/types/known/structpb"
)

// This exercises configured predictions through the public gRPC API and the
// launched emulator binary, including the command-line provider setting.
func TestMLPredictUsesConfiguredProvider(t *testing.T) {
	runfiles := os.Getenv("TEST_SRCDIR")
	if runfiles == "" {
		t.Skip("requires Bazel runfiles for //binaries:emulator_main")
	}
	binary := filepath.Join(runfiles, os.Getenv("TEST_WORKSPACE"), "binaries", "emulator_main")
	if _, err := os.Stat(binary); err != nil {
		t.Fatalf("emulator binary %q: %v", binary, err)
	}

	provider := httptest.NewServer(http.HandlerFunc(func(w http.ResponseWriter, r *http.Request) {
		if r.Method != http.MethodPost || r.URL.Path != "/" {
			http.Error(w, "unexpected request", http.StatusBadRequest)
			return
		}
		var request struct {
			Calls [][]map[string]json.RawMessage `json:"calls"`
		}
		if err := json.NewDecoder(r.Body).Decode(&request); err != nil || len(request.Calls) != 1 || len(request.Calls[0]) < 1 {
			http.Error(w, "invalid prediction request", http.StatusBadRequest)
			return
		}
		var x int64
		if err := json.Unmarshal(request.Calls[0][0]["x"], &x); err != nil {
			http.Error(w, "missing x input", http.StatusBadRequest)
			return
		}
		w.Header().Set("Content-Type", "application/json")
		if x == 13 {
			fmt.Fprint(w, `{"errorMessage":"provider rejected input 13"}`)
			return
		}
		fmt.Fprintf(w, `{"replies":[{"y":%d}]}`, 2*x+1)
	}))
	defer provider.Close()
	providerPort := provider.Listener.Addr().(*net.TCPAddr).Port

	listener, err := net.Listen("tcp", "127.0.0.1:0")
	if err != nil {
		t.Fatal(err)
	}
	address := listener.Addr().String()
	if err := listener.Close(); err != nil {
		t.Fatal(err)
	}
	cmd, exited := startProcess(t, binary, "--host_port", address,
		"--remote_functions_host_port", fmt.Sprintf("localhost:%d", providerPort))
	t.Cleanup(func() {
		stopEmulator(cmd.Process, exited, 5*time.Second)
		waitForExit(t, exited)
	})

	ctx, cancel := context.WithTimeout(context.Background(), 2*time.Minute)
	defer cancel()
	readyCtx, readyCancel := context.WithTimeout(ctx, 30*time.Second)
	defer readyCancel()
	conn, err := grpc.DialContext(readyCtx, address, grpc.WithInsecure(), grpc.WithBlock())
	if err != nil {
		t.Fatalf("connecting to emulator: %v", err)
	}
	defer conn.Close()
	instances := instancepb.NewInstanceAdminClient(conn)
	for {
		_, err = instances.ListInstanceConfigs(readyCtx,
			&instancepb.ListInstanceConfigsRequest{Parent: "projects/ml-provider-test"})
		if err == nil {
			break
		}
		if readyCtx.Err() != nil {
			t.Fatalf("waiting for emulator readiness: %v", err)
		}
		time.Sleep(100 * time.Millisecond)
	}
	instance := "projects/ml-provider-test/instances/ml-provider"
	op, err := instances.CreateInstance(ctx, &instancepb.CreateInstanceRequest{
		Parent:     "projects/ml-provider-test",
		InstanceId: "ml-provider",
		Instance: &instancepb.Instance{
			Config:    "projects/ml-provider-test/instanceConfigs/emulator-config",
			NodeCount: 1,
		},
	})
	if err != nil || !op.GetDone() || op.GetError() != nil {
		t.Fatalf("creating instance: operation=%v error=%v", op, err)
	}
	admin := databasepb.NewDatabaseAdminClient(conn)
	createDatabase := func(id string, dialect databasepb.DatabaseDialect, statements ...string) string {
		t.Helper()
		statement := "CREATE DATABASE " + id
		if dialect == databasepb.DatabaseDialect_POSTGRESQL {
			statement = "CREATE DATABASE \"" + id + "\""
		}
		op, err := admin.CreateDatabase(ctx, &databasepb.CreateDatabaseRequest{
			Parent: instance, CreateStatement: statement,
			ExtraStatements: statements, DatabaseDialect: dialect,
		})
		if err != nil || !op.GetDone() || op.GetError() != nil {
			t.Fatalf("creating %s database: operation=%v error=%v", id, op, err)
		}
		return instance + "/databases/" + id
	}
	client := spannerpb.NewSpannerClient(conn)
	query := func(database, sql string) (*spannerpb.ResultSet, error) {
		t.Helper()
		session, err := client.CreateSession(ctx, &spannerpb.CreateSessionRequest{Database: database})
		if err != nil {
			return nil, err
		}
		return client.ExecuteSql(ctx, &spannerpb.ExecuteSqlRequest{Session: session.GetName(), Sql: sql})
	}

	gsql := createDatabase("ml_gsql", databasepb.DatabaseDialect_GOOGLE_STANDARD_SQL,
		"CREATE TABLE inputs (id INT64 NOT NULL, x INT64) PRIMARY KEY (id)",
		"CREATE MODEL predictor INPUT (x INT64) OUTPUT (y INT64) REMOTE OPTIONS (endpoint = '//aiplatform.googleapis.com/projects/tp/locations/tl/endpoints/te')")
	session, err := client.CreateSession(ctx, &spannerpb.CreateSessionRequest{Database: gsql})
	if err != nil {
		t.Fatalf("creating GoogleSQL session: %v", err)
	}
	_, err = client.Commit(ctx, &spannerpb.CommitRequest{
		Session: session.GetName(),
		Transaction: &spannerpb.CommitRequest_SingleUseTransaction{
			SingleUseTransaction: &spannerpb.TransactionOptions{
				Mode: &spannerpb.TransactionOptions_ReadWrite_{ReadWrite: &spannerpb.TransactionOptions_ReadWrite{}},
			},
		},
		Mutations: []*spannerpb.Mutation{{Operation: &spannerpb.Mutation_Insert{Insert: &spannerpb.Mutation_Write{
			Table: "inputs", Columns: []string{"id", "x"},
			Values: []*structpb.ListValue{
				{Values: []*structpb.Value{structpb.NewStringValue("1"), structpb.NewStringValue("2")}},
				{Values: []*structpb.Value{structpb.NewStringValue("2"), structpb.NewStringValue("9")}},
			},
		}}}},
	})
	if err != nil {
		t.Fatalf("inserting GoogleSQL model inputs: %v", err)
	}
	result, err := query(gsql, "SELECT id, y FROM ML.PREDICT(MODEL predictor, TABLE inputs) ORDER BY id")
	if err != nil {
		t.Fatalf("GoogleSQL ML.PREDICT: %v", err)
	}
	rows := result.GetRows()
	if len(rows) != 2 || len(rows[0].GetValues()) != 2 || len(rows[1].GetValues()) != 2 ||
		rows[0].GetValues()[0].GetStringValue() != "1" ||
		rows[0].GetValues()[1].GetStringValue() != "5" ||
		rows[1].GetValues()[0].GetStringValue() != "2" ||
		rows[1].GetValues()[1].GetStringValue() != "19" {
		t.Fatalf("GoogleSQL provider predictions = %v, want (1,5), (2,19)", rows)
	}

	pg := createDatabase("ml_pg", databasepb.DatabaseDialect_POSTGRESQL)
	result, err = query(pg, `SELECT spanner.ml_predict_row('test-endpoint'::text, '{"instances":[{"x":2},{"x":9}]}'::jsonb)`)
	if err != nil {
		t.Fatalf("PostgreSQL ml_predict_row: %v", err)
	}
	if len(result.GetRows()) != 1 || len(result.GetRows()[0].GetValues()) != 1 {
		t.Fatalf("PostgreSQL provider result = %v", result.GetRows())
	}
	var prediction struct {
		Predictions []struct {
			Y int64 `json:"y"`
		} `json:"predictions"`
	}
	if err := json.Unmarshal([]byte(result.GetRows()[0].GetValues()[0].GetStringValue()), &prediction); err != nil ||
		len(prediction.Predictions) != 2 || prediction.Predictions[0].Y != 5 || prediction.Predictions[1].Y != 19 {
		t.Fatalf("PostgreSQL provider predictions = %v, decode error = %v", prediction, err)
	}
	_, err = query(pg, `SELECT spanner.ml_predict_row('test-endpoint'::text, '{"instances":[{"x":13}]}'::jsonb)`)
	if err == nil || !strings.Contains(err.Error(), "provider rejected input 13") {
		t.Fatalf("PostgreSQL provider error = %v, want rejection without fallback", err)
	}
}

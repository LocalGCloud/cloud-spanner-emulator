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
)

func TestRemoteUDFUsesConfiguredProvider(t *testing.T) {
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
		var body map[string]json.RawMessage
		if err := json.NewDecoder(r.Body).Decode(&body); err != nil {
			http.Error(w, "invalid JSON request", http.StatusBadRequest)
			return
		}
		var calls [][]int64
		var endpoint, function, requestID string
		if err := json.Unmarshal(body["calls"], &calls); err != nil ||
			json.Unmarshal(body["_spanner_endpoint"], &endpoint) != nil ||
			json.Unmarshal(body["_spanner_schema_object"], &function) != nil ||
			json.Unmarshal(body["requestId"], &requestID) != nil || requestID == "" ||
			len(calls) != 1 || len(calls[0]) != 2 || calls[0][1] != 4 ||
			(calls[0][0] != 3 && calls[0][0] != 13) ||
			endpoint != "https://example.com/remote-sum" || function != "remote_sum" {
			http.Error(w, "invalid remote UDF request", http.StatusBadRequest)
			return
		}
		w.Header().Set("Content-Type", "application/json")
		if calls[0][0] == 13 {
			w.WriteHeader(http.StatusBadRequest)
			fmt.Fprint(w, "{\"errorMessage\":\"provider rejected 13\"}")
			return
		}
		fmt.Fprintf(w, "{\"replies\":[%d]}", calls[0][0]+calls[0][1])
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
			&instancepb.ListInstanceConfigsRequest{Parent: "projects/udf-provider-test"})
		if err == nil {
			break
		}
		if readyCtx.Err() != nil {
			t.Fatalf("waiting for emulator readiness: %v", err)
		}
		time.Sleep(100 * time.Millisecond)
	}
	instance := "projects/udf-provider-test/instances/udf-provider"
	op, err := instances.CreateInstance(ctx, &instancepb.CreateInstanceRequest{
		Parent:     "projects/udf-provider-test",
		InstanceId: "udf-provider",
		Instance: &instancepb.Instance{
			Config:    "projects/udf-provider-test/instanceConfigs/emulator-config",
			NodeCount: 1,
		},
	})
	if err != nil || !op.GetDone() || op.GetError() != nil {
		t.Fatalf("creating instance: operation=%v error=%v", op, err)
	}
	database := instance + "/databases/udf_provider"
	admin := databasepb.NewDatabaseAdminClient(conn)
	op, err = admin.CreateDatabase(ctx, &databasepb.CreateDatabaseRequest{
		Parent:          instance,
		CreateStatement: "CREATE DATABASE udf_provider",
		DatabaseDialect: databasepb.DatabaseDialect_GOOGLE_STANDARD_SQL,
		ExtraStatements: []string{
			"CREATE FUNCTION remote_sum(x INT64, y INT64) RETURNS INT64 NOT DETERMINISTIC LANGUAGE REMOTE OPTIONS (endpoint = 'https://example.com/remote-sum')",
		},
	})
	if err != nil || !op.GetDone() || op.GetError() != nil {
		t.Fatalf("creating database with remote UDF: operation=%v error=%v", op, err)
	}

	client := spannerpb.NewSpannerClient(conn)
	session, err := client.CreateSession(ctx, &spannerpb.CreateSessionRequest{Database: database})
	if err != nil {
		t.Fatalf("creating session: %v", err)
	}
	query := func(sql string) (*spannerpb.ResultSet, error) {
		return client.ExecuteSql(ctx, &spannerpb.ExecuteSqlRequest{Session: session.GetName(), Sql: sql})
	}
	result, err := query("SELECT remote_sum(3, 4)")
	if err != nil {
		t.Fatalf("calling remote UDF: %v", err)
	}
	if len(result.GetRows()) != 1 || len(result.GetRows()[0].GetValues()) != 1 ||
		result.GetRows()[0].GetValues()[0].GetStringValue() != "7" {
		t.Fatalf("remote UDF result = %v, want 7", result.GetRows())
	}

	_, err = query("SELECT remote_sum(13, 4)")
	if err == nil || !strings.Contains(err.Error(), "code = FailedPrecondition") ||
		!strings.HasSuffix(err.Error(), "desc = provider rejected 13") {
		t.Fatalf("remote UDF provider error = %v, want FailedPrecondition with provider message", err)
	}
}

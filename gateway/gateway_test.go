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
	"net"
	"net/http"
	"net/http/httptest"
	"net/url"
	"os"
	"os/exec"
	"path/filepath"
	"slices"
	"strings"
	"syscall"
	"testing"
	"time"

	databasepb "cloud.google.com/go/spanner/admin/database/apiv1/databasepb"
	instancepb "cloud.google.com/go/spanner/admin/instance/apiv1/instancepb"
	spannerpb "cloud.google.com/go/spanner/apiv1/spannerpb"
	"github.com/grpc-ecosystem/grpc-gateway/v2/utilities"
	"google.golang.org/grpc"
	"google.golang.org/protobuf/types/known/structpb"
)

func TestEmulatorArgsForwardsRepairCorruptedDatabases(t *testing.T) {
	args := emulatorArgs(Options{
		FrontendAddress:          "localhost:9010",
		DataDir:                  "/data",
		RepairCorruptedDatabases: true,
	})
	if !slices.Contains(args, "--repair_corrupted_databases") {
		t.Errorf("emulatorArgs() = %q, want --repair_corrupted_databases", args)
	}

	args = emulatorArgs(Options{FrontendAddress: "localhost:9010"})
	if slices.Contains(args, "--repair_corrupted_databases") {
		t.Errorf("emulatorArgs() = %q, want no --repair_corrupted_databases", args)
	}
}

func TestEmulatorArgsForwardsSpannerSysExposeOpenInterval(t *testing.T) {
	args := emulatorArgs(Options{FrontendAddress: "localhost:9010"})
	if slices.Contains(args, "--spanner_sys_expose_open_interval") {
		t.Errorf("emulatorArgs() = %q, want no --spanner_sys_expose_open_interval by default", args)
	}
	args = emulatorArgs(Options{FrontendAddress: "localhost:9010", SpannerSysExposeOpenInterval: true})
	if !slices.Contains(args, "--spanner_sys_expose_open_interval") {
		t.Errorf("emulatorArgs() = %q, want --spanner_sys_expose_open_interval", args)
	}
}

func TestEmulatorArgsForwardsDataDir(t *testing.T) {
	args := emulatorArgs(Options{FrontendAddress: "localhost:9010", DataDir: "/data"})
	i := slices.Index(args, "--data_dir")
	if i < 0 || i+1 >= len(args) || args[i+1] != "/data" {
		t.Errorf("emulatorArgs() = %q, want --data_dir /data", args)
	}
}

func TestEmulatorArgsForwardsRemoteFunctionsHostPort(t *testing.T) {
	args := emulatorArgs(Options{FrontendAddress: "localhost:9010"})
	if slices.Contains(args, "--remote_functions_host_port") {
		t.Fatalf("emulatorArgs() = %q, want no remote functions flag by default", args)
	}
	args = emulatorArgs(Options{FrontendAddress: "localhost:9010", RemoteFunctionsHostPort: "localhost:8080"})
	i := slices.Index(args, "--remote_functions_host_port")
	if i < 0 || i+1 >= len(args) || args[i+1] != "localhost:8080" {
		t.Errorf("emulatorArgs() = %q, want --remote_functions_host_port localhost:8080", args)
	}
}

func TestEmulatorArgsForwardsRowDeletionPolicySweepInterval(t *testing.T) {
	args := emulatorArgs(Options{
		FrontendAddress:                       "localhost:9010",
		RowDeletionPolicySweepIntervalSeconds: 5,
	})
	if !slices.Contains(args, "--row_deletion_policy_sweep_interval_seconds=5") {
		t.Errorf("emulatorArgs() = %q, want --row_deletion_policy_sweep_interval_seconds=5", args)
	}
}

func TestEmulatorArgsForwardsLockWaitTimeout(t *testing.T) {
	args := emulatorArgs(Options{FrontendAddress: "localhost:9010", LockWaitTimeoutMs: 250})
	if !slices.Contains(args, "--lock_wait_timeout_ms=250") {
		t.Errorf("emulatorArgs() = %q, want --lock_wait_timeout_ms=250", args)
	}
}

// startProcess starts a command and returns a channel that is closed once the
// process has exited and been reaped.
func startProcess(t *testing.T, name string, args ...string) (*exec.Cmd, <-chan struct{}) {
	t.Helper()
	cmd := exec.Command(name, args...)
	if err := cmd.Start(); err != nil {
		t.Fatalf("starting %s: %v", name, err)
	}
	// Reap by pid rather than with cmd.Wait: after os.Process.Release,
	// cmd.Wait returns at once while the process keeps running.
	pid := cmd.Process.Pid
	exited := make(chan struct{})
	go func() {
		var status syscall.WaitStatus
		for {
			if _, err := syscall.Wait4(pid, &status, 0, nil); err != syscall.EINTR {
				break
			}
		}
		close(exited)
	}()
	t.Cleanup(func() {
		select {
		case <-exited:
		default:
			syscall.Kill(pid, syscall.SIGKILL)
		}
	})
	return cmd, exited
}

func waitForExit(t *testing.T, exited <-chan struct{}) {
	t.Helper()
	select {
	case <-exited:
	case <-time.After(10 * time.Second):
		t.Fatal("emulator process is still running after stopEmulator returned")
	}
}

func TestStopEmulatorStopsTheProcess(t *testing.T) {
	cmd, exited := startProcess(t, "/bin/sleep", "60")
	stopEmulator(cmd.Process, exited, 5*time.Second)
	waitForExit(t, exited)
}

func TestStopEmulatorKillsAProcessThatIgnoresSigterm(t *testing.T) {
	cmd, exited := startProcess(t, "/bin/sh", "-c", "trap '' TERM; exec /bin/sleep 60")
	// Let the shell install its trap before the signal arrives.
	time.Sleep(200 * time.Millisecond)
	stopEmulator(cmd.Process, exited, 200*time.Millisecond)
	waitForExit(t, exited)
}

func emulatorBinary(t *testing.T) string {
	t.Helper()
	runfiles := os.Getenv("TEST_SRCDIR")
	if runfiles == "" {
		t.Skip("requires Bazel runfiles for //binaries:emulator_main")
	}
	binary := filepath.Join(runfiles, os.Getenv("TEST_WORKSPACE"), "binaries", "emulator_main")
	if _, err := os.Stat(binary); err != nil {
		t.Fatalf("emulator binary %q: %v", binary, err)
	}
	return binary
}

func TestRESTGatewayFieldMaskAndLongError(t *testing.T) {
	listener, err := net.Listen("tcp", "127.0.0.1:0")
	if err != nil {
		t.Fatal(err)
	}
	address := listener.Addr().String()
	if err := listener.Close(); err != nil {
		t.Fatal(err)
	}
	cmd, exited := startProcess(t, emulatorBinary(t), "--host_port", address)
	t.Cleanup(func() {
		stopEmulator(cmd.Process, exited, 5*time.Second)
		waitForExit(t, exited)
	})
	ctx, cancel := context.WithTimeout(context.Background(), time.Minute)
	defer cancel()
	conn, err := grpc.DialContext(ctx, address, grpc.WithInsecure(), grpc.WithBlock())
	if err != nil {
		t.Fatal(err)
	}
	defer conn.Close()
	admin := instancepb.NewInstanceAdminClient(conn)
	for {
		_, err = admin.ListInstanceConfigs(ctx, &instancepb.ListInstanceConfigsRequest{Parent: "projects/rest-test"})
		if err == nil {
			break
		}
		if ctx.Err() != nil {
			t.Fatalf("waiting for emulator: %v", err)
		}
		time.Sleep(100 * time.Millisecond)
	}
	instanceOp, err := admin.CreateInstance(ctx, &instancepb.CreateInstanceRequest{
		Parent:     "projects/rest-test",
		InstanceId: "rest-test",
		Instance: &instancepb.Instance{
			Config:    "projects/rest-test/instanceConfigs/emulator-config",
			NodeCount: 1,
		},
	})
	if err != nil || !instanceOp.GetDone() || instanceOp.GetError() != nil {
		t.Fatalf("creating instance: operation=%v error=%v", instanceOp, err)
	}
	const database = "projects/rest-test/instances/rest-test/databases/rest-test"
	dbAdmin := databasepb.NewDatabaseAdminClient(conn)
	databaseOp, err := dbAdmin.CreateDatabase(ctx, &databasepb.CreateDatabaseRequest{
		Parent:          "projects/rest-test/instances/rest-test",
		CreateStatement: "CREATE DATABASE \"rest-test\"",
		DatabaseDialect: databasepb.DatabaseDialect_POSTGRESQL,
	})
	if err != nil || !databaseOp.GetDone() || databaseOp.GetError() != nil {
		t.Fatalf("creating database: operation=%v error=%v", databaseOp, err)
	}
	mux, err := newServeMux(ctx, address)
	if err != nil {
		t.Fatal(err)
	}
	server := httptest.NewServer(mux)
	defer server.Close()
	patch := func(mask string) *http.Response {
		t.Helper()
		request, err := http.NewRequestWithContext(ctx, http.MethodPatch,
			server.URL+"/v1/"+database+"?updateMask="+url.QueryEscape(mask),
			strings.NewReader(`{"enableDropProtection":true}`))
		if err != nil {
			t.Fatal(err)
		}
		request.Header.Set("Content-Type", "application/json")
		response, err := server.Client().Do(request)
		if err != nil {
			t.Fatal(err)
		}
		return response
	}

	response := patch("enableDropProtection")
	response.Body.Close()
	if response.StatusCode != http.StatusOK {
		t.Fatalf("camelCase updateMask HTTP status = %d, want 200", response.StatusCode)
	}
	db, err := dbAdmin.GetDatabase(ctx, &databasepb.GetDatabaseRequest{Name: database})
	if err != nil || !db.GetEnableDropProtection() {
		t.Fatalf("database after camelCase mask = %v, error=%v", db, err)
	}

	longPath := strings.Repeat("x", 2048) + "tail"
	response = patch(longPath)
	defer response.Body.Close()
	var failure struct {
		Error struct {
			Code    int    `json:"code"`
			Message string `json:"message"`
			Status  string `json:"status"`
		} `json:"error"`
	}
	if err := json.NewDecoder(response.Body).Decode(&failure); err != nil {
		t.Fatalf("decoding REST error: %v", err)
	}
	if response.StatusCode != http.StatusBadRequest || failure.Error.Code != http.StatusBadRequest ||
		failure.Error.Status != "INVALID_ARGUMENT" {
		t.Fatalf("invalid mask error = HTTP %d, body code %d status %q, want 400/400/INVALID_ARGUMENT",
			response.StatusCode, failure.Error.Code, failure.Error.Status)
	}
	if !strings.Contains(failure.Error.Message, longPath) {
		t.Errorf("REST error message length = %d, want entire %d-byte field path", len(failure.Error.Message), len(longPath))
	}
}

func TestPostgreSQLUUIDPersistsAcrossProcessRestart(t *testing.T) {
	binary := emulatorBinary(t)

	listener, err := net.Listen("tcp", "127.0.0.1:0")
	if err != nil {
		t.Fatal(err)
	}
	address := listener.Addr().String()
	if err := listener.Close(); err != nil {
		t.Fatal(err)
	}
	dataDir := t.TempDir()
	ctx, cancel := context.WithTimeout(context.Background(), 2*time.Minute)
	defer cancel()

	stop := func(cmd *exec.Cmd, exited <-chan struct{}) {
		select {
		case <-exited:
			return
		default:
		}
		stopEmulator(cmd.Process, exited, 5*time.Second)
		waitForExit(t, exited)
	}
	start := func() (*exec.Cmd, <-chan struct{}, *grpc.ClientConn) {
		cmd, exited := startProcess(t, binary, "--host_port", address, "--data_dir", dataDir)
		t.Cleanup(func() { stop(cmd, exited) })
		readyCtx, readyCancel := context.WithTimeout(ctx, 30*time.Second)
		defer readyCancel()
		conn, err := grpc.DialContext(readyCtx, address, grpc.WithInsecure(), grpc.WithBlock())
		if err != nil {
			t.Fatalf("connecting to emulator: %v", err)
		}
		t.Cleanup(func() { conn.Close() })
		admin := instancepb.NewInstanceAdminClient(conn)
		for {
			_, err = admin.ListInstanceConfigs(readyCtx,
				&instancepb.ListInstanceConfigsRequest{Parent: "projects/uuid-test"})
			if err == nil {
				break
			}
			select {
			case <-readyCtx.Done():
				t.Fatalf("waiting for emulator readiness: %v", err)
			case <-time.After(100 * time.Millisecond):
			}
		}
		return cmd, exited, conn
	}

	const database = "projects/uuid-test/instances/uuid-persist/databases/uuid-persist"
	const id = "9a31411b-caca-4ff1-86e9-39fbd2bc3f39"
	const payload = "12345678-1234-4abc-8def-123456789abc"
	read := func(client spannerpb.SpannerClient) {
		session, err := client.CreateSession(ctx, &spannerpb.CreateSessionRequest{Database: database})
		if err != nil {
			t.Fatalf("creating session: %v", err)
		}
		result, err := client.Read(ctx, &spannerpb.ReadRequest{
			Session: session.GetName(),
			Table:   "uuid_values",
			Columns: []string{"id", "payload"},
			KeySet: &spannerpb.KeySet{Keys: []*structpb.ListValue{
				{Values: []*structpb.Value{structpb.NewStringValue(id)}},
			}},
		})
		if err != nil {
			t.Fatalf("reading UUID key: %v", err)
		}
		fields := result.GetMetadata().GetRowType().GetFields()
		if len(fields) != 2 || fields[0].GetType().GetCode() != spannerpb.TypeCode_UUID ||
			fields[1].GetType().GetCode() != spannerpb.TypeCode_UUID {
			t.Fatalf("UUID read metadata = %v", fields)
		}
		rows := result.GetRows()
		if len(rows) != 1 || len(rows[0].GetValues()) != 2 ||
			rows[0].GetValues()[0].GetStringValue() != id ||
			rows[0].GetValues()[1].GetStringValue() != payload {
			t.Fatalf("UUID read rows = %v", rows)
		}
	}

	firstCmd, firstExited, firstConn := start()
	instanceOp, err := instancepb.NewInstanceAdminClient(firstConn).CreateInstance(ctx,
		&instancepb.CreateInstanceRequest{
			Parent:     "projects/uuid-test",
			InstanceId: "uuid-persist",
			Instance: &instancepb.Instance{
				Config:    "projects/uuid-test/instanceConfigs/emulator-config",
				NodeCount: 1,
			},
		})
	if err != nil || !instanceOp.GetDone() || instanceOp.GetError() != nil {
		t.Fatalf("creating instance: operation=%v error=%v", instanceOp, err)
	}
	databaseOp, err := databasepb.NewDatabaseAdminClient(firstConn).CreateDatabase(ctx,
		&databasepb.CreateDatabaseRequest{
			Parent:          "projects/uuid-test/instances/uuid-persist",
			CreateStatement: "CREATE DATABASE \"uuid-persist\"",
			ExtraStatements: []string{"CREATE TABLE uuid_values (id uuid PRIMARY KEY, payload uuid)"},
			DatabaseDialect: databasepb.DatabaseDialect_POSTGRESQL,
		})
	if err != nil || !databaseOp.GetDone() || databaseOp.GetError() != nil {
		t.Fatalf("creating PostgreSQL database: operation=%v error=%v", databaseOp, err)
	}
	firstClient := spannerpb.NewSpannerClient(firstConn)
	session, err := firstClient.CreateSession(ctx, &spannerpb.CreateSessionRequest{Database: database})
	if err != nil {
		t.Fatalf("creating write session: %v", err)
	}
	_, err = firstClient.Commit(ctx, &spannerpb.CommitRequest{
		Session: session.GetName(),
		Transaction: &spannerpb.CommitRequest_SingleUseTransaction{
			SingleUseTransaction: &spannerpb.TransactionOptions{
				Mode: &spannerpb.TransactionOptions_ReadWrite_{ReadWrite: &spannerpb.TransactionOptions_ReadWrite{}},
			},
		},
		Mutations: []*spannerpb.Mutation{{Operation: &spannerpb.Mutation_Insert{Insert: &spannerpb.Mutation_Write{
			Table:   "uuid_values",
			Columns: []string{"id", "payload"},
			Values: []*structpb.ListValue{{Values: []*structpb.Value{
				structpb.NewStringValue(id), structpb.NewStringValue(payload),
			}}},
		}}}},
	})
	if err != nil {
		t.Fatalf("committing UUID row: %v", err)
	}
	read(firstClient)
	if err := firstConn.Close(); err != nil {
		t.Fatal(err)
	}
	stop(firstCmd, firstExited)

	_, _, secondConn := start()
	read(spannerpb.NewSpannerClient(secondConn))
}

func TestQueryParserConvertsFieldMaskPathsToProtoNames(t *testing.T) {
	for _, tc := range []struct {
		query string
		want  []string
	}{
		// The JSON form that Cloud Spanner's REST API documents.
		{"updateMask=enableDropProtection", []string{"enable_drop_protection"}},
		// Proto field names still work.
		{"updateMask=enable_drop_protection", []string{"enable_drop_protection"}},
		// Several paths, and nested paths.
		{"updateMask=expireTime,encryptionConfig.kmsKeyName", []string{"expire_time", "encryption_config.kms_key_name"}},
		{"update_mask=retentionDuration", []string{"retention_duration"}},
	} {
		values, err := url.ParseQuery(tc.query)
		if err != nil {
			t.Fatal(err)
		}
		request := &databasepb.UpdateDatabaseRequest{}
		if err := newQueryParser().Parse(request, values, utilities.NewDoubleArray(nil)); err != nil {
			t.Fatalf("Parse(%q) failed: %v", tc.query, err)
		}
		if got := request.GetUpdateMask().GetPaths(); !slices.Equal(got, tc.want) {
			t.Errorf("Parse(%q) paths = %q, want %q", tc.query, got, tc.want)
		}
	}
}

//
// Copyright 2020 Google LLC
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

// Package gateway implements a REST gateway for the emulator GRPC service.
package gateway

import (
	"context"
	"fmt"
	"log"
	"net/http"
	"os"
	"os/exec"
	"os/signal"
	"syscall"
	"time"

	// We need this to make sure that the gateway can serialize the google.rpc.ErrorInfo proto.
	_ "google.golang.org/genproto/googleapis/rpc/errdetails"

	"github.com/grpc-ecosystem/grpc-gateway/v2/runtime"
	"google.golang.org/grpc"

	instancepb "cloud.google.com/go/spanner/admin/instance/apiv1/instancepb"
	lrgw "cloud_spanner_emulator/gateway/longrunning_operations_gateway"
	dagw "cloud_spanner_emulator/gateway/spanner_admin_database_gateway"
	spgw "cloud_spanner_emulator/gateway/spanner_gateway"
	iagw "cloud_spanner_emulator/gateway/spanner_admin_instance_gateway"
)

// Options encapsulates options for the emulator gateway.
type Options struct {
	GatewayAddress                                 string
	FrontendBinary                                 string
	FrontendAddress                                string
	CopyEmulatorStdout                             bool
	CopyEmulatorStderr                             bool
	LogRequests                                    bool
	EnableFaultInjection                           bool
	DisableQueryNullFilteredIndexCheck             bool
	EnforcePlacementDmlRestrictions                bool
	RepairCorruptedDatabases                       bool
	OverrideMaxDatabasesPerInstance                int
	OverrideChangeStreamPartitionTokenAliveSeconds int
	DataDir                                        string
}

// Gateway implements the emulator gateway server.
type Gateway struct {
	opts Options
}

// New returns a new gateway.
func New(opts Options) *Gateway {
	return &Gateway{opts}
}

// emulatorArgs returns the flags that the gateway passes to the emulator grpc
// server.
func emulatorArgs(opts Options) []string {
	args := []string{
		"--host_port", opts.FrontendAddress,
	}
	if opts.DataDir != "" {
		args = append(args, "--data_dir", opts.DataDir)
	}
	if opts.RepairCorruptedDatabases {
		args = append(args, "--repair_corrupted_databases")
	}
	if opts.LogRequests {
		args = append(args, "--log_requests")
	}
	if opts.EnableFaultInjection {
		args = append(args, "--enable_fault_injection")
	}
	if opts.DisableQueryNullFilteredIndexCheck {
		args = append(args, "--disable_query_null_filtered_index_check")
	}
	args = append(args,
		fmt.Sprintf("--enforce_placement_dml_restrictions=%t",
			opts.EnforcePlacementDmlRestrictions))
	args = append(args,
		fmt.Sprintf("--override_max_databases_per_instance=%d",
			opts.OverrideMaxDatabasesPerInstance))
	args = append(args,
		fmt.Sprintf("--override_change_stream_partition_token_alive_seconds=%d",
			opts.OverrideChangeStreamPartitionTokenAliveSeconds))
	return args
}

// emulatorStopTimeout is how long the gateway waits for the emulator grpc
// server to exit after SIGTERM before killing it. It's shorter than the 10
// seconds `docker stop` waits before killing the container.
const emulatorStopTimeout = 5 * time.Second

// stopEmulator stops the emulator grpc server process: it sends SIGTERM,
// waits up to timeout for the process to exit, then kills it. exited must be
// closed once the process has exited and been waited for.
func stopEmulator(p *os.Process, exited <-chan struct{}, timeout time.Duration) {
	if err := p.Signal(syscall.SIGTERM); err == nil {
		select {
		case <-exited:
			return
		case <-time.After(timeout):
			log.Printf("Emulator grpc server did not exit within %v; killing it.", timeout)
		}
	}
	p.Kill()
	select {
	case <-exited:
	case <-time.After(timeout):
		log.Println("Emulator grpc server is still running after being killed.")
	}
}

// Run starts the emulator gateway server.
func (gw *Gateway) Run() {
	// Start the emulator grpc server and redirect its output.
	cmd := exec.Command(gw.opts.FrontendBinary, emulatorArgs(gw.opts)...)

	// Proxy emulator log to gateway log.
	if gw.opts.CopyEmulatorStdout {
		cmd.Stdout = os.Stdout
	}
	if gw.opts.CopyEmulatorStderr {
		cmd.Stderr = os.Stderr
	}

	// Start the grpc server but won't block for the grpc server to be up.
	err := cmd.Start()
	if err != nil {
		log.Fatal(err)
	}

	// Stop the grpc server before exiting when the gateway is asked to stop
	// (Ctrl-C, `docker stop`, process managers), so it doesn't keep running
	// with its databases open. Exit the gateway if the grpc server exits.
	// One goroutine handles both, so only one of them calls os.Exit.
	exited := make(chan struct{})
	go func() {
		cmd.Wait()
		close(exited)
	}()
	stop := make(chan os.Signal, 1)
	signal.Notify(stop, os.Interrupt, syscall.SIGTERM)
	go func() {
		select {
		case sig := <-stop:
			log.Printf("Received %v; stopping the emulator grpc server.", sig)
			stopEmulator(cmd.Process, exited, emulatorStopTimeout)
			os.Exit(0)
		case <-exited:
			log.Println("Shutting down gateway server since grpc server is terminated.")
			os.Exit(cmd.ProcessState.ExitCode())
		}
	}()

	// Wait for the grpc server to be up.
	ctx := context.Background()
	addr := gw.opts.FrontendAddress
	if err = waitForReady(ctx, addr); err != nil {
		log.Fatal(fmt.Errorf("Error waiting for emulator to start: %v", err))
	}

	// Setup the gateway services.
	mux := runtime.NewServeMux(
		runtime.WithMarshalerOption(runtime.MIMEWildcard, &runtime.JSONPb{}))
	opts := []grpc.DialOption{grpc.WithInsecure()}
	err = spgw.RegisterSpannerHandlerFromEndpoint(ctx, mux, addr, opts)
	if err != nil {
		log.Fatal(err)
	}
	err = iagw.RegisterInstanceAdminHandlerFromEndpoint(ctx, mux, addr, opts)
	if err != nil {
		log.Fatal(err)
	}
	err = dagw.RegisterDatabaseAdminHandlerFromEndpoint(ctx, mux, addr, opts)
	if err != nil {
		log.Fatal(err)
	}
	err = lrgw.RegisterOperationsHandlerFromEndpoint(ctx, mux, addr, opts)
	if err != nil {
		log.Fatal(err)
	}

	// Start the gateway http server.
	log.Println("Cloud Spanner emulator running.")
	log.Println("REST server listening at", gw.opts.GatewayAddress)
	log.Println("gRPC server listening at", gw.opts.FrontendAddress)
	err = http.ListenAndServe(gw.opts.GatewayAddress, mux)
	if err != nil {
		log.Fatal(err)
	}
}

func waitForReady(ctx context.Context, endpoint string) error {
	timeout := 30 * time.Second
	ctx, cancel := context.WithTimeout(ctx, timeout)
	defer cancel()

	conn, err := grpc.Dial(endpoint, grpc.WithInsecure(), grpc.WithBlock())
	if err != nil {
		return err
	}
	defer conn.Close()

	// To test whether the server is up, wait for ListInstanceConfigs to respond
	// for a dummy project.
	instanceAdminClient := instancepb.NewInstanceAdminClient(conn)
	if _, err = instanceAdminClient.ListInstanceConfigs(ctx,
		&instancepb.ListInstanceConfigsRequest{
			Parent: "projects/test-project",
		}); err != nil {
		return fmt.Errorf("emulator failed to come up at %v within %v deadline: %v",
			endpoint, timeout.String(), err)
	}
	return nil
}

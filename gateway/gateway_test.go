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
	"os/exec"
	"slices"
	"syscall"
	"testing"
	"time"
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

func TestEmulatorArgsForwardsDataDir(t *testing.T) {
	args := emulatorArgs(Options{FrontendAddress: "localhost:9010", DataDir: "/data"})
	i := slices.Index(args, "--data_dir")
	if i < 0 || i+1 >= len(args) || args[i+1] != "/data" {
		t.Errorf("emulatorArgs() = %q, want --data_dir /data", args)
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

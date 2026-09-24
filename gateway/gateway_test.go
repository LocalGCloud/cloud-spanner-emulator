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
	"slices"
	"testing"
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

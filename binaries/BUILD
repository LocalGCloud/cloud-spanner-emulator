#
# Copyright 2020 Google LLC
#
# Licensed under the Apache License, Version 2.0 (the "License");
# you may not use this file except in compliance with the License.
# You may obtain a copy of the License at
#
#     http://www.apache.org/licenses/LICENSE-2.0
#
# Unless required by applicable law or agreed to in writing, software
# distributed under the License is distributed on an "AS IS" BASIS,
# WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
# See the License for the specific language governing permissions and
# limitations under the License.
#

load("@io_bazel_rules_go//go:def.bzl", "go_binary")
load("@rules_cc//cc:cc_binary.bzl", "cc_binary")

package(
    default_visibility = [
        "//:__subpackages__",
    ],
)

licenses(["notice"])

cc_binary(
    name = "emulator_main",
    srcs = ["emulator_main.cc"],
    linkopts = select({
        "@platforms//os:linux": [
            "-static-libgcc",
            "-static-libstdc++",
        ],
        "//conditions:default": [],
    }),
    deps = [
        "//backend/database",
        "//frontend/collections:database_manager",
        "//backend/schema/updater:schema_updater",
        "//common:config",
        "//frontend/common:uris",
        "//frontend/converters:time",
        "//frontend/persistence:backup_catalog",
        "//frontend/persistence:metadata_store",
        "//frontend/entities:operation",
        "//frontend/server",
        "@com_github_grpc_grpc//:grpc++",
        "@com_google_absl//absl/flags:parse",
        "@com_google_absl//absl/strings",
        "@com_google_absl//absl/time",
        "@com_google_googleapis//google/iam/v1:policy_cc_proto",
        "@com_google_googleapis//google/longrunning:longrunning_cc_grpc",
        "@com_google_googleapis//google/spanner/admin/database/v1:database_cc_proto",
        "@com_google_googleapis//google/spanner/admin/instance/v1:instance_cc_proto",
        "@com_google_protobuf//:cc_wkt_protos",
        "@com_google_absl//absl/log:absl_log",
        "@com_google_absl//absl/status",
    ],
)

go_binary(
    name = "gateway_main",
    srcs = ["gateway_main.go"],
    data = [
        ":emulator_main",
    ],
    deps = ["//gateway"],
)

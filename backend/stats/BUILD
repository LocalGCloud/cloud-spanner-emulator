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

load("@rules_cc//cc:cc_library.bzl", "cc_library")
load("@rules_cc//cc:cc_test.bzl", "cc_test")

package(
    default_visibility = ["//:__subpackages__"],
)

licenses(["notice"])

cc_library(
    name = "system_stats",
    srcs = [
        "latency_distribution.cc",
        "operation_stats.cc",
        "spanner_sys_types.cc",
        "system_stats_collector.cc",
    ],
    hdrs = [
        "latency_distribution.h",
        "operation_stats.h",
        "spanner_sys_types.h",
        "system_stats_collector.h",
    ],
    deps = [
        "//backend/access:write",
        "//backend/common:ids",
        "//backend/datamodel:key",
        "//backend/datamodel:key_range",
        "//backend/schema/catalog:schema",
        "//common:constants",
        "@com_google_absl//absl/base:core_headers",
        "@com_google_absl//absl/base:no_destructor",
        "@com_google_absl//absl/container:btree",
        "@com_google_absl//absl/container:flat_hash_map",
        "@com_google_absl//absl/log",
        "@com_google_absl//absl/log:check",
        "@com_google_absl//absl/status",
        "@com_google_absl//absl/strings",
        "@com_google_absl//absl/synchronization",
        "@com_google_absl//absl/time",
        "@com_google_absl//absl/types:span",
        "@com_google_farmhash//:farmhash_fingerprint",
        "@com_google_googlesql//googlesql/public:type",
        "@com_google_googlesql//googlesql/public:value",
        "@json",
    ],
)

cc_test(
    name = "system_stats_test",
    srcs = [
        "latency_distribution_test.cc",
        "operation_stats_test.cc",
        "system_stats_collector_test.cc",
    ],
    deps = [
        ":system_stats",
        "//backend/access:write",
        "//backend/datamodel:key",
        "@com_google_absl//absl/status",
        "@com_google_absl//absl/strings",
        "@com_google_absl//absl/time",
        "@com_google_googlesql//googlesql/public:value",
        "@com_google_googletest//:gtest_main",
        "@json",
    ],
)

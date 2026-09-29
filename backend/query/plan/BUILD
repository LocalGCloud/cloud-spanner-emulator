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
    name = "query_plan",
    srcs = [
        "query_plan_builder.cc",
        "scan_profiler.cc",
    ],
    hdrs = [
        "query_plan_builder.h",
        "scan_profiler.h",
    ],
    deps = [
        "@com_google_absl//absl/container:flat_hash_map",
        "@com_google_absl//absl/container:flat_hash_set",
        "@com_google_absl//absl/status",
        "@com_google_absl//absl/status:statusor",
        "@com_google_absl//absl/strings",
        "@com_google_absl//absl/strings:str_format",
        "@com_google_absl//absl/time",
        "@com_google_absl//absl/types:span",
        "@com_google_googleapis//google/spanner/v1:spanner_cc_proto",
        "@com_google_googlesql//googlesql/base:status",
        "@com_google_googlesql//googlesql/public:catalog",
        "@com_google_googlesql//googlesql/public:evaluator_table_iterator",
        "@com_google_googlesql//googlesql/public:function",
        "@com_google_googlesql//googlesql/public:options_cc_proto",
        "@com_google_googlesql//googlesql/public:type",
        "@com_google_googlesql//googlesql/public:value",
        "@com_google_googlesql//googlesql/resolved_ast",
        "@com_google_protobuf//:cc_wkt_protos",
    ],
)

cc_test(
    name = "query_plan_builder_test",
    srcs = ["query_plan_builder_test.cc"],
    deps = [
        ":query_plan",
        "//backend/query:analyzer_options",
        "//backend/query:catalog",
        "//backend/query:function_catalog",
        "//backend/schema/catalog:schema",
        "//tests/common:proto_matchers",
        "//tests/common:test_schema_constructor",
        "@com_google_absl//absl/strings",
        "@com_google_absl//absl/time",
        "@com_google_googleapis//google/spanner/v1:spanner_cc_proto",
        "@com_google_googlesql//googlesql/base/testing:status_matchers",
        "@com_google_googlesql//googlesql/public:analyzer",
        "@com_google_googlesql//googlesql/public:analyzer_options",
        "@com_google_googlesql//googlesql/public:type",
        "@com_google_googlesql//googlesql/resolved_ast",
        "@com_google_googletest//:gtest_main",
        "@com_google_protobuf//:cc_wkt_protos",
    ],
)

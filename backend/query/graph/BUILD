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
    name = "graph_algorithms",
    srcs = ["graph_algorithms.cc"],
    hdrs = ["graph_algorithms.h"],
    deps = [
        "@com_google_absl//absl/container:btree",
        "@com_google_absl//absl/container:flat_hash_map",
    ],
)

cc_test(
    name = "graph_algorithms_test",
    srcs = ["graph_algorithms_test.cc"],
    deps = [
        ":graph_algorithms",
        "@com_google_googletest//:gtest_main",
    ],
)

cc_library(
    name = "graph_algorithm_table_valued_function",
    srcs = ["graph_algorithm_table_valued_function.cc"],
    hdrs = ["graph_algorithm_table_valued_function.h"],
    deps = [
        ":graph_algorithms",
        "//backend/common:case",
        "//backend/query:analyzer_options",
        "@com_google_absl//absl/container:flat_hash_map",
        "@com_google_absl//absl/container:flat_hash_set",
        "@com_google_absl//absl/status",
        "@com_google_absl//absl/status:statusor",
        "@com_google_absl//absl/strings",
        "@com_google_absl//absl/strings:string_view",
        "@com_google_absl//absl/time",
        "@com_google_googlesql//googlesql/base:status",
        "@com_google_googlesql//googlesql/parser",
        "@com_google_googlesql//googlesql/public:analyzer",
        "@com_google_googlesql//googlesql/public:analyzer_options",
        "@com_google_googlesql//googlesql/public:analyzer_output",
        "@com_google_googlesql//googlesql/public:catalog",
        "@com_google_googlesql//googlesql/public:evaluator",
        "@com_google_googlesql//googlesql/public:evaluator_table_iterator",
        "@com_google_googlesql//googlesql/public:function",
        "@com_google_googlesql//googlesql/public:language_options",
        "@com_google_googlesql//googlesql/public:options_cc_proto",
        "@com_google_googlesql//googlesql/public:simple_catalog",
        "@com_google_googlesql//googlesql/public:type",
        "@com_google_googlesql//googlesql/public:value",
        "@com_google_googlesql//googlesql/resolved_ast",
    ],
)

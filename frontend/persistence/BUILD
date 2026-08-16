load("@rules_cc//cc:cc_library.bzl", "cc_library")
load("@rules_cc//cc:cc_test.bzl", "cc_test")

package(default_visibility = ["//:__subpackages__"])

cc_library(
    name = "atomic_file",
    srcs = ["atomic_file.cc"],
    hdrs = ["atomic_file.h"],
    deps = [
        "@com_google_absl//absl/status",
        "@com_google_absl//absl/status:statusor",
        "@com_google_absl//absl/strings",
    ],
)

cc_library(
    name = "schema_change_batch",
    hdrs = ["schema_change_batch.h"],
)

cc_library(
    name = "backup_catalog",
    srcs = ["backup_catalog.cc"],
    hdrs = ["backup_catalog.h"],
    deps = [
        ":atomic_file",
        ":schema_change_batch",
        "@com_google_absl//absl/status",
        "@com_google_absl//absl/status:statusor",
        "@com_google_absl//absl/strings",
        "@com_google_absl//absl/synchronization",
        "@com_google_googleapis//google/longrunning:longrunning_cc_grpc",
        "@com_google_googleapis//google/spanner/admin/database/v1:database_cc_proto",
        "@com_google_leveldb//:leveldb",
        "@com_google_googlesql//googlesql/base:status",
        "@json",
    ],
)

cc_test(
    name = "backup_catalog_test",
    srcs = ["backup_catalog_test.cc"],
    deps = [
        ":backup_catalog",
        "@com_google_absl//absl/time",
        "@com_google_googleapis//google/longrunning:longrunning_cc_grpc",
        "@com_google_googleapis//google/spanner/admin/database/v1:database_cc_proto",
        "@com_google_leveldb//:leveldb",
        "@com_google_googlesql//googlesql/base/testing:status_matchers",
        "@com_google_googletest//:gtest_main",
        "@json",
    ],
)

cc_library(
    name = "metadata_store",
    srcs = ["metadata_store.cc"],
    hdrs = ["metadata_store.h"],
    deps = [
        ":backup_catalog",
        ":atomic_file",
        ":schema_change_batch",
        "@com_google_absl//absl/status",
        "@com_google_absl//absl/strings",
        "@com_google_absl//absl/synchronization",
        "@com_google_absl//absl/time",
        "@com_google_absl//absl/log:absl_log",
        "@com_google_googleapis//google/iam/v1:policy_cc_proto",
        "@com_google_googleapis//google/longrunning:longrunning_cc_proto",
        "@com_google_googleapis//google/spanner/admin/instance/v1:instance_cc_proto",
        "@json",
    ],
)

cc_test(
    name = "metadata_store_test",
    srcs = ["metadata_store_test.cc"],
    deps = [
        ":backup_catalog",
        ":metadata_store",
        "@com_google_absl//absl/status",
        "@com_google_absl//absl/time",
        "@com_google_googlesql//googlesql/base/testing:status_matchers",
        "@com_google_googleapis//google/iam/v1:policy_cc_proto",
        "@com_google_googleapis//google/longrunning:longrunning_cc_proto",
        "@com_google_googleapis//google/spanner/admin/instance/v1:instance_cc_proto",
        "@json",
        "@com_google_googletest//:gtest_main",
    ],
)

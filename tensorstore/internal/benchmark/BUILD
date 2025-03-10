load("//bazel:tensorstore.bzl", "tensorstore_cc_binary", "tensorstore_cc_library")

package(default_visibility = ["//visibility:public"])

licenses(["notice"])

tensorstore_cc_library(
    name = "metric_utils",
    srcs = ["metric_utils.cc"],
    hdrs = ["metric_utils.h"],
    deps = [
        "//tensorstore/internal/json",
        "//tensorstore/internal/metrics:collect",
        "//tensorstore/internal/metrics:registry",
        "//tensorstore/kvstore",
        "//tensorstore/util:result",
        "@com_github_nlohmann_json//:json",
        "@com_google_absl//absl/status",
        "@com_google_absl//absl/strings:cord",
    ],
)

tensorstore_cc_library(
    name = "vector_flag",
    hdrs = ["vector_flag.h"],
    deps = [
        "@com_google_absl//absl/flags:marshalling",
        "@com_google_absl//absl/strings",
        "@com_google_absl//absl/strings:string_view",
    ],
)

tensorstore_cc_binary(
    name = "ts_benchmark",
    testonly = True,
    srcs = ["ts_benchmark.cc"],
    deps = [
        ":metric_utils",
        ":vector_flag",
        "//tensorstore:all_drivers",
        "//tensorstore:context",
        "//tensorstore:index",
        "//tensorstore:spec",
        "//tensorstore/driver:driver_testutil",
        "//tensorstore/kvstore:all_drivers",
        "//tensorstore/util:json_absl_flag",
        "//tensorstore/util:result",
        "//tensorstore/util:status",
        "@com_google_absl//absl/flags:flag",
        "@com_google_absl//absl/flags:parse",
        "@com_google_absl//absl/log:absl_log",
        "@com_google_absl//absl/random",
        "@com_google_absl//absl/strings",
    ],
)

tensorstore_cc_binary(
    name = "kvstore_benchmark",
    srcs = ["kvstore_benchmark.cc"],
    deps = [
        ":metric_utils",
        "//tensorstore:context",
        "//tensorstore/internal:path",
        "//tensorstore/internal/metrics",
        "//tensorstore/internal/metrics:metadata",
        "//tensorstore/internal/metrics:registry",
        "//tensorstore/kvstore",
        "//tensorstore/kvstore:all_drivers",
        "//tensorstore/kvstore:generation",
        "//tensorstore/util:division",
        "//tensorstore/util:future",
        "//tensorstore/util:json_absl_flag",
        "//tensorstore/util:result",
        "//tensorstore/util:status",
        "@com_github_nlohmann_json//:json",
        "@com_google_absl//absl/flags:flag",
        "@com_google_absl//absl/flags:parse",
        "@com_google_absl//absl/log:absl_check",
        "@com_google_absl//absl/log:absl_log",
        "@com_google_absl//absl/random",
        "@com_google_absl//absl/status",
        "@com_google_absl//absl/strings",
        "@com_google_absl//absl/strings:cord",
        "@com_google_absl//absl/strings:str_format",
        "@com_google_absl//absl/time",
    ],
)

tensorstore_cc_binary(
    name = "kvstore_duration",
    srcs = ["kvstore_duration.cc"],
    deps = [
        ":metric_utils",
        "//tensorstore:context",
        "//tensorstore/internal:intrusive_ptr",
        "//tensorstore/internal:path",
        "//tensorstore/internal/metrics",
        "//tensorstore/internal/metrics:metadata",
        "//tensorstore/kvstore",
        "//tensorstore/kvstore:all_drivers",
        "//tensorstore/util:future",
        "//tensorstore/util:json_absl_flag",
        "//tensorstore/util:result",
        "//tensorstore/util:status",
        "@com_google_absl//absl/flags:flag",
        "@com_google_absl//absl/flags:parse",
        "@com_google_absl//absl/log:absl_check",
        "@com_google_absl//absl/log:absl_log",
        "@com_google_absl//absl/random",
        "@com_google_absl//absl/status",
        "@com_google_absl//absl/strings",
        "@com_google_absl//absl/strings:str_format",
        "@com_google_absl//absl/synchronization",
        "@com_google_absl//absl/time",
    ],
)

tensorstore_cc_library(
    name = "multi_spec",
    srcs = ["multi_spec.cc"],
    hdrs = ["multi_spec.h"],
    deps = [
        "//tensorstore:json_serialization_options_base",
        "//tensorstore:spec",
        "//tensorstore/internal/json_binding",
        "//tensorstore/internal/json_binding:bindable",
        "@com_github_nlohmann_json//:json",
        "@com_google_absl//absl/log:absl_log",
        "@com_google_riegeli//riegeli/bytes:fd_reader",
        "@com_google_riegeli//riegeli/bytes:read_all",
    ],
)

tensorstore_cc_binary(
    name = "multi_read_benchmark",
    testonly = True,
    srcs = ["multi_read_benchmark.cc"],
    deps = [
        ":metric_utils",
        ":multi_spec",
        "//tensorstore",
        "//tensorstore:all_drivers",
        "//tensorstore:array",
        "//tensorstore:context",
        "//tensorstore:open",
        "//tensorstore:open_mode",
        "//tensorstore:spec",
        "//tensorstore/internal/json_binding",
        "//tensorstore/internal/metrics",
        "//tensorstore/internal/metrics:metadata",
        "//tensorstore/internal/os:file_util",
        "//tensorstore/util:future",
        "//tensorstore/util:json_absl_flag",
        "//tensorstore/util:result",
        "//tensorstore/util:span",
        "@com_github_nlohmann_json//:json",
        "@com_google_absl//absl/flags:flag",
        "@com_google_absl//absl/flags:parse",
        "@com_google_absl//absl/log",
        "@com_google_absl//absl/log:absl_check",
        "@com_google_absl//absl/log:absl_log",
        "@com_google_absl//absl/status",
        "@com_google_absl//absl/strings",
        "@com_google_absl//absl/strings:str_format",
        "@com_google_absl//absl/strings:string_view",
        "@com_google_absl//absl/synchronization",
        "@com_google_absl//absl/time",
    ],
)

tensorstore_cc_binary(
    name = "multi_write_benchmark",
    testonly = True,
    srcs = ["multi_write_benchmark.cc"],
    deps = [
        ":metric_utils",
        ":multi_spec",
        "//tensorstore",
        "//tensorstore:all_drivers",
        "//tensorstore:array",
        "//tensorstore:box",
        "//tensorstore:chunk_layout",
        "//tensorstore:context",
        "//tensorstore:index",
        "//tensorstore:open",
        "//tensorstore:open_mode",
        "//tensorstore:schema",
        "//tensorstore:spec",
        "//tensorstore/index_space:dim_expression",
        "//tensorstore/internal:data_type_random_generator",
        "//tensorstore/internal/json_binding:bindable",
        "//tensorstore/internal/metrics",
        "//tensorstore/internal/metrics:metadata",
        "//tensorstore/kvstore",
        "//tensorstore/util:division",
        "//tensorstore/util:future",
        "//tensorstore/util:iterate",
        "//tensorstore/util:json_absl_flag",
        "//tensorstore/util:result",
        "//tensorstore/util:span",
        "//tensorstore/util:status",
        "@com_github_nlohmann_json//:json",
        "@com_google_absl//absl/container:flat_hash_map",
        "@com_google_absl//absl/flags:flag",
        "@com_google_absl//absl/flags:parse",
        "@com_google_absl//absl/hash",
        "@com_google_absl//absl/log:absl_check",
        "@com_google_absl//absl/log:absl_log",
        "@com_google_absl//absl/random",
        "@com_google_absl//absl/status",
        "@com_google_absl//absl/strings:str_format",
        "@com_google_absl//absl/strings:string_view",
        "@com_google_absl//absl/time",
    ],
)

tensorstore_cc_binary(
    name = "multi_genspec",
    srcs = ["multi_genspec.cc"],
    deps = [
        ":vector_flag",
        "//tensorstore",
        "//tensorstore:all_drivers",
        "//tensorstore:array_storage_statistics",
        "//tensorstore:box",
        "//tensorstore:chunk_layout",
        "//tensorstore:context",
        "//tensorstore:data_type",
        "//tensorstore:index",
        "//tensorstore:json_serialization_options_base",
        "//tensorstore:open",
        "//tensorstore:open_mode",
        "//tensorstore:schema",
        "//tensorstore:spec",
        "//tensorstore/index_space:dim_expression",
        "//tensorstore/internal/json_binding",
        "//tensorstore/internal/json_binding:bindable",
        "//tensorstore/kvstore",
        "//tensorstore/util:division",
        "//tensorstore/util:future",
        "//tensorstore/util:iterate",
        "//tensorstore/util:json_absl_flag",
        "//tensorstore/util:result",
        "//tensorstore/util:span",
        "//tensorstore/util:status",
        "@com_github_nlohmann_json//:json",
        "@com_google_absl//absl/flags:flag",
        "@com_google_absl//absl/flags:parse",
        "@com_google_absl//absl/log",
        "@com_google_absl//absl/log:absl_check",
        "@com_google_absl//absl/log:absl_log",
        "@com_google_absl//absl/status",
        "@com_google_absl//absl/strings:string_view",
        "@com_google_absl//absl/synchronization",
        "@com_google_riegeli//riegeli/bytes:fd_reader",
        "@com_google_riegeli//riegeli/bytes:read_all",
    ],
)

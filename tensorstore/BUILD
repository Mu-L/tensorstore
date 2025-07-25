load("//bazel:non_compile.bzl", "cc_with_non_compile_test")
load("//bazel:tensorstore.bzl", "tensorstore_cc_library", "tensorstore_cc_test")

package(default_visibility = ["//visibility:public"])

licenses(["notice"])

exports_files(["LICENSE"])

tensorstore_cc_library(
    name = "array",
    srcs = [
        "array.cc",
        "make_array.inc",
    ],
    hdrs = ["array.h"],
    deps = [
        ":box",
        ":container_kind",
        ":contiguous_layout",
        ":data_type",
        ":index",
        ":rank",
        ":static_cast",
        ":strided_layout",
        "//tensorstore/internal:element_copy_function",
        "//tensorstore/internal:elementwise_function",
        "//tensorstore/internal:integer_overflow",
        "//tensorstore/internal:unaligned_data_type_functions",
        "//tensorstore/internal/meta",
        "//tensorstore/internal/meta:attributes",
        "//tensorstore/internal/meta:type_traits",
        "//tensorstore/internal/meta:void_wrapper",
        "//tensorstore/serialization",
        "//tensorstore/util:byte_strided_pointer",
        "//tensorstore/util:dimension_set",
        "//tensorstore/util:element_pointer",
        "//tensorstore/util:element_traits",
        "//tensorstore/util:extents",
        "//tensorstore/util:iterate",
        "//tensorstore/util:result",
        "//tensorstore/util:span",
        "//tensorstore/util:status",
        "//tensorstore/util:str_cat",
        "//tensorstore/util/garbage_collection",
        "@abseil-cpp//absl/log:absl_check",
        "@abseil-cpp//absl/meta:type_traits",
        "@abseil-cpp//absl/status",
        "@riegeli//riegeli/varint:varint_reading",
        "@riegeli//riegeli/varint:varint_writing",
    ],
)

cc_with_non_compile_test(
    name = "array_nc_test",
    srcs = ["array_nc_test.cc"],
    nc_test_shard_count = 1,
    tags = ["nctest"],
    deps = [
        ":array",
        ":index",
        "//tensorstore/util:span",
    ],
)

tensorstore_cc_test(
    name = "array_test",
    size = "small",
    srcs = ["array_test.cc"],
    deps = [
        ":array",
        ":box",
        ":container_kind",
        ":contiguous_layout",
        ":data_type",
        ":index",
        ":rank",
        ":static_cast",
        ":strided_layout",
        "//tensorstore/index_space:index_transform_testutil",
        "//tensorstore/internal:data_type_random_generator",
        "//tensorstore/internal/testing:random_seed",
        "//tensorstore/serialization",
        "//tensorstore/serialization:batch",
        "//tensorstore/serialization:test_util",
        "//tensorstore/util:element_pointer",
        "//tensorstore/util:iterate",
        "//tensorstore/util:result",
        "//tensorstore/util:span",
        "//tensorstore/util:status_testutil",
        "//tensorstore/util:str_cat",
        "@abseil-cpp//absl/random:bit_gen_ref",
        "@abseil-cpp//absl/status",
        "@googletest//:gtest_main",
    ],
)

tensorstore_cc_library(
    name = "array_testutil",
    testonly = 1,
    srcs = [
        "array_testutil.cc",
        "array_testutil_matches_array.inc",
    ],
    hdrs = ["array_testutil.h"],
    deps = [
        ":array",
        ":container_kind",
        ":data_type",
        ":index",
        ":rank",
        ":static_cast",
        ":strided_layout",
        "//tensorstore/util:iterate_over_index_range",
        "//tensorstore/util:span",
        "@googletest//:gtest",
    ],
)

tensorstore_cc_test(
    name = "array_testutil_test",
    size = "small",
    srcs = ["array_testutil_test.cc"],
    deps = [
        ":array_testutil",
        "//tensorstore/util:span",
        "@googletest//:gtest_main",
    ],
)

tensorstore_cc_library(
    name = "array_storage_statistics",
    srcs = ["array_storage_statistics.cc"],
    hdrs = ["array_storage_statistics.h"],
    deps = [
        ":batch",
        "@abseil-cpp//absl/meta:type_traits",
        "@abseil-cpp//absl/status",
    ],
)

tensorstore_cc_library(
    name = "auto",
    hdrs = ["auto.h"],
    deps = [
        ":spec",
        "//tensorstore/driver/auto",
        "//tensorstore/kvstore",
    ],
)

tensorstore_cc_library(
    name = "batch",
    srcs = ["batch.cc"],
    hdrs = [
        "batch.h",
        "batch_impl.h",
    ],
    deps = [
        "//tensorstore/internal:intrusive_ptr",
        "//tensorstore/internal/container:hash_set_of_any",
        "//tensorstore/internal/container:intrusive_red_black_tree",
        "@abseil-cpp//absl/synchronization",
        "@abseil-cpp//absl/types:compare",
    ],
)

tensorstore_cc_test(
    name = "batch_test",
    size = "small",
    srcs = ["batch_test.cc"],
    deps = [
        ":batch",
        "@abseil-cpp//absl/strings",
        "@abseil-cpp//absl/strings:str_format",
        "@googletest//:gtest_main",
    ],
)

tensorstore_cc_library(
    name = "box",
    srcs = ["box.cc"],
    hdrs = ["box.h"],
    deps = [
        ":index",
        ":index_interval",
        ":rank",
        ":static_cast",
        "//tensorstore/internal:lldb_scripting",
        "//tensorstore/internal/container:multi_vector",
        "//tensorstore/internal/container:multi_vector_view",
        "//tensorstore/internal/meta:attributes",
        "//tensorstore/internal/meta:type_traits",
        "//tensorstore/serialization",
        "//tensorstore/util:constant_vector",
        "//tensorstore/util:extents",
        "//tensorstore/util:span",
        "//tensorstore/util:str_cat",
        "//tensorstore/util/garbage_collection",
        "@abseil-cpp//absl/meta:type_traits",
        "@abseil-cpp//absl/status",
    ],
)

tensorstore_cc_test(
    name = "box_test",
    size = "small",
    srcs = ["box_test.cc"],
    deps = [
        ":box",
        ":index",
        ":index_interval",
        ":rank",
        ":static_cast",
        "//tensorstore/serialization",
        "//tensorstore/serialization:test_util",
        "//tensorstore/util:span",
        "//tensorstore/util:status_testutil",
        "//tensorstore/util:str_cat",
        "@abseil-cpp//absl/status",
        "@googletest//:gtest_main",
    ],
)

tensorstore_cc_library(
    name = "cast",
    hdrs = ["cast.h"],
    deps = [
        ":data_type",
        ":index",
        ":open_mode",
        ":tensorstore",
        "//tensorstore/driver/cast",
        "//tensorstore/util:result",
    ],
)

tensorstore_cc_library(
    name = "chunk_layout",
    srcs = ["chunk_layout.cc"],
    hdrs = ["chunk_layout.h"],
    deps = [
        ":box",
        ":contiguous_layout",
        ":index",
        ":index_interval",
        ":json_serialization_options",
        ":json_serialization_options_base",
        ":rank",
        "//tensorstore/index_space:index_transform",
        "//tensorstore/index_space:output_index_method",
        "//tensorstore/internal:integer_overflow",
        "//tensorstore/internal:intrusive_ptr",
        "//tensorstore/internal/json_binding",
        "//tensorstore/internal/json_binding:bindable",
        "//tensorstore/internal/json_binding:dimension_indexed",
        "//tensorstore/internal/meta:integer_range",
        "//tensorstore/internal/meta:type_traits",
        "//tensorstore/serialization",
        "//tensorstore/serialization:json",
        "//tensorstore/util:dimension_set",
        "//tensorstore/util:division",
        "//tensorstore/util:maybe_hard_constraint",
        "//tensorstore/util:result",
        "//tensorstore/util:small_bit_set",
        "//tensorstore/util:span",
        "//tensorstore/util:status",
        "//tensorstore/util:str_cat",
        "//tensorstore/util/garbage_collection",
        "@abseil-cpp//absl/base:core_headers",
        "@abseil-cpp//absl/functional:function_ref",
        "@abseil-cpp//absl/meta:type_traits",
        "@abseil-cpp//absl/status",
    ],
)

tensorstore_cc_test(
    name = "chunk_layout_test",
    size = "small",
    srcs = ["chunk_layout_test.cc"],
    deps = [
        ":box",
        ":chunk_layout",
        ":index",
        ":json_serialization_options_base",
        ":rank",
        "//tensorstore/index_space:dim_expression",
        "//tensorstore/index_space:index_transform",
        "//tensorstore/index_space:index_transform_testutil",
        "//tensorstore/index_space:output_index_method",
        "//tensorstore/internal/json_binding:bindable",
        "//tensorstore/internal/json_binding:gtest",
        "//tensorstore/internal/testing:json_gtest",
        "//tensorstore/internal/testing:random_seed",
        "//tensorstore/serialization",
        "//tensorstore/serialization:test_util",
        "//tensorstore/util:dimension_set",
        "//tensorstore/util:division",
        "//tensorstore/util:span",
        "//tensorstore/util:status",
        "//tensorstore/util:status_testutil",
        "//tensorstore/util:str_cat",
        "@abseil-cpp//absl/container:flat_hash_map",
        "@abseil-cpp//absl/random",
        "@abseil-cpp//absl/random:bit_gen_ref",
        "@abseil-cpp//absl/status",
        "@googletest//:gtest_main",
        "@nlohmann_json//:json",
    ],
)

exports_files(
    ["virtual_chunked.h"],
    visibility = [":internal_packages"],
)

alias(
    name = "virtual_chunked",
    actual = "//tensorstore/driver/virtual_chunked",
)

tensorstore_cc_library(
    name = "container_kind",
    hdrs = ["container_kind.h"],
)

tensorstore_cc_library(
    name = "context",
    srcs = [
        "context.cc",
        "context_impl_base.h",
    ],
    hdrs = [
        "context.h",
        "context_impl.h",
        "context_resource_provider.h",
    ],
    deps = [
        ":json_serialization_options",
        ":json_serialization_options_base",
        "//tensorstore/internal:intrusive_ptr",
        "//tensorstore/internal:mutex",
        "//tensorstore/internal:tagged_ptr",
        "//tensorstore/internal/cache_key",
        "//tensorstore/internal/container:heterogeneous_container",
        "//tensorstore/internal/json:same",
        "//tensorstore/internal/json:value_as",
        "//tensorstore/internal/json_binding",
        "//tensorstore/internal/json_binding:bindable",
        "//tensorstore/internal/meta:type_traits",
        "//tensorstore/internal/riegeli:delimited",
        "//tensorstore/serialization",
        "//tensorstore/serialization:json",
        "//tensorstore/util:quote_string",
        "//tensorstore/util:result",
        "//tensorstore/util:status",
        "//tensorstore/util:str_cat",
        "//tensorstore/util/garbage_collection",
        "@abseil-cpp//absl/base:core_headers",
        "@abseil-cpp//absl/base:no_destructor",
        "@abseil-cpp//absl/container:flat_hash_map",
        "@abseil-cpp//absl/log:absl_check",
        "@abseil-cpp//absl/log:absl_log",
        "@abseil-cpp//absl/meta:type_traits",
        "@abseil-cpp//absl/status",
        "@abseil-cpp//absl/strings",
        "@abseil-cpp//absl/synchronization",
        "@abseil-cpp//absl/time",
        "@nlohmann_json//:json",
    ],
)

tensorstore_cc_test(
    name = "context_test",
    size = "small",
    srcs = [
        "context_test.cc",
    ],
    deps = [
        ":context",
        ":json_serialization_options",
        ":json_serialization_options_base",
        "//tensorstore/internal/cache_key",
        "//tensorstore/internal/json_binding",
        "//tensorstore/internal/json_binding:bindable",
        "//tensorstore/internal/testing:concurrent",
        "//tensorstore/internal/testing:json_gtest",
        "//tensorstore/serialization",
        "//tensorstore/serialization:test_util",
        "//tensorstore/util:result",
        "//tensorstore/util:status",
        "//tensorstore/util:status_testutil",
        "@abseil-cpp//absl/status",
        "@googletest//:gtest_main",
        "@nlohmann_json//:json",
    ],
)

tensorstore_cc_library(
    name = "contiguous_layout",
    srcs = ["contiguous_layout.cc"],
    hdrs = ["contiguous_layout.h"],
    deps = [
        ":index",
        ":rank",
        "//tensorstore/util:dimension_set",
        "//tensorstore/util:span",
        "@abseil-cpp//absl/log:absl_check",
    ],
)

tensorstore_cc_test(
    name = "contiguous_layout_test",
    size = "small",
    srcs = ["contiguous_layout_test.cc"],
    deps = [
        ":contiguous_layout",
        ":index",
        "//tensorstore/util:span",
        "@googletest//:gtest_main",
    ],
)

tensorstore_cc_library(
    name = "data_type",
    srcs = ["data_type.cc"],
    hdrs = [
        "data_type.h",
        "data_type_conversion.h",
    ],
    deps = [
        ":index",
        ":static_cast",
        "//tensorstore/internal:elementwise_function",
        "//tensorstore/internal:utf8",
        "//tensorstore/internal/json:same",
        "//tensorstore/internal/json:value_as",
        "//tensorstore/internal/meta:integer_types",
        "//tensorstore/internal/meta:type_traits",
        "//tensorstore/serialization",
        "//tensorstore/util:bfloat16",
        "//tensorstore/util:division",
        "//tensorstore/util:float8",
        "//tensorstore/util:int2",
        "//tensorstore/util:int4",
        "//tensorstore/util:result",
        "//tensorstore/util:str_cat",
        "//tensorstore/util:utf8_string",
        "@abseil-cpp//absl/base",
        "@abseil-cpp//absl/base:core_headers",
        "@abseil-cpp//absl/meta:type_traits",
        "@abseil-cpp//absl/status",
        "@net_sourceforge_half//:half",
        "@nlohmann_json//:json",
    ],
)

tensorstore_cc_test(
    name = "data_type_conversion_test",
    size = "small",
    srcs = ["data_type_conversion_test.cc"],
    deps = [
        ":data_type",
        ":index",
        "//tensorstore/internal:element_copy_function",
        "//tensorstore/internal:elementwise_function",
        "//tensorstore/internal/testing:half_gtest",
        "//tensorstore/internal/testing:json_gtest",
        "//tensorstore/util:result",
        "//tensorstore/util:status_testutil",
        "//tensorstore/util:str_cat",
        "@abseil-cpp//absl/status",
        "@googletest//:gtest_main",
        "@nlohmann_json//:json",
    ],
)

tensorstore_cc_test(
    name = "data_type_test",
    size = "small",
    srcs = ["data_type_test.cc"],
    deps = [
        ":data_type",
        ":index",
        ":static_cast",
        "//tensorstore/internal:elementwise_function",
        "//tensorstore/serialization",
        "//tensorstore/serialization:test_util",
        "//tensorstore/util:status_testutil",
        "//tensorstore/util:str_cat",
        "@abseil-cpp//absl/status",
        "@googletest//:gtest_main",
        "@nlohmann_json//:json",
    ],
)

tensorstore_cc_library(
    name = "index",
    hdrs = ["index.h"],
)

tensorstore_cc_library(
    name = "index_interval",
    srcs = ["index_interval.cc"],
    hdrs = ["index_interval.h"],
    visibility = ["//visibility:public"],
    deps = [
        ":container_kind",
        ":index",
        "//tensorstore/internal:integer_overflow",
        "//tensorstore/serialization",
        "//tensorstore/util:division",
        "//tensorstore/util:quote_string",
        "//tensorstore/util:result",
        "//tensorstore/util:str_cat",
        "@abseil-cpp//absl/status",
    ],
)

tensorstore_cc_test(
    name = "index_interval_test",
    size = "small",
    srcs = ["index_interval_test.cc"],
    deps = [
        ":index_interval",
        "//tensorstore/serialization",
        "//tensorstore/serialization:test_util",
        "//tensorstore/util:status",
        "//tensorstore/util:status_testutil",
        "//tensorstore/util:str_cat",
        "@abseil-cpp//absl/hash:hash_testing",
        "@googletest//:gtest_main",
    ],
)

tensorstore_cc_library(
    name = "index_space",
    hdrs = ["index_space.h"],
    deps = [
        "//tensorstore/index_space:dim_expression",
        "//tensorstore/index_space:dimension_identifier",
        "//tensorstore/index_space:index_transform",
        "//tensorstore/index_space:transformed_array",
    ],
)

tensorstore_cc_library(
    name = "json_serialization_options",
    hdrs = ["json_serialization_options.h"],
    deps = [
        ":data_type",
        ":index",
        ":json_serialization_options_base",
        ":rank",
    ],
)

tensorstore_cc_library(
    name = "json_serialization_options_base",
    hdrs = ["json_serialization_options_base.h"],
)

tensorstore_cc_library(
    name = "open",
    hdrs = ["open.h"],
    deps = [
        ":index",
        ":open_mode",
        ":open_options",
        ":rank",
        ":spec",
        ":tensorstore",
        "//tensorstore/driver",
        "//tensorstore/util:future",
        "//tensorstore/util:option",
        "//tensorstore/util:result",
        "//tensorstore/util:status",
        "@nlohmann_json//:json",
    ],
)

tensorstore_cc_library(
    name = "open_mode",
    srcs = ["open_mode.cc"],
    hdrs = ["open_mode.h"],
    deps = ["@abseil-cpp//absl/status"],
)

tensorstore_cc_test(
    name = "open_mode_test",
    size = "small",
    srcs = ["open_mode_test.cc"],
    deps = [
        ":open_mode",
        "//tensorstore/util:str_cat",
        "@googletest//:gtest_main",
    ],
)

tensorstore_cc_library(
    name = "open_options",
    srcs = ["open_options.cc"],
    hdrs = ["open_options.h"],
    deps = [
        ":batch",
        ":context",
        ":open_mode",
        ":schema",
        ":staleness_bound",
        ":transaction",
        "//tensorstore/kvstore",
        "@abseil-cpp//absl/status",
    ],
)

tensorstore_cc_library(
    name = "progress",
    srcs = ["progress.cc"],
    hdrs = ["progress.h"],
    deps = [
        ":index",
        "//tensorstore/internal/poly",
        "//tensorstore/util:future",
        "@abseil-cpp//absl/status",
    ],
)

tensorstore_cc_test(
    name = "progress_test",
    size = "small",
    srcs = ["progress_test.cc"],
    deps = [
        ":progress",
        "//tensorstore/util:str_cat",
        "@googletest//:gtest_main",
    ],
)

tensorstore_cc_library(
    name = "rank",
    srcs = ["rank.cc"],
    hdrs = ["rank.h"],
    deps = [
        ":index",
        ":static_cast",
        "//tensorstore/util:result",
        "//tensorstore/util:status",
        "//tensorstore/util:str_cat",
    ],
)

tensorstore_cc_test(
    name = "rank_test",
    size = "small",
    srcs = ["rank_test.cc"],
    deps = [
        ":rank",
        "//tensorstore/util:result",
        "//tensorstore/util:status",
        "//tensorstore/util:status_testutil",
        "@googletest//:gtest_main",
    ],
)

tensorstore_cc_library(
    name = "read_write_options",
    hdrs = ["read_write_options.h"],
    deps = [
        ":batch",
        ":contiguous_layout",
        ":progress",
        "//tensorstore/index_space:alignment",
        "@abseil-cpp//absl/status",
    ],
)

tensorstore_cc_library(
    name = "resize_options",
    srcs = ["resize_options.cc"],
    hdrs = ["resize_options.h"],
    deps = [
        ":batch",
        "@abseil-cpp//absl/base:core_headers",
        "@abseil-cpp//absl/meta:type_traits",
        "@abseil-cpp//absl/status",
    ],
)

tensorstore_cc_test(
    name = "resize_options_test",
    size = "small",
    srcs = ["resize_options_test.cc"],
    deps = [
        ":resize_options",
        "//tensorstore/util:str_cat",
        "@googletest//:gtest_main",
    ],
)

tensorstore_cc_library(
    name = "spec",
    srcs = [
        "spec.cc",
        "spec_impl.h",
    ],
    hdrs = ["spec.h"],
    deps = [
        ":array",
        ":chunk_layout",
        ":codec_spec",
        ":context",
        ":data_type",
        ":index",
        ":json_serialization_options",
        ":open_mode",
        ":open_options",
        ":rank",
        ":schema",
        "//tensorstore/driver",
        "//tensorstore/index_space:dimension_units",
        "//tensorstore/index_space:index_transform",
        "//tensorstore/internal/json_binding",
        "//tensorstore/internal/json_binding:bindable",
        "//tensorstore/internal/meta:type_traits",
        "//tensorstore/kvstore",
        "//tensorstore/serialization",
        "//tensorstore/util:option",
        "//tensorstore/util:result",
        "//tensorstore/util:status",
        "//tensorstore/util/garbage_collection",
        "@abseil-cpp//absl/meta:type_traits",
        "@abseil-cpp//absl/status",
        "@nlohmann_json//:json",
    ],
)

tensorstore_cc_test(
    name = "spec_test",
    size = "small",
    srcs = ["spec_test.cc"],
    deps = [
        ":box",
        ":context",
        ":index",
        ":json_serialization_options",
        ":json_serialization_options_base",
        ":spec",
        "//tensorstore/driver/array",
        "//tensorstore/driver/zarr",
        "//tensorstore/index_space:dim_expression",
        "//tensorstore/index_space:index_transform",
        "//tensorstore/internal/json_binding:bindable",
        "//tensorstore/internal/json_binding:gtest",
        "//tensorstore/internal/testing:json_gtest",
        "//tensorstore/kvstore",
        "//tensorstore/kvstore/file",
        "//tensorstore/kvstore/memory",
        "//tensorstore/serialization",
        "//tensorstore/serialization:test_util",
        "//tensorstore/util:result",
        "//tensorstore/util:status_testutil",
        "//tensorstore/util:str_cat",
        "@abseil-cpp//absl/status",
        "@googletest//:gtest_main",
        "@nlohmann_json//:json",
    ],
)

tensorstore_cc_library(
    name = "staleness_bound",
    hdrs = ["staleness_bound.h"],
    deps = ["@abseil-cpp//absl/time"],
)

tensorstore_cc_library(
    name = "static_cast",
    srcs = ["static_cast.cc"],
    hdrs = ["static_cast.h"],
    deps = [
        "//tensorstore/util:result",
        "//tensorstore/util:str_cat",
        "@abseil-cpp//absl/meta:type_traits",
        "@abseil-cpp//absl/status",
    ],
)

tensorstore_cc_test(
    name = "static_cast_test",
    size = "small",
    srcs = ["static_cast_test.cc"],
    deps = [
        ":static_cast",
        "//tensorstore/util:result",
        "//tensorstore/util:span",
        "//tensorstore/util:status_testutil",
        "//tensorstore/util:str_cat",
        "@abseil-cpp//absl/status",
        "@googletest//:gtest_main",
    ],
)

tensorstore_cc_library(
    name = "strided_layout",
    srcs = ["strided_layout.cc"],
    hdrs = ["strided_layout.h"],
    deps = [
        ":box",
        ":container_kind",
        ":contiguous_layout",
        ":index",
        ":rank",
        ":static_cast",
        "//tensorstore/internal:integer_overflow",
        "//tensorstore/internal:lldb_scripting",
        "//tensorstore/internal/container:multi_vector",
        "//tensorstore/internal/container:multi_vector_view",
        "//tensorstore/internal/meta:attributes",
        "//tensorstore/internal/meta:type_traits",
        "//tensorstore/util:constant_vector",
        "//tensorstore/util:extents",
        "//tensorstore/util:result",
        "//tensorstore/util:span",
        "//tensorstore/util:status",
        "//tensorstore/util:str_cat",
        "@abseil-cpp//absl/status",
    ],
)

tensorstore_cc_test(
    name = "strided_layout_test",
    size = "small",
    srcs = ["strided_layout_test.cc"],
    deps = [
        ":box",
        ":contiguous_layout",
        ":index",
        ":rank",
        ":static_cast",
        ":strided_layout",
        "//tensorstore/internal/meta:type_traits",
        "//tensorstore/util:span",
        "//tensorstore/util:status_testutil",
        "//tensorstore/util:str_cat",
        "@abseil-cpp//absl/meta:type_traits",
        "@abseil-cpp//absl/status",
        "@googletest//:gtest_main",
    ],
)

tensorstore_cc_library(
    name = "tensorstore",
    srcs = [
        "tensorstore.cc",
        "tensorstore_impl.h",
    ],
    hdrs = ["tensorstore.h"],
    deps = [
        ":array",
        ":array_storage_statistics",
        ":chunk_layout",
        ":codec_spec",
        ":container_kind",
        ":data_type",
        ":index",
        ":open_mode",
        ":open_options",
        ":progress",
        ":rank",
        ":read_write_options",
        ":resize_options",
        ":schema",
        ":spec",
        ":static_cast",
        ":strided_layout",
        ":transaction",
        "//tensorstore/driver",
        "//tensorstore/driver/auto",
        "//tensorstore/index_space:dimension_units",
        "//tensorstore/index_space:index_transform",
        "//tensorstore/index_space:transformed_array",
        "//tensorstore/kvstore",
        "//tensorstore/serialization",
        "//tensorstore/util:executor",
        "//tensorstore/util:future",
        "//tensorstore/util:option",
        "//tensorstore/util:result",
        "//tensorstore/util:span",
        "//tensorstore/util:status",
        "//tensorstore/util:str_cat",
        "//tensorstore/util/garbage_collection",
        "@abseil-cpp//absl/status",
    ],
)

tensorstore_cc_library(
    name = "transaction",
    srcs = ["transaction.cc"],
    hdrs = [
        "transaction.h",
        "transaction_impl.h",
    ],
    deps = [
        "//tensorstore/internal:compare",
        "//tensorstore/internal:intrusive_ptr",
        "//tensorstore/internal:mutex",
        "//tensorstore/internal/container:intrusive_red_black_tree",
        "//tensorstore/serialization",
        "//tensorstore/util:future",
        "//tensorstore/util:result",
        "//tensorstore/util:str_cat",
        "//tensorstore/util/garbage_collection",
        "@abseil-cpp//absl/base:core_headers",
        "@abseil-cpp//absl/functional:function_ref",
        "@abseil-cpp//absl/status",
        "@abseil-cpp//absl/synchronization",
        "@abseil-cpp//absl/time",
        "@abseil-cpp//absl/types:compare",
    ],
)

tensorstore_cc_test(
    name = "transaction_test",
    size = "small",
    srcs = ["transaction_test.cc"],
    deps = [
        ":transaction",
        "//tensorstore/util:status_testutil",
        "//tensorstore/util:str_cat",
        "@googletest//:gtest_main",
    ],
)

tensorstore_cc_library(
    name = "all_drivers",
    deps = [
        "//tensorstore/driver:all_drivers",
        "//tensorstore/kvstore:all_drivers",
    ],
)

# Package group with visibility to internal-only targets.
package_group(
    name = "internal_packages",
    packages = [
        "//...",
        "//python/tensorstore/...",
        "//tensorstore/...",
    ],
)

tensorstore_cc_library(
    name = "downsample_method",
    srcs = ["downsample_method.cc"],
    hdrs = ["downsample_method.h"],
)

tensorstore_cc_library(
    name = "downsample",
    hdrs = ["downsample.h"],
    deps = [
        ":downsample_method",
        ":index",
        ":open_mode",
        ":rank",
        ":spec",
        ":tensorstore",
        "//tensorstore/driver/downsample",
        "//tensorstore/internal/meta:type_traits",
        "//tensorstore/util:result",
        "//tensorstore/util:span",
    ],
)

tensorstore_cc_library(
    name = "codec_spec",
    srcs = ["codec_spec.cc"],
    hdrs = [
        "codec_spec.h",
        "codec_spec_registry.h",
    ],
    deps = [
        ":json_serialization_options",
        "//tensorstore/internal:intrusive_ptr",
        "//tensorstore/internal:json_registry",
        "//tensorstore/internal/json:same",
        "//tensorstore/internal/json_binding",
        "//tensorstore/internal/json_binding:bindable",
        "//tensorstore/serialization",
        "//tensorstore/serialization:json",
        "//tensorstore/util:result",
        "//tensorstore/util:span",
        "//tensorstore/util/garbage_collection",
        "@abseil-cpp//absl/base:no_destructor",
        "@abseil-cpp//absl/status",
    ],
)

tensorstore_cc_test(
    name = "codec_spec_test",
    size = "small",
    srcs = ["codec_spec_test.cc"],
    deps = [
        ":codec_spec",
        "//tensorstore/driver/zarr",
        "//tensorstore/serialization",
        "//tensorstore/serialization:test_util",
        "@googletest//:gtest_main",
    ],
)

tensorstore_cc_library(
    name = "schema",
    srcs = ["schema.cc"],
    hdrs = ["schema.h"],
    deps = [
        ":array",
        ":box",
        ":chunk_layout",
        ":codec_spec",
        ":container_kind",
        ":data_type",
        ":index",
        ":index_interval",
        ":json_serialization_options",
        ":rank",
        "//tensorstore/index_space:dimension_units",
        "//tensorstore/index_space:index_transform",
        "//tensorstore/index_space:output_index_method",
        "//tensorstore/index_space:transform_broadcastable_array",
        "//tensorstore/internal:intrusive_ptr",
        "//tensorstore/internal/json_binding",
        "//tensorstore/internal/json_binding:array",
        "//tensorstore/internal/json_binding:bindable",
        "//tensorstore/internal/json_binding:data_type",
        "//tensorstore/internal/json_binding:unit",
        "//tensorstore/internal/meta:type_traits",
        "//tensorstore/serialization",
        "//tensorstore/serialization:json",
        "//tensorstore/util:dimension_set",
        "//tensorstore/util:result",
        "//tensorstore/util:span",
        "//tensorstore/util:status",
        "//tensorstore/util:str_cat",
        "//tensorstore/util:unit",
        "//tensorstore/util/garbage_collection",
        "@abseil-cpp//absl/meta:type_traits",
        "@abseil-cpp//absl/status",
        "@nlohmann_json//:json",
    ],
)

tensorstore_cc_test(
    name = "schema_test",
    size = "small",
    srcs = ["schema_test.cc"],
    deps = [
        ":array",
        ":box",
        ":codec_spec",
        ":data_type",
        ":index",
        ":json_serialization_options_base",
        ":rank",
        ":schema",
        "//tensorstore/driver/n5:metadata",
        "//tensorstore/driver/zarr:spec",
        "//tensorstore/index_space:dim_expression",
        "//tensorstore/index_space:index_transform",
        "//tensorstore/index_space:index_transform_testutil",
        "//tensorstore/internal/json_binding:gtest",
        "//tensorstore/internal/testing:json_gtest",
        "//tensorstore/internal/testing:random_seed",
        "//tensorstore/serialization",
        "//tensorstore/serialization:test_util",
        "//tensorstore/util:result",
        "//tensorstore/util:status",
        "//tensorstore/util:status_testutil",
        "//tensorstore/util:str_cat",
        "//tensorstore/util:unit",
        "@abseil-cpp//absl/random",
        "@abseil-cpp//absl/random:bit_gen_ref",
        "@abseil-cpp//absl/status",
        "@googletest//:gtest_main",
        "@nlohmann_json//:json",
    ],
)

tensorstore_cc_test(
    name = "tensorstore_serialization_test",
    size = "small",
    srcs = ["tensorstore_serialization_test.cc"],
    deps = [
        ":array",
        ":context",
        ":open",
        ":open_mode",
        ":spec",
        ":tensorstore",
        "//tensorstore/driver/array",
        "//tensorstore/driver/n5",
        "//tensorstore/internal/testing:scoped_directory",
        "//tensorstore/kvstore/file",
        "//tensorstore/kvstore/memory",
        "//tensorstore/serialization",
        "//tensorstore/serialization:test_util",
        "//tensorstore/util:status_testutil",
        "@googletest//:gtest_main",
    ],
)

exports_files(
    ["stack.h"],
    visibility = ["//tensorstore:internal_packages"],
)

alias(
    name = "stack",
    actual = "//tensorstore/driver/stack",
)

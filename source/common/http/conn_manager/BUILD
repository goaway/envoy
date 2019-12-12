licenses(["notice"])  # Apache 2

load(
    "//bazel:envoy_build_system.bzl",
    "envoy_cc_library",
    "envoy_package",
)

envoy_package()

envoy_cc_library(
    name = "stream_manager_interface",
    hdrs = ["stream_manager.h"],
    deps = [
        "//include/envoy/http:codec_interface",
        "//include/envoy/http:header_map_interface",
    ],
)

envoy_cc_library(
    name = "active_stream_lib",
    srcs = [
        "active_stream.cc",
        "active_stream_decoder_filter.cc",
        "active_stream_encoder_filter.cc",
        "active_stream_filter_base.cc",
    ],
    hdrs = [
        "active_stream.h",
        "active_stream_decoder_filter.h",
        "active_stream_encoder_filter.h",
        "active_stream_filter_base.h",
    ],
    deps = [
        ":stream_manager_interface",
        "//include/envoy/access_log:access_log_interface",
        "//include/envoy/buffer:buffer_interface",
        "//include/envoy/event:deferred_deletable",
        "//include/envoy/http:filter_interface",
        "//include/envoy/http:header_map_interface",
        "//include/envoy/network:connection_interface",
        "//include/envoy/router:scopes_interface",
        "//include/envoy/upstream:upstream_interface",
        "//source/common/buffer:watermark_buffer_lib",
        "//source/common/common:linked_object",
        "//source/common/common:scope_tracker",
        "//source/common/grpc:common_lib",
        "//source/common/http:codes_lib",
        "//source/common/http:conn_manager_config_interface",
        "//source/common/http:conn_manager_info_interface",
        "//source/common/http:conn_manager_utility_lib",
        "//source/common/http:utility_lib",
        "//source/common/router:config_lib",
        "//source/common/runtime:runtime_lib",
        "//source/common/stats:timespan_lib",
        "//source/common/stream_info:stream_info_lib",
    ],
)

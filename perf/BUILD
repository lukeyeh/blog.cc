load("@rules_cc//cc:cc_binary.bzl", "cc_binary")
load("@rules_cc//cc:cc_library.bzl", "cc_library")
load("@rules_cc//cc:cc_test.bzl", "cc_test")

# Benchmarking: a load generator for HTTP servers and the program that runs
# it. Nothing else depends on this.
package(default_visibility = ["//visibility:private"])

cc_binary(
    name = "bench",
    srcs = ["bench.cc"],
    deps = [
        ":load",
        "//http:message",
        "//http:server",
        "//site",
        "@abseil-cpp",
    ],
)

cc_library(
    name = "histogram",
    srcs = ["histogram.cc"],
    hdrs = ["histogram.h"],
)

cc_library(
    name = "load",
    srcs = ["load.cc"],
    hdrs = ["load.h"],
    deps = [
        ":histogram",
        "//async:task",
        "//http:client",
        "//net:event_loop",
        "@abseil-cpp",
    ],
)

cc_test(
    name = "histogram_test",
    srcs = ["histogram_test.cc"],
    deps = [
        ":histogram",
        "//testing:main",
        "@gbenchmark",
        "@gtest",
    ],
)

cc_test(
    name = "load_test",
    srcs = ["load_test.cc"],
    deps = [
        ":load",
        "//http:message",
        "//http:server",
        "//testing:main",
        "@abseil-cpp",
        "@gbenchmark",
        "@gtest",
    ],
)

load("@rules_cc//cc:cc_binary.bzl", "cc_binary")
load("@rules_cc//cc:cc_library.bzl", "cc_library")
load("@rules_cc//cc:cc_test.bzl", "cc_test")

cc_library(
    name = "http",
    srcs = ["http.cc"],
    hdrs = ["http.h"],
    deps = [
        "@abseil-cpp//absl/status",
        "@abseil-cpp//absl/status:statusor",
        "@abseil-cpp//absl/strings",
    ],
)

cc_library(
    name = "site",
    srcs = ["site.cc"],
    hdrs = ["site.h"],
    deps = [
        ":http",
        "@abseil-cpp//absl/strings",
    ],
)

cc_library(
    name = "server_lib",
    srcs = ["server.cc"],
    hdrs = ["server.h"],
    deps = [
        ":http",
        "@abseil-cpp//absl/log",
        "@abseil-cpp//absl/status",
        "@abseil-cpp//absl/strings",
    ],
)

cc_binary(
    name = "server",
    srcs = ["main.cc"],
    deps = [
        ":server_lib",
        ":site",
        "@abseil-cpp//absl/flags:flag",
        "@abseil-cpp//absl/flags:parse",
        "@abseil-cpp//absl/log",
        "@abseil-cpp//absl/log:globals",
        "@abseil-cpp//absl/log:initialize",
        "@abseil-cpp//absl/status",
    ],
)

cc_test(
    name = "http_test",
    srcs = ["http_test.cc"],
    deps = [
        ":http",
        "@googletest//:gtest_main",
    ],
)

cc_test(
    name = "site_test",
    srcs = ["site_test.cc"],
    deps = [
        ":http",
        ":site",
        "@googletest//:gtest_main",
    ],
)

alias(
    name = "compile_commands",
    actual = "@wolfd_bazel_compile_commands//:generate_compile_commands",
)

load("@rules_cc//cc:cc_library.bzl", "cc_library")
load("@rules_cc//cc:cc_test.bzl", "cc_test")

package(default_visibility = ["//:__subpackages__"])

cc_library(
    name = "message",
    srcs = ["message.cc"],
    hdrs = ["message.h"],
    deps = [
        "@abseil-cpp",
    ],
)

cc_library(
    name = "server",
    srcs = [
        "connection.cc",
        "connection.h",
        "server.cc",
    ],
    hdrs = ["server.h"],
    deps = [
        ":message",
        "//async:task",
        "//net:event_loop",
        "//net:tcp",
        "@abseil-cpp",
    ],
)

cc_test(
    name = "message_test",
    srcs = ["message_test.cc"],
    deps = [
        ":message",
        "//testing:main",
        "@abseil-cpp",
        "@gbenchmark",
        "@gtest",
    ],
)

cc_library(
    name = "client",
    srcs = ["client.cc"],
    hdrs = ["client.h"],
    deps = [
        ":message",
        "//async:task",
        "//net:tcp",
        "@abseil-cpp",
    ],
)

cc_test(
    name = "client_test",
    srcs = [
        "client_test.cc",
        "connection.h",
    ],
    deps = [
        ":client",
        ":message",
        ":server",
        "//async:task",
        "//net:event_loop",
        "//net:tcp",
        "//testing:main",
        "@abseil-cpp",
        "@gbenchmark",
        "@gtest",
    ],
)

cc_test(
    name = "server_test",
    srcs = ["server_test.cc"],
    deps = [
        ":client",
        ":message",
        ":server",
        "//async:task",
        "//net:event_loop",
        "//testing:main",
        "@abseil-cpp",
        "@gbenchmark",
        "@gtest",
    ],
)

cc_test(
    name = "connection_test",
    srcs = [
        "connection.h",
        "connection_test.cc",
    ],
    deps = [
        ":message",
        ":server",
        "//async:task",
        "//net:event_loop",
        "//net:tcp",
        "//testing:main",
        "@abseil-cpp",
        "@gbenchmark",
        "@gtest",
    ],
)

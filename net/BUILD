load("@rules_cc//cc:cc_library.bzl", "cc_library")
load("@rules_cc//cc:cc_test.bzl", "cc_test")

# Only the HTTP layer and the load generator do network I/O directly;
# everything else goes through //http. Nothing here talks to the kernel; that
# is //os.
package(default_visibility = [
    "//http:__pkg__",
    "//perf:__pkg__",
])

cc_library(
    name = "event_loop",
    srcs = ["event_loop.cc"],
    hdrs = ["event_loop.h"],
    deps = [
        "//async:task",
        "//async:task_scope",
        "//os:io",
        "@abseil-cpp",
    ],
)

cc_library(
    name = "tcp",
    srcs = ["tcp.cc"],
    hdrs = ["tcp.h"],
    deps = [
        "//async:task",
        "//os:io",
        "//os:socket",
        "@abseil-cpp",
    ],
)

cc_test(
    name = "event_loop_test",
    srcs = ["event_loop_test.cc"],
    deps = [
        ":event_loop",
        ":tcp",
        "//async:task",
        "//testing:main",
        "@abseil-cpp",
        "@gbenchmark",
        "@gtest",
    ],
)

cc_test(
    name = "tcp_test",
    srcs = ["tcp_test.cc"],
    deps = [
        ":event_loop",
        ":tcp",
        "//async:task",
        "//testing:main",
        "@abseil-cpp",
        "@gbenchmark",
        "@gtest",
    ],
)

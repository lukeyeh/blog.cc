load("@rules_cc//cc:cc_library.bzl", "cc_library")
load("@rules_cc//cc:cc_test.bzl", "cc_test")

# Asynchronous functions. The only code that touches the C++ coroutine
# machinery; depends on nothing else in this repository.
package(default_visibility = ["//:__subpackages__"])

cc_library(
    name = "task",
    hdrs = ["task.h"],
)

cc_library(
    name = "awaitable",
    hdrs = ["awaitable.h"],
)

cc_library(
    name = "task_scope",
    srcs = ["task_scope.cc"],
    hdrs = ["task_scope.h"],
    deps = [
        ":task",
        "@abseil-cpp",
    ],
)

cc_test(
    name = "task_test",
    srcs = ["task_test.cc"],
    deps = [
        ":task",
        ":task_scope",
        "//testing:main",
        "@gbenchmark",
        "@gtest",
    ],
)

cc_test(
    name = "awaitable_test",
    srcs = ["awaitable_test.cc"],
    deps = [
        ":awaitable",
        ":task",
        ":task_scope",
        "//testing:main",
        "@gbenchmark",
        "@gtest",
    ],
)

cc_test(
    name = "task_scope_test",
    srcs = ["task_scope_test.cc"],
    deps = [
        ":awaitable",
        ":task",
        ":task_scope",
        "//testing:main",
        "@gbenchmark",
        "@gtest",
    ],
)

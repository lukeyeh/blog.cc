load("@rules_cc//cc:cc_library.bzl", "cc_library")
load("@rules_cc//cc:cc_test.bzl", "cc_test")

package(default_visibility = ["//:__subpackages__"])

cc_library(
    name = "site",
    srcs = ["site.cc"],
    hdrs = ["site.h"],
    deps = [
        "//http:message",
        "@abseil-cpp",
    ],
)

cc_test(
    name = "site_test",
    srcs = ["site_test.cc"],
    deps = [
        ":site",
        "//http:message",
        "//testing:main",
        "@abseil-cpp",
        "@gbenchmark",
        "@gtest",
    ],
)

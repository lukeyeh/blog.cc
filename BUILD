load("@rules_cc//cc:cc_binary.bzl", "cc_binary")

cc_binary(
    name = "server",
    srcs = ["main.cc"],
    deps = [
        "//http:message",
        "//http:server",
        "//site",
        "@abseil-cpp",
    ],
)

alias(
    name = "compile_commands",
    actual = "@wolfd_bazel_compile_commands//:generate_compile_commands",
)

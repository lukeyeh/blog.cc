# The libraries the server, its tests and its benchmarks are built against.
#
# This is the one place they are chosen. The Nix package (package.nix) and
# the Bazel build (through bazel.nix) both take them from here, at the
# versions flake.lock pins, so the binary that is developed and benchmarked
# is built from the same libraries as the one that is deployed.
pkgs: {
  # Built as C++20 like the server, so that the two agree on which standard
  # library types Abseil's own types stand for.
  abseil-cpp = pkgs.abseil-cpp.override { cxxStandard = "20"; };
  liburing = pkgs.liburing;

  # For tests and benchmarks only.
  gtest = pkgs.gtest;
  gbenchmark = pkgs.gbenchmark;
}

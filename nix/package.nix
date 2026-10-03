# The server as a Nix package, for deployment.
#
# This compiles the same sources as `bazel build //:server`, against the same
# libraries: both this package and Bazel take them from deps.nix. Only the
# compiling itself is done twice over, here by Nix and in development by
# Bazel, because Bazel inside a Nix build is fragile (it wants to download
# things, which a Nix build may not).
#
# What the two builds must still agree on is the compiler flags, below and in
# .bazelrc. Source files need no upkeep: every non-test .cc file under the
# server's directories is compiled.
{
  lib,
  stdenv,
  abseil-cpp,
  liburing,
  pkg-config,
}:

stdenv.mkDerivation {
  pname = "blog-server";
  version = "0.1.0";

  # Only the server's sources, so that editing a README, a test's BUILD file
  # or the benchmark does not rebuild the package.
  src = lib.fileset.toSource {
    root = ../.;
    fileset = lib.fileset.unions [
      ../main.cc
      ../async
      ../os
      ../net
      ../http
      ../site
    ];
  };

  nativeBuildInputs = [ pkg-config ];
  buildInputs = [
    abseil-cpp
    liburing
  ];

  buildPhase = ''
    runHook preBuild

    sources=$(find main.cc async os net http site -name '*.cc' ! -name '*_test.cc')
    libraries="absl_flags absl_flags_parse absl_log absl_log_flags
               absl_log_globals absl_log_initialize absl_check absl_status
               absl_statusor absl_status_macros absl_strings absl_str_format
               absl_flat_hash_map absl_flat_hash_set liburing"

    # The flags mirror .bazelrc: C++20, no exceptions.
    $CXX -std=c++20 -O2 -fno-exceptions \
      -Wno-coroutine-missing-unhandled-exception \
      -I. $sources -o blog-server \
      $(pkg-config --cflags --libs $libraries)

    runHook postBuild
  '';

  installPhase = ''
    runHook preInstall
    install -Dm755 blog-server $out/bin/blog-server
    runHook postInstall
  '';

  meta = {
    description = "Fast HTTP server for a static personal website, built on io_uring";
    homepage = "https://github.com/lukeyeh/blog.cc";
    mainProgram = "blog-server";
    platforms = lib.platforms.linux;
  };
}

{
  description = "Fast C++ HTTP server for a personal website";

  inputs = {
    nixpkgs.url = "github:nixos/nixpkgs?ref=nixos-unstable";
  };

  outputs = { self, nixpkgs }:
    let
      # Linux only: the server is built on io_uring.
      systems = [ "x86_64-linux" "aarch64-linux" ];
      forAllSystems = f: nixpkgs.lib.genAttrs systems (system: f nixpkgs.legacyPackages.${system});

      # The LLVM version everything is compiled with. The exact compiler is
      # whatever flake.lock pins for this major version.
      llvmFor = pkgs: pkgs.llvmPackages_21;
    in
    {
      # The server, for deployment: `nix build`, `nix run`.
      packages = forAllSystems (pkgs: {
        default = pkgs.callPackage ./nix/package.nix {
          stdenv = (llvmFor pkgs).stdenv;
          # The same libraries Bazel builds against.
          inherit (import ./nix/deps.nix pkgs) abseil-cpp liburing;
        };
      });

      # `services.blog-server` for a NixOS machine.
      nixosModules.default = import ./nix/module.nix self;

      # `nix flake check` builds the package and boots it in a VM.
      checks = forAllSystems (pkgs: {
        package = self.packages.${pkgs.stdenv.hostPlatform.system}.default;
        nixos = import ./nix/test.nix self { inherit pkgs; };
      });

      devShells = forAllSystems (pkgs:
        let
          # The compiler Bazel builds with. Bazel picks up $CC from this shell.
          llvm = llvmFor pkgs;
        in
        {
          default = (pkgs.mkShell.override { stdenv = llvm.stdenv; }) {
            packages = [
              pkgs.bazel_9
              pkgs.bazel-buildtools
              llvm.clang-tools
              # Profiling: perf samples a running program, and flamegraph
              # (from cargo-flamegraph) runs perf and draws the result.
              pkgs.perf
              pkgs.cargo-flamegraph
              # samply opens perf's recordings in the Firefox Profiler, an
              # interactive viewer that runs in the browser.
              pkgs.samply
              # pprof is an alternative viewer, also usable in the terminal.
              # perf_data_converter lets it read perf's recordings, and
              # graphviz draws its call graphs.
              pkgs.pprof
              pkgs.perf_data_converter
              pkgs.graphviz
            ];
          };
        });
    };
}

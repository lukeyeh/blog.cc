# personal-site

A small, fast HTTP/1.1 server in C++ for a personal website. It serves static
files from memory, with one event loop per CPU core. Built with Bazel inside a
Nix dev shell.

Linux only. It does its network I/O with io_uring where the system allows
that (kernel 6.1 or later, outside sandboxes that forbid it, such as Docker's
default seccomp profile) and with epoll everywhere else, so it also runs on
container platforms. The `--io_backend` flag chooses explicitly.

## Setup

```
nix develop          # or `direnv allow` once
bazel run :server    # serves ./www on http://127.0.0.1:8080
bazel test //...
```

Bazel must be run inside the Nix shell; outside it, Bazel silently uses the
host compiler.

Flags:

| Flag | Default | Meaning |
| --- | --- | --- |
| `--port` | `8080` | Port to listen on |
| `--address` | `127.0.0.1` | IPv4 address to listen on (`0.0.0.0` for all interfaces) |
| `--root` | `www` | Directory of static files |
| `--io_backend` | `auto` | `io_uring`, `epoll`, or `auto`: io_uring if the system allows it, otherwise epoll |
| `--v` | `0` | `1` logs every request; off by default because it is slow |

For a deployable binary: `bazel build -c opt :server`, then run
`bazel-bin/server --root=/path/to/www`.

## What it does

- `GET` and `HEAD` for files under `--root`; directories serve `index.html`.
- `GET /healthz` returns `ok`.
- Keeps connections open, so a client can send many requests over one.
- Dotfiles, and symbolic links that lead outside the root, are not served.

The files are read into memory once at startup. **Restart the server to pick
up changes to the site.**

## What it does not do

- TLS or HTTP/2. Run it behind a reverse proxy such as Caddy or nginx.
- Request bodies, so no `POST`.
- Caching headers, compression, or range requests.
- Limit the number of connections.

## Performance

```
bazel run -c opt //perf:bench                    # measure this server
bazel run -c opt //perf:bench -- --port=8080     # measure a server already running
```

The benchmark is a load generator written on the same layers as the server.
It holds `--connections` connections open across `--threads` threads, requests
`--path` over each as fast as the responses come back, and prints one line per
round:

```
GET http://127.0.0.1:42465/   64 connections   4 threads   8s per round
round 1:   788,080 requests/s   p50 78us   p99 164us   max 17.99ms   0 errors
```

With no `--port` it starts this repository's server inside the benchmark
process. With `--port` it measures whatever is listening there, which is how to
compare two builds of the server, or this server against another one.

On an 8-core machine, with the load generator competing for the same cores,
the server handles roughly 600,000 to 800,000 requests per second. Rounds
differ by 10% or more with whatever else the machine is doing. The first
version of this server, which used a thread and a connection per request and
read each file from disk, managed about 63,000.

[perf/README.md](perf/README.md) has the details: how the benchmark works, its
flags, the microbenchmarks for individual pieces, and how to get a flamegraph.

## Deployment

The flake provides the server as a Nix package and a NixOS module.

```
nix build          # result/bin/blog-server
nix run . -- --root=www --port=8080
nix flake check    # builds the package and boots it in a virtual machine
```

On a NixOS machine, add the flake as an input and enable the service:

```nix
{
  inputs.blog.url = "github:lukeyeh/blog.cc";

  outputs = { nixpkgs, blog, ... }: {
    nixosConfigurations.myhost = nixpkgs.lib.nixosSystem {
      system = "x86_64-linux";
      modules = [
        blog.nixosModules.default
        {
          services.blog-server = {
            enable = true;
            root = ./www;   # the site to serve
          };
        }
      ];
    };
  };
}
```

| Option | Default | Meaning |
| --- | --- | --- |
| `enable` | `false` | Run the server as a systemd service |
| `root` | (required) | Directory of static files |
| `address` | `"127.0.0.1"` | IPv4 address to listen on |
| `port` | `8080` | Port to listen on |
| `openFirewall` | `false` | Open `port` in the firewall |
| `ioBackend` | `"auto"` | `"io_uring"`, `"epoll"`, or `"auto"` |
| `logRequests` | `false` | Log every request to the journal; slow |
| `package` | this flake's | The server package to run |

Things to know:

- The server speaks plain HTTP on localhost by default. Put something that
  terminates TLS in front of it, such as Caddy or a Cloudflare Tunnel.
- With `root = ./www`, the site is copied into the Nix store, and changing it
  restarts the service on the next deploy. That matters because the server
  only reads the files when it starts.
- The service runs as a throwaway user with no access to home directories,
  devices or anything writable.
- The package is compiled by Nix directly, not through Bazel, but against the
  same libraries: both take them from `nix/deps.nix`. `nix/package.nix`
  explains why. It compiles every non-test source file, so it needs no
  changes when files are added.

## Layout

Each directory is one layer and only depends on the ones below it.

| Path | Purpose |
| --- | --- |
| `main.cc` | Flags and wiring |
| `site/` | What this website serves: the files held in memory, and routing. Add new endpoints in `Site::Handle` |
| `http/message.{h,cc}` | The HTTP message format: parse a request head, build a response head |
| `http/server.{h,cc}` | `HttpServer`: listen, then serve with one event loop per core |
| `http/client.{h,cc}` | `HttpClient`: fetch pages over one connection; used by tests and the benchmark |
| `http/connection.{h,cc}` | The conversation with one client: read a request, answer it, repeat |
| `net/event_loop.{h,cc}` | Runs tasks on a thread, continuing each when its I/O finishes |
| `net/tcp.{h,cc}` | TCP listener and connection classes: delimited reads, buffering, timeouts |
| `os/io.{h,cc}` | Network I/O as C++: an `IoDriver`, and awaitable `Accept`, `Connect`, `Receive`, `Send`, `Sleep` |
| `os/io_uring_backend.{h,cc}` | Carries those operations out with io_uring |
| `os/epoll_backend.{h,cc}` | Carries them out with epoll |
| `os/socket.{h,cc}` | Sockets as C++: `Socket`, `SocketAddress`, `SocketOption` |
| `async/task.h` | `Task<T>`, the return type of functions that wait without blocking a thread |
| `async/awaitable.h` | `Awaitable` and `Waker`: how to write the thing a task waits for |
| `async/task_scope.{h,cc}` | `TaskScope`: starting tasks that nobody awaits |
| `perf/` | Benchmarking: a latency histogram, a load generator, and the `bench` program |
| `testing/main.cc` | The `main` of every test: runs a file's tests, or its benchmarks when asked |
| `nix/` | The Nix package, the NixOS module, and a VM test of the module |
| `www/` | The site's content |

Only `async/` deals with the C++ coroutine machinery; the rest of the code
uses `Task`, `co_await`, `Awaitable` and `Spawn`.

Only `os/` talks to the kernel. It turns file descriptors, option flags and
error numbers into classes, enums and `absl::Status`, so nothing above it
contains a system call.

Every source file has a test beside it (`foo_test.cc`), written to double as
documentation of the file it covers. The same file holds its microbenchmarks:
`bazel run -c opt //http:message_test -- --benchmark_filter=all`.

Tests that do I/O run on io_uring by default. `bazel test --config=epoll //...`
runs the whole suite on epoll instead; both should pass.

Format and lint with `clang-format -i` and, after `bazel run
:compile_commands`, `clang-tidy`.

## Libraries

Abseil, liburing, GoogleTest and Google Benchmark come from nixpkgs, at the
versions `flake.lock` pins. `nix/deps.nix` is the one list of them, used by
both builds:

- Bazel imports them through
  [rules_nixpkgs](https://github.com/tweag/rules_nixpkgs), set up in
  `MODULE.bazel`. BUILD files depend on them as `"@abseil-cpp"`, `"@liburing"`,
  `"@gtest"` and `"@gbenchmark"`.
- The Nix package takes them from the same file.

So the binary you develop and benchmark is built from the same libraries as
the one you deploy, and `nix flake update` moves both at once.

Bazel fetches a library by asking Nix to build it, which needs `nix` on the
`PATH`; inside `nix develop` it is. The first build after a `flake.lock`
change takes a minute or two while Nix builds Abseil.

### Adding one

1. Add the nixpkgs package to `nix/deps.nix`.
2. Add a `nix_pkg.file(...)` block for it in `MODULE.bazel`, copying an
   existing one, and its name to the `use_repo` line.
3. Depend on it in a `BUILD` file as `"@<name>"`.
4. If the server itself uses it, add it to `buildInputs` and the `libraries`
   list in `nix/package.nix`.

## clangd

`bazel run :compile_commands` writes `compile_commands.json`. Start clangd
with `--query-driver=/**/*` so it can find Nix's system headers.

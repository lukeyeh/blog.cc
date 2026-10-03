# personal-site

A barebones HTTP/1.1 server in C++ for a personal website. It serves static
files from a directory and has no dependencies beyond POSIX sockets and
Abseil. Built with Bazel inside a Nix dev shell.

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

For a deployable binary: `bazel build -c opt :server`, then run
`bazel-bin/server --root=/path/to/www`.

## What it does

- `GET` and `HEAD` for files under `--root`; directories serve `index.html`.
- `GET /healthz` returns `ok`.
- Requests that resolve outside the root (`..`, symlinks) or name a dotfile
  get a 404.

## What it does not do

- TLS or HTTP/2. Run it behind a reverse proxy such as Caddy or nginx.
- Keep-alive: one request per connection.
- Request bodies, so no `POST`.
- Caching headers, compression, or range requests.
- Limit concurrent connections: each one gets its own thread, and each file is
  read fully into memory.

## Layout

| File | Purpose |
| --- | --- |
| `http.{h,cc}` | Parse a request head, serialise a response |
| `site.{h,cc}` | Routing and static file lookup; add new endpoints in `Handle` |
| `server.{h,cc}` | Runs the HTTP server: one thread per connection, one request each |
| `net.{h,cc}` | TCP listener and connection classes; the only code that touches the OS socket API |
| `main.cc` | Flags and wiring |
| `www/` | The site itself |

## Adding a dependency

Find the module on https://registry.bazel.build/, add
`bazel_dep(name = "...", version = "...")` to `MODULE.bazel`, reference it as
`@<module_name>//pkg:target`, and run `bazel mod tidy`.

## clangd

`bazel run :compile_commands` writes `compile_commands.json`. Start clangd
with `--query-driver=/**/*` so it can find Nix's system headers.

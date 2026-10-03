# perf

How to measure this server's performance. There are three tools, for three
questions:

| Question | Tool |
| --- | --- |
| How fast is the server as a whole? | [The server benchmark](#the-server-benchmark), `//perf:bench` |
| What does one small piece cost? | [Microbenchmarks](#microbenchmarks), in every `_test.cc` |
| Where is the time going? | [Flamegraphs](#flamegraphs), `--config=flamegraph`, or an [interactive profiler UI](#an-interactive-profiler-ui), `--config=record` |

Everything runs from the repository root, inside the Nix shell (`nix
develop`). The compiler, Bazel, every library and the profiling tools are
pinned by `flake.lock`, so the same commit builds the same binaries on any
machine. The measurements themselves still
depend on the machine and on what else it is doing.

# The server benchmark

A load generator and the program that runs it.

## Running it

From the repository root, inside the Nix shell:

```
nix develop
bazel run -c opt //perf:bench
```

That starts this repository's server inside the benchmark process, serving
`./www`, and measures it:

```
GET http://127.0.0.1:44417/   64 connections   4 threads   8s per round
round 1:   731,177 requests/s   p50 70us   p99 295us   max 15.85ms   0 errors
round 2:   705,189 requests/s   p50 98us   p99 180us   max 18.92ms   0 errors
round 3:   602,507 requests/s   p50 82us   p99 492us   max 51.99ms   0 errors
```

Always pass `-c opt`. An unoptimised build is roughly a third slower, and the
benchmark prints a warning if it is one.

## Reading the output

| Column | Meaning |
| --- | --- |
| `requests/s` | Responses with status 200 per second |
| `p50` | Half of the requests took this long or less |
| `p99` | All but the slowest 1% took this long or less |
| `max` | The slowest request |
| `errors` | Anything that was not a 200: other statuses, failed requests, failed connections. Should be 0 |

Latency is measured from sending a request to receiving the last byte of its
response. Percentiles are approximate, at most about 6% above the true value.

## How it works

### The setup

Everything runs on one machine, and by default in one process:

```
                         benchmark process
 ┌──────────────────────────────────────────────────────────────────┐
 │  load generator                         server under test        │
 │  --threads threads (4)                  one thread per core      │
 │                                                                  │
 │  thread 1: 16 clients ─┐                ┌─ thread 1              │
 │  thread 2: 16 clients ─┤  64 TCP        ├─ thread 2              │
 │  thread 3: 16 clients ─┼─ connections ──┼─ ...                   │
 │  thread 4: 16 clients ─┘  over loopback └─ thread N              │
 │                           (127.0.0.1)                            │
 └──────────────────────────────────────────────────────────────────┘
```

- **The server** is the same `HttpServer` and `Site` that `//:server` runs. It
  loads `--root` into memory, listens on a free port chosen by the system,
  and serves on one thread per CPU core. It starts once and keeps running for
  all rounds.
- **The load generator** is a set of clients. Each client is one task holding
  one connection. The `--connections` clients are divided evenly over
  `--threads` threads, and each thread runs its clients on its own event loop,
  the same kind the server uses.
- **The network** is real TCP over the loopback interface. Requests go through
  the kernel's network stack but never leave the machine, so there is no
  network delay or packet loss in the numbers.

With `--port`, the left half is unchanged and the right half is whatever
server you started yourself, in its own process.

### What a client does

Each client runs this loop for the whole round:

1. Connect to the server.
2. Note the time, send `GET <path>`, wait for the complete response (head and
   body), and note the time again.
3. If the status was 200, count one request and record the time it took.
   Otherwise count one error.
4. Go to 2 without pausing.

If a request fails outright, the client counts an error and reconnects. If
it cannot connect, it counts an error and waits 10ms before trying again.

A client only sends its next request once the previous response has arrived,
so there are never more requests in progress than there are connections. The
request rate is therefore not set by the benchmark: it is however fast the
server answers. This measures the most the server can do for that many
clients, not how it behaves at a given rate.

### What happens in a round

```
 0s            1s                                        9s
 ├── warmup ───┼──────────── measured (--duration) ───────┤
 │ clients     │ responses are counted and timed          │ clients are
 │ connect and │                                          │ stopped where
 │ send, but   │                                          │ they are
 │ nothing is  │                                          │
 │ counted     │                                          │
```

1. Every thread starts its clients and they begin sending immediately.
2. During the warmup, responses are received but not counted. This keeps the
   cost of opening connections, and the server's first moments, out of the
   result. (Errors are counted even during the warmup.)
3. During the measured period, every 200 response adds to the thread's request
   count and its latency histogram. Each thread keeps its own, so recording
   takes no locks.
4. When the time is up, each thread's event loop stops and abandons its
   clients mid-request. Requests in flight at that moment are not counted.
5. The threads' counts and histograms are added together into one result, and
   the line for the round is printed.

Each round starts from scratch on the client side, with new threads and new
connections, against the same running server.

### What the result is made of

- `requests/s` is the number of counted responses divided by the length of
  the measured period.
- `p50`, `p99` and `max` come from the latencies of those same responses.
- With no errors, every number describes complete, successful requests only.

## Flags

Flags go after `--`:

```
bazel run -c opt //perf:bench -- --connections=256 --duration=30s
```

| Flag | Default | Meaning |
| --- | --- | --- |
| `--port` | `0` | Port of a server that is already running. `0` starts this repository's server inside the benchmark |
| `--address` | `127.0.0.1` | IPv4 address of the server |
| `--root` | `www` | With `--port=0`, the directory the server serves |
| `--path` | `/` | Page to request |
| `--connections` | `64` | Connections held open, each sending requests back to back |
| `--threads` | `4` | Load-generating threads the connections are spread over |
| `--duration` | `8s` | Length of each measured round |
| `--warmup` | `1s` | Time spent sending requests before each round's measurement starts |
| `--rounds` | `3` | Number of rounds |

## Measuring another server

With `--port`, the benchmark measures whatever is listening there instead of
starting a server of its own. Use this to compare two builds of this server,
or this server against a different one.

To compare a change against what came before it:

```
# Before the change: build the server and keep a copy.
bazel build -c opt //:server && cp bazel-bin/server /tmp/before

# After the change: do the same.
bazel build -c opt //:server && cp bazel-bin/server /tmp/after

# Run each on its own port, then measure both.
/tmp/before --port=8081 &
/tmp/after --port=8082 &
bazel run -c opt //perf:bench -- --port=8081
bazel run -c opt //perf:bench -- --port=8082
```

Copy the binaries as shown rather than running `bazel-bin/server` directly.
That path follows the most recent build, so after a `bazel test` it leads to
an unoptimised binary.

## Getting numbers you can trust

- Rounds differ by 10% or more depending on what else the machine is doing.
  Close other programs, and compare several rounds rather than one.
- When comparing two servers, alternate between them instead of running all
  of one and then all of the other, so that drift affects both equally.
- The load generator shares the machine's cores with the server. The numbers
  are good for comparing one build with another on the same machine; they
  understate what the server could do with the machine to itself.
- A run in which no request succeeds stops with an error saying why, such as
  `status 404` for a wrong `--path` or `Connection refused` for a wrong
  `--port`.

## What is here

| File | Purpose |
| --- | --- |
| `bench.cc` | The program: flags, starting the server, printing results |
| `load.{h,cc}` | `GenerateLoad`: the load generator. Returns a `LoadResult` |
| `histogram.{h,cc}` | `LatencyHistogram`: records latencies in fixed memory and answers percentiles |

`load_test.cc` and `histogram_test.cc` show how each is used.

# Microbenchmarks

The server benchmark says how fast the server is; microbenchmarks say what
individual pieces cost, in nanoseconds or microseconds, with as little else
in the way as possible. They use
[Google Benchmark](https://github.com/google/benchmark).

They live in the test files. Each `<name>_test.cc` holds the tests for
`<name>`, which say whether it works, and below them its benchmarks, which
say what it costs. One binary runs either.

## Running them

```
bazel test //http:message_test                                   # the tests
bazel run -c opt //http:message_test -- --benchmark_filter=all   # the benchmarks
```

Any flag starting with `--benchmark` switches the binary from tests to
benchmarks. `--benchmark_filter` takes a regular expression choosing which
to run; `all` runs every one in the file.

Every library has them. From the top layer down:

| Target | What its benchmarks measure |
| --- | --- |
| `//site:site_test` | Deciding on a response (`Site::Handle`) for each kind of request, and loading a site into memory |
| `//http:message_test` | Parsing requests and responses and writing response heads: the pure computation in a request |
| `//http:connection_test` | One whole request and response on one thread, for several page sizes. The cleanest view of the per-request path |
| `//http:client_test` | A request with keep-alive against a request that opens its own connection |
| `//http:server_test` | A request to a server running on other threads |
| `//net:tcp_test` | Opening a connection, and sending and receiving text over one |
| `//net:event_loop_test` | Creating an event loop, one turn of it, and spawning a task on it |
| `//os:io_test` | One operation that has to wait, and real sends and receives, on each backend side by side |
| `//os:io_uring_backend_test` | What doing several operations per trip into the kernel saves on io_uring |
| `//os:epoll_backend_test` | An operation that can proceed at once against one that has to be parked, on epoll |
| `//os:socket_test` | Parsing addresses, and creating and binding sockets |
| `//async:task_test` | Calling an asynchronous function, against an ordinary call as the baseline |
| `//async:awaitable_test` | Suspending a task and waking it again |
| `//async:task_scope_test` | Starting tasks nobody awaits, and abandoning them |
| `//perf:histogram_test` | Recording a latency, which the load generator does once per request |

The benchmarks are at the bottom of each file, under a "Benchmarks" heading
with a comment on what to compare; each benchmark has a comment on what it
isolates. `//perf:load_test` has none: the load generator is a benchmark
itself, and `//perf:bench` is how to run it.

## Comparing io_uring and epoll

Every binary that does I/O takes `--io_backend`, so any benchmark can be run
on either:

```
bazel run -c opt //perf:bench -- --io_backend=io_uring
bazel run -c opt //perf:bench -- --io_backend=epoll
bazel run -c opt //http:connection_test -- --benchmark_filter=all --io_backend=epoll
```

For `//perf:bench` the flag applies to both the server it starts and the load
generator. On the machine this was written on, with 64 connections:

| | io_uring | epoll |
| --- | --- | --- |
| Whole server, requests per second | 590k to 710k | 570k to 595k |
| Whole server, 99th percentile latency | 0.4 to 0.6 ms | about 1 ms |
| One request on one thread (`BM_RequestAndResponse/100`) | 8.9 us | 7.4 us |
| One operation that waits (`BM_WaitRoundTrip`) | 900 ns | 160 ns |

The two are close. epoll is quicker when there is one thing to do at a time,
because an operation that can proceed is a plain system call with no detour
through a queue. io_uring pulls ahead under load, where many operations
share each trip into the kernel, and its tail latency is better.

## Putting the numbers together

Read from the bottom layer up, the benchmarks account for where a request's
time goes. On the machine they were written on, roughly:

| Step | Cost | From |
| --- | --- | --- |
| Deciding which page to send | 13 ns | `BM_HandleFile` |
| Parsing a browser's request | 290 ns | `BM_ParseBrowserRequest` |
| Writing the response head | 56 ns | `BM_AppendResponseHead` |
| One trip into the kernel and back (io_uring) | 900 ns | `BM_WaitRoundTrip/io_uring` |
| Sending and receiving a small message | 4,000 ns | `BM_SendAndReceive/io_uring/64` |
| A whole request and response, one thread | 8,700 ns | `BM_RequestAndResponse/100` |
| The same, opening a connection first | 51,000 ns | `BM_ConnectAndGet` |

Everything the server's own code does for a request adds up to a few hundred
nanoseconds. The rest is the kernel moving bytes between sockets, which is
why keep-alive matters far more than faster parsing would.

## Reading the output

```
-----------------------------------------------------------------
Benchmark                       Time             CPU   Iterations
-----------------------------------------------------------------
BM_PlainCall                 1.29 ns         1.28 ns    550017702
BM_AwaitTask                 9.35 ns         9.33 ns     73193174
BM_CreateAndDropTask         8.27 ns         8.26 ns     83691012
BM_AwaitNestedTasks/1        19.9 ns         19.9 ns     34900233 items_per_second=50.3106M/s
BM_AwaitNestedTasks/8         132 ns          131 ns      5425187 items_per_second=60.8868M/s
BM_AwaitNestedTasks/64       2325 ns         2320 ns       298190 items_per_second=27.5889M/s
```

- `Time` and `CPU` are per iteration of the measured loop: one call, one
  await, one request.
- `Iterations` is how many times the loop ran; the library picks it so that
  each benchmark runs long enough to be stable.
- The number after a `/` is the benchmark's argument, such as how many tasks
  deep the chain is or how many bytes are sent.
- `items_per_second` and `bytes_per_second` appear where one iteration does
  several things or moves data, and count those.

## Useful flags

Google Benchmark's flags go after `--`:

| Flag | Meaning |
| --- | --- |
| `--benchmark_filter=<regex>` | Run the benchmarks whose names match; `all` for every one |
| `--benchmark_repetitions=<n>` | Run each benchmark n times and report mean, median and standard deviation |
| `--benchmark_min_time=<n>s` | Run each benchmark for at least this long |
| `--benchmark_format=json` | Machine-readable output |
| `--benchmark_list_tests` | List the benchmarks without running them |

## Adding one

Write it at the bottom of the `_test.cc` for the code it measures, inside
that file's benchmark section; the existing ones are short enough to copy
from. No build changes are needed, because every test already links
`//testing:main`, which provides the `main` that can run either.

To measure something that uses `co_await`, put the measured loop inside a
task, as `AwaitInLoop` does in `async/task_test.cc`.

# Flamegraphs

A flamegraph shows where a program spent its time: one box per function,
as wide as the share of time spent in it, stacked on the function that
called it. Use one when a benchmark says something is slow and you want to
know which part.

## Making one

```
bazel run --config=flamegraph //async:task_test -- --benchmark_filter=BM_AwaitTask
```

This builds the target optimised but with debug information and frame
pointers, runs it under `perf`, and writes `/tmp/flamegraph.svg`. Open the
file in a browser: boxes can be clicked to zoom in, and searched.

It works for any binary. Flags for the binary go after `--`, as usual:

```
bazel run --config=flamegraph //perf:bench -- --rounds=1
```

For a microbenchmark, use `--benchmark_filter` to profile one benchmark at a
time; otherwise the picture mixes them all together.

## An interactive profiler UI

For more than a picture, record a profile and open it in the
[Firefox Profiler](https://profiler.firefox.com/docs/):

```
bazel run --config=record //async:task_test -- --benchmark_filter=BM_AwaitNestedTasks/64
samply import /tmp/perf.data
```

The first command runs the binary under `perf` and writes the recording to
`/tmp/perf.data`. The second opens the profile in your browser
(any browser, not only Firefox). Stop it with Ctrl-C.

The page itself is loaded from profiler.firefox.com, so it needs an internet
connection, but the profile is served from your machine and is not uploaded
unless you press the Upload button.

What the tabs show:

| Tab | Shows |
| --- | --- |
| Call Tree | Functions as a tree of who called whom, with time spent in each. "Invert call stack" turns it into a list of where time was spent directly |
| Flame Graph | The same picture as `flamegraph.svg`, interactive |
| Stack Chart | What was running at each moment, left to right in time |

Above the tabs is a timeline of CPU activity per thread. Drag across it to
look at only that stretch of time, which is how to separate, say, a server's
startup from its steady state. Double-clicking a function opens its source
with time per line. The search box filters every view by function name.

### pprof, as an alternative

[pprof](https://github.com/google/pprof) reads the same recording. Its
browser interface is plainer, but it also works in the terminal, which is
handy for a quick look:

```
pprof -top /tmp/perf.data             # functions by time spent
pprof -list=Nested /tmp/perf.data     # annotated source of functions matching "Nested"
pprof -http=localhost:8000 /tmp/perf.data   # its browser interface
```

It prints a screenful of warnings about parts of the recording it skips. They
are harmless.

### Recordings go stale

A recording stores the path of the binary it came from, and both viewers
read that binary to name functions. A recording stops being readable once
the binary is rebuilt; record again after a change.

## If perf is not permitted

`perf` needs the kernel's permission to sample. If the command fails with a
message about `perf_event_paranoid`, allow profiling of your own programs:

```
sudo sysctl kernel.perf_event_paranoid=2
```

That lasts until reboot. To keep it, put `kernel.perf_event_paranoid = 2` in
`/etc/sysctl.conf`. Ubuntu's default of 4 forbids all profiling by ordinary
users.

## Output files

The commands above write to fixed places outside the repository,
`/tmp/flamegraph.svg` and `/tmp/perf.data`, so nothing they produce can be
committed by accident. Each run overwrites the last; copy a file elsewhere to
keep it.

Running `perf`, `flamegraph` or `samply` by hand writes into the current
directory instead: `flamegraph.svg`, `perf.data`, `perf.data.old`,
`profile.json.gz`. Those names are in `.gitignore` in case that directory is
the repository.

## Using perf directly

`--config=profile` gives the same build without running anything:

```
bazel build --config=profile //async:task_test
perf record -g -o /tmp/perf.data bazel-bin/async/task_test --benchmark_filter=BM_AwaitTask
perf report -i /tmp/perf.data
```

## How it is set up

| Where | What |
| --- | --- |
| `flake.nix` | Adds `perf`, `flamegraph` (from cargo-flamegraph), `samply`, `pprof`, `perf_to_profile` (which lets pprof read `perf.data`) and graphviz to the shell |
| `.bazelrc`, `build:profile` | `-c opt`, `-g`, `-fno-omit-frame-pointer`, no stripping |
| `.bazelrc`, `run:flamegraph` | The profile build, plus `--run_under` to run the binary under `flamegraph` |
| `.bazelrc`, `run:record` | The profile build, plus `--run_under` to run the binary under `perf record` |

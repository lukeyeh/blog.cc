// Benchmarks an HTTP server and prints one line of results per round.
//
//   bazel run -c opt //perf:bench
//       Measures this repository's server, started inside this process.
//
//   bazel run -c opt //perf:bench -- --port=8080
//       Measures whatever server is already listening on that port. Use this
//       to compare two builds of the server, or this server against another.
//
// Always build with -c opt. Results drift by several percent from run to run
// with whatever else the machine is doing, which is why there are several
// rounds. The load generator shares the machine's cores with the server, so
// the numbers understate what the server could do with the machine to itself.

#include <cstdint>
#include <iostream>
#include <string>
#include <thread>
#include <utility>

#include "absl/base/log_severity.h"
#include "absl/flags/flag.h"
#include "absl/flags/parse.h"
#include "absl/log/globals.h"
#include "absl/log/initialize.h"
#include "absl/log/log.h"
#include "absl/status/status.h"
#include "absl/status/status_macros.h"
#include "absl/status/statusor.h"
#include "absl/time/time.h"
#include "http/message.h"
#include "http/server.h"
#include "perf/load.h"
#include "site/site.h"

ABSL_FLAG(std::string, address, "127.0.0.1",
          "IPv4 address of the server to measure");
ABSL_FLAG(uint16_t, port, 0,
          "Port of an already running server to measure. 0 means start this "
          "repository's server inside the benchmark and measure that");
ABSL_FLAG(std::string, root, "www",
          "With --port=0, the directory the server serves");
ABSL_FLAG(std::string, path, "/", "Page to request");
ABSL_FLAG(int, connections, 64, "Number of connections held open");
ABSL_FLAG(int, threads, 4, "Number of load-generating threads");
ABSL_FLAG(absl::Duration, duration, absl::Seconds(8),
          "Length of each measured round");
ABSL_FLAG(absl::Duration, warmup, absl::Seconds(1),
          "Time spent sending requests before each round's measurement starts");
ABSL_FLAG(int, rounds, 3, "Number of rounds");

namespace {

// Starts this repository's server on a free port, on threads that run until
// the process exits. Returns the port.
absl::StatusOr<uint16_t> StartServer() {
  ABSL_ASSIGN_OR_RETURN(Site loaded, Site::Load(absl::GetFlag(FLAGS_root)),
                        _.SetPrepend() << "--root: ");
  // Deliberately never freed: the server threads use it until the process
  // exits.
  const Site* const site = new Site(std::move(loaded));

  ABSL_ASSIGN_OR_RETURN(HttpServer server,
                        HttpServer::Listen(absl::GetFlag(FLAGS_address), 0));
  const uint16_t port = server.port();
  std::thread([server = std::move(server), site]() mutable {
    const absl::Status status = server.Run(
        [site](const Request& request) { return site->Handle(request); });
    LOG(ERROR) << "server stopped: " << status.message();
  }).detach();
  return port;
}

absl::Status Run() {
  LoadOptions options = {
      .address = absl::GetFlag(FLAGS_address),
      .port = absl::GetFlag(FLAGS_port),
      .path = absl::GetFlag(FLAGS_path),
      .connections = absl::GetFlag(FLAGS_connections),
      .threads = absl::GetFlag(FLAGS_threads),
      .duration = absl::ToChronoNanoseconds(absl::GetFlag(FLAGS_duration)),
      .warmup = absl::ToChronoNanoseconds(absl::GetFlag(FLAGS_warmup)),
  };
  if (options.port == 0) {
    ABSL_ASSIGN_OR_RETURN(options.port, StartServer());
  }

  // Flushed line by line so that each round shows up as it finishes.
  std::cout << "GET http://" << options.address << ":" << options.port
            << options.path << "   " << options.connections << " connections   "
            << options.threads << " threads   "
            << absl::FormatDuration(absl::GetFlag(FLAGS_duration))
            << " per round\n"
            << std::flush;
  for (int round = 1; round <= absl::GetFlag(FLAGS_rounds); ++round) {
    ABSL_ASSIGN_OR_RETURN(const LoadResult result, GenerateLoad(options));
    std::cout << "round " << round << ":   " << Describe(result) << "\n"
              << std::flush;
  }
  return absl::OkStatus();
}

}  // namespace

int main(int argc, char** argv) {
  absl::ParseCommandLine(argc, argv);
  absl::InitializeLog();
  absl::SetStderrThreshold(absl::LogSeverityAtLeast::kWarning);

#ifndef NDEBUG
  LOG(WARNING) << "this is an unoptimised build, so the results mean little; "
                  "use `bazel run -c opt //perf:bench`";
#endif

  const absl::Status status = Run();
  if (!status.ok()) {
    LOG(ERROR) << status.message();
    return 1;
  }
  return 0;
}

// A load generator for HTTP servers: the measuring half of a benchmark.
//
// It behaves like a crowd of impatient clients. Each holds one connection
// open and requests the same page over and over, sending the next request
// the moment the previous response arrives. The result says how many
// responses the server produced and how long each took.

#ifndef PERF_LOAD_H_
#define PERF_LOAD_H_

#include <chrono>
#include <cstdint>
#include <string>

#include "absl/status/statusor.h"
#include "perf/histogram.h"

struct LoadOptions {
  // The server to measure. `address` is a dotted IPv4 address.
  std::string address = "127.0.0.1";
  uint16_t port = 8080;

  // The page every client requests.
  std::string path = "/";

  // How many clients there are, each with one connection, and how many
  // threads they are spread over. More connections means more requests in
  // progress at once; more threads lets the generator itself keep up with a
  // fast server.
  int connections = 64;
  int threads = 4;

  // How long to measure for.
  std::chrono::nanoseconds duration = std::chrono::seconds(8);

  // How long to send requests before measuring starts. Gives connections time
  // to open and the server time to reach a steady state, so that neither
  // skews the result.
  std::chrono::nanoseconds warmup = std::chrono::seconds(1);
};

struct LoadResult {
  // Responses with status 200 received during the measured period.
  uint64_t requests = 0;

  // Everything else: responses with another status, failed requests and
  // failed connection attempts. A healthy run has none.
  uint64_t errors = 0;

  // The length of the measured period, which is `duration` give or take
  // scheduling.
  std::chrono::nanoseconds elapsed{0};

  // How long each counted response took, from sending the request to
  // receiving the last byte of the response.
  LatencyHistogram latency;

  // The headline number: requests divided by elapsed time.
  double RequestsPerSecond() const;
};

// Subjects a server to load as described by `options` and reports how it
// did. Blocks for the warmup plus the duration.
//
// Fails with InvalidArgument if the options make no sense, and with
// Unavailable if not one request succeeded, in which case the message says
// what went wrong with the first.
absl::StatusOr<LoadResult> GenerateLoad(const LoadOptions& options);

// A one-line summary of `result` for people, such as
// "685,001 requests/s   p50 57us   p99 2.60ms   max 21.15ms   0 errors".
std::string Describe(const LoadResult& result);

#endif  // PERF_LOAD_H_

#include "perf/load.h"

#include <algorithm>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>
#include <thread>
#include <utility>
#include <vector>

#include "absl/status/status.h"
#include "absl/status/status_macros.h"
#include "absl/status/statusor.h"
#include "absl/strings/str_cat.h"
#include "absl/strings/str_format.h"
#include "async/task.h"
#include "http/client.h"
#include "net/event_loop.h"
#include "perf/histogram.h"

namespace {

using Clock = std::chrono::steady_clock;

// How long a client waits before trying again after failing to connect, so
// that an absent server is not hammered with attempts.
constexpr std::chrono::milliseconds kReconnectDelay(10);

// What one thread's clients have observed. Each thread has its own, so
// recording needs no locking.
struct Tally {
  uint64_t requests = 0;
  uint64_t errors = 0;
  LatencyHistogram latency;
  std::chrono::nanoseconds measured{0};
  // What went wrong the first time something did.
  std::string first_error;

  void NoteError(std::string_view what) {
    ++errors;
    if (first_error.empty()) first_error = std::string(what);
  }
};

// One client: requests the page over and over, forever, reconnecting if the
// connection is lost. Responses that arrive before `measure_from` are not
// counted.
Task<> RunClient(const LoadOptions& options, Clock::time_point measure_from,
                 Tally& tally) {
  for (;;) {
    absl::StatusOr<HttpClient> client =
        co_await HttpClient::Connect(options.address, options.port);
    if (!client.ok()) {
      tally.NoteError(client.status().message());
      co_await Sleep(kReconnectDelay);
      continue;
    }

    for (;;) {
      const Clock::time_point sent = Clock::now();
      const absl::StatusOr<HttpClient::Result> response =
          co_await client->Get(options.path);
      const Clock::time_point received = Clock::now();

      if (!response.ok()) {
        tally.NoteError(response.status().message());
        // The connection is unusable now; start again with a new one.
        break;
      }
      if (response->status != 200) {
        tally.NoteError(absl::StrCat("status ", response->status));
      } else if (received >= measure_from) {
        ++tally.requests;
        tally.latency.Record(received - sent);
      }
    }
  }
}

// One thread's share of the work: starts `clients` clients, lets them run for
// the warmup and the duration, and returns. Returning is what stops the
// clients, since the event loop abandons tasks still running when its main
// task finishes.
Task<> RunClients(const LoadOptions& options, int clients, Tally& tally) {
  const Clock::time_point measure_from = Clock::now() + options.warmup;
  for (int i = 0; i < clients; ++i) {
    Spawn(RunClient(options, measure_from, tally));
  }
  co_await Sleep(options.warmup + options.duration);
  tally.measured = Clock::now() - measure_from;
}

// 1234567 -> "1,234,567".
std::string WithThousandsSeparators(uint64_t number) {
  std::string digits = absl::StrCat(number);
  for (int i = static_cast<int>(digits.size()) - 3; i > 0; i -= 3) {
    digits.insert(static_cast<size_t>(i), ",");
  }
  return digits;
}

// A latency in whichever unit reads best: "57us", "2.60ms", "1.25s".
std::string FormatLatency(std::chrono::nanoseconds latency) {
  const double nanoseconds = static_cast<double>(latency.count());
  if (nanoseconds < 1e3) return absl::StrFormat("%.0fns", nanoseconds);
  if (nanoseconds < 1e6) return absl::StrFormat("%.0fus", nanoseconds / 1e3);
  if (nanoseconds < 1e9) return absl::StrFormat("%.2fms", nanoseconds / 1e6);
  return absl::StrFormat("%.2fs", nanoseconds / 1e9);
}

}  // namespace

double LoadResult::RequestsPerSecond() const {
  if (elapsed <= std::chrono::nanoseconds::zero()) return 0;
  return static_cast<double>(requests) /
         std::chrono::duration<double>(elapsed).count();
}

absl::StatusOr<LoadResult> GenerateLoad(const LoadOptions& options) {
  if (options.threads < 1 || options.connections < options.threads) {
    return absl::InvalidArgumentError(
        "need at least one thread, and at least one connection per thread");
  }
  if (options.duration <= std::chrono::nanoseconds::zero()) {
    return absl::InvalidArgumentError("duration must be positive");
  }

  // Everything that can fail is done before the first thread starts.
  std::vector<EventLoop> loops;
  loops.reserve(options.threads);
  for (int i = 0; i < options.threads; ++i) {
    ABSL_ASSIGN_OR_RETURN(EventLoop loop, EventLoop::Create());
    loops.push_back(std::move(loop));
  }

  std::vector<Tally> tallies(options.threads);
  {
    std::vector<std::jthread> threads;
    threads.reserve(options.threads);
    for (int i = 0; i < options.threads; ++i) {
      // Spread the connections evenly, the first threads taking the remainder.
      const int clients = options.connections / options.threads +
                          (i < options.connections % options.threads ? 1 : 0);
      threads.emplace_back(
          [&options, &loop = loops[i], &tally = tallies[i], clients] {
            loop.Run(RunClients(options, clients, tally));
          });
    }
    // Leaving this scope waits for every thread to finish.
  }

  LoadResult result;
  std::string first_error;
  for (const Tally& tally : tallies) {
    result.requests += tally.requests;
    result.errors += tally.errors;
    result.latency.Merge(tally.latency);
    result.elapsed = std::max(result.elapsed, tally.measured);
    if (first_error.empty()) first_error = tally.first_error;
  }
  if (result.requests == 0) {
    return absl::UnavailableError(
        absl::StrCat("no successful response from http://", options.address,
                     ":", options.port, options.path, ": ", first_error));
  }
  return result;
}

std::string Describe(const LoadResult& result) {
  return absl::StrCat(WithThousandsSeparators(
                          static_cast<uint64_t>(result.RequestsPerSecond())),
                      " requests/s   p50 ",
                      FormatLatency(result.latency.Percentile(50)), "   p99 ",
                      FormatLatency(result.latency.Percentile(99)), "   max ",
                      FormatLatency(result.latency.max()), "   ",
                      WithThousandsSeparators(result.errors), " errors");
}

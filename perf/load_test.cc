// GenerateLoad by example: measuring a real server, started here on a free
// port, for a fraction of a second.

#include "perf/load.h"

#include <chrono>
#include <cstdint>
#include <thread>
#include <utility>

#include "absl/status/status.h"
#include "absl/status/status_matchers.h"
#include "absl/status/statusor.h"
#include "gmock/gmock.h"
#include "gtest/gtest.h"
#include "http/message.h"
#include "http/server.h"

using absl_testing::StatusIs;
using std::chrono::microseconds;
using std::chrono::milliseconds;
using testing::HasSubstr;

// Serves "/" successfully and nothing else.
Response OnlyRoot(const Request& request) {
  if (request.path != "/") return StatusResponse(404);
  return Response{
      .body = "hello",
  };
}

// Starts a server that runs until the test process exits, and returns its
// port.
uint16_t StartServer() {
  absl::StatusOr<HttpServer> server = HttpServer::Listen("127.0.0.1", 0);
  ABSL_EXPECT_OK(server);
  const uint16_t port = server->port();
  std::thread([server = std::move(*server)]() mutable {
    server.Run(OnlyRoot).IgnoreError();
  }).detach();
  return port;
}

// Options for a run short enough for a test.
LoadOptions QuickRun(uint16_t port) {
  return LoadOptions{
      .port = port,
      .connections = 4,
      .threads = 2,
      .duration = milliseconds(200),
      .warmup = milliseconds(50),
  };
}

// The basic use: point it at a server, get back how many requests were
// answered, how fast, and how long each took.
TEST(GenerateLoadTest, MeasuresAServer) {
  const absl::StatusOr<LoadResult> result =
      GenerateLoad(QuickRun(StartServer()));
  ABSL_ASSERT_OK(result);

  EXPECT_GT(result->requests, 0);
  EXPECT_EQ(result->errors, 0);
  EXPECT_GT(result->RequestsPerSecond(), 0);
  // Every counted request has a latency, and each took some time.
  EXPECT_EQ(result->latency.count(), result->requests);
  EXPECT_GT(result->latency.Percentile(50), std::chrono::nanoseconds(0));
  // The measured period is the duration; the warmup is not part of it.
  EXPECT_GE(result->elapsed, milliseconds(200));
  EXPECT_LT(result->elapsed, milliseconds(400));
}

// Only 200 responses count. Asking for a page that does not exist is the
// classic way to benchmark a server's error path by accident, so a run with
// no successes at all is reported as a failure, with the reason.
TEST(GenerateLoadTest, FailsWhenEveryResponseIsAnError) {
  LoadOptions options = QuickRun(StartServer());
  options.path = "/no-such-page";
  EXPECT_THAT(GenerateLoad(options), StatusIs(absl::StatusCode::kUnavailable,
                                              HasSubstr("status 404")));
}

TEST(GenerateLoadTest, FailsWhenNothingIsListening) {
  // The server is destroyed at the end of this line, leaving its port free.
  const uint16_t free_port = HttpServer::Listen("127.0.0.1", 0)->port();
  EXPECT_THAT(GenerateLoad(QuickRun(free_port)),
              StatusIs(absl::StatusCode::kUnavailable,
                       HasSubstr("Connection refused")));
}

TEST(GenerateLoadTest, RejectsOptionsThatMakeNoSense) {
  LoadOptions fewer_connections_than_threads;
  fewer_connections_than_threads.connections = 1;
  fewer_connections_than_threads.threads = 2;
  EXPECT_THAT(GenerateLoad(fewer_connections_than_threads),
              StatusIs(absl::StatusCode::kInvalidArgument));

  LoadOptions no_duration;
  no_duration.duration = milliseconds(0);
  EXPECT_THAT(GenerateLoad(no_duration),
              StatusIs(absl::StatusCode::kInvalidArgument));
}

// The rate is simply requests over time.
TEST(LoadResultTest, RequestsPerSecond) {
  LoadResult result;
  result.requests = 500;
  result.elapsed = milliseconds(250);
  EXPECT_DOUBLE_EQ(result.RequestsPerSecond(), 2000);
}

// One line with the numbers worth looking at: the rate, the typical latency,
// the bad latency, the worst, and whether anything went wrong.
TEST(DescribeTest, SummarisesAResultOnOneLine) {
  LoadResult result;
  result.requests = 1'234'567;
  result.elapsed = std::chrono::seconds(1);
  result.errors = 2;
  // All the same, so that the approximate percentiles are exact.
  for (int i = 0; i < 100; ++i) result.latency.Record(microseconds(64));

  EXPECT_EQ(Describe(result),
            "1,234,567 requests/s   p50 64us   p99 64us   max 64us   "
            "2 errors");
}

// Latencies are shown in whichever unit reads best.
TEST(DescribeTest, PicksAReadableUnit) {
  LoadResult result;
  result.latency.Record(milliseconds(4));
  EXPECT_THAT(Describe(result), HasSubstr("max 4.00ms"));
}

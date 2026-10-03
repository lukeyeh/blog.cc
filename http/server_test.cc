// HttpServer by example: the whole server as a client program sees it.

#include "http/server.h"

#include <benchmark/benchmark.h>

#include <cstdint>
#include <string>
#include <thread>
#include <utility>

#include "absl/status/status.h"
#include "absl/status/status_matchers.h"
#include "absl/status/statusor.h"
#include "async/task.h"
#include "gmock/gmock.h"
#include "gtest/gtest.h"
#include "http/client.h"
#include "http/message.h"
#include "net/event_loop.h"

using absl_testing::StatusIs;

// All a program has to supply: what to answer.
Response Hello(const Request& request) {
  return Response{
      .body = "hello " + request.path,
  };
}

// Port 0 asks for any free port, which keeps tests from clashing.
HttpServer ListenOnFreePort() {
  absl::StatusOr<HttpServer> server = HttpServer::Listen("127.0.0.1", 0);
  ABSL_EXPECT_OK(server);
  return std::move(*server);
}

// Listening is the step that fails, and it says why.
TEST(HttpServerTest, ListenFailsForBadAddress) {
  EXPECT_THAT(HttpServer::Listen("not-an-address", 8080),
              StatusIs(absl::StatusCode::kInvalidArgument));
}

TEST(HttpServerTest, ListenFailsWhenPortIsTaken) {
  const HttpServer occupant = ListenOnFreePort();
  EXPECT_THAT(HttpServer::Listen("127.0.0.1", occupant.port()),
              StatusIs(absl::StatusCode::kFailedPrecondition));
}

// The port is known as soon as Listen returns, before anything is served.
TEST(HttpServerTest, ReportsThePortItWasGiven) {
  EXPECT_NE(ListenOnFreePort().port(), 0);
}

Task<> FetchTwoPages(uint16_t port) {
  absl::StatusOr<HttpClient> client =
      co_await HttpClient::Connect("127.0.0.1", port);
  ABSL_EXPECT_OK(client);
  if (!client.ok()) co_return;

  for (const std::string path : {
           "/first",
           "/second",
       }) {
    const absl::StatusOr<HttpClient::Result> page = co_await client->Get(path);
    ABSL_EXPECT_OK(page);
    EXPECT_EQ(page->status, 200);
    EXPECT_EQ(page->body, "hello " + path);
  }
}

TEST(HttpServerTest, AnswersRequestsWithTheHandlersResponse) {
  HttpServer server = ListenOnFreePort();
  const uint16_t port = server.port();
  // A running server never returns from Run, so it gets a thread that is left
  // running when the test ends. Clients that connect before Run starts simply
  // wait their turn.
  std::thread([server = std::move(server)]() mutable {
    server.Run(Hello).IgnoreError();
  }).detach();

  absl::StatusOr<EventLoop> loop = EventLoop::Create();
  ABSL_ASSERT_OK(loop);
  loop->Run(FetchTwoPages(port));
}

// -----------------------------------------------------------------------------
// Benchmarks
// -----------------------------------------------------------------------------
//
// What a request costs when the server is on other threads.
//
//   bazel run -c opt //http:server_test -- --benchmark_filter=all
//
// Compare with BM_Get in //http:client_benchmark, where client and server
// share a thread. Here each request also has to cross between threads, by way
// of the kernel waking a sleeping server thread and then the client's, and
// that is where the extra time goes. For throughput with many connections,
// use //perf:bench instead.

namespace {

// Starts a server that runs until the process exits, and returns its port.
uint16_t StartServer() {
  HttpServer server = *HttpServer::Listen("127.0.0.1", 0);
  const uint16_t port = server.port();
  std::thread([server = std::move(server)]() mutable {
    server.Run(Hello).IgnoreError();
  }).detach();
  return port;
}

Task<> GetInLoop(benchmark::State& state, uint16_t port) {
  HttpClient client = *co_await HttpClient::Connect("127.0.0.1", port);
  for (auto _ : state) benchmark::DoNotOptimize(co_await client.Get("/"));
}

// One client making requests one after another to a server running on every
// core.
void BM_GetFromServerOnOtherThreads(benchmark::State& state) {
  static const uint16_t port = StartServer();
  EventLoop::Create()->Run(GetInLoop(state, port));
}
BENCHMARK(BM_GetFromServerOnOtherThreads);

}  // namespace

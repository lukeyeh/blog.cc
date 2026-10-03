// HttpClient by example: fetching pages from a server. The server here is
// this repository's own, answering on the same event loop as the client.

#include "http/client.h"

#include <benchmark/benchmark.h>

#include <cstdint>
#include <string>
#include <utility>

#include "absl/status/status.h"
#include "absl/status/status_matchers.h"
#include "absl/status/statusor.h"
#include "async/task.h"
#include "gmock/gmock.h"
#include "gtest/gtest.h"
#include "http/connection.h"
#include "http/message.h"
#include "http/server.h"
#include "net/event_loop.h"
#include "net/tcp.h"

using absl_testing::StatusIs;

// Serves "/big" (a large page), "/missing" (a 404) and, for anything else,
// the path it was asked for.
Response Pages(const Request& request) {
  if (request.path == "/big") {
    return Response{
        .body = std::string(200'000, 'x'),
    };
  }
  if (request.path == "/missing") return StatusResponse(404);
  return Response{
      .body = "you asked for " + request.path,
  };
}

// Serves every connection to `listener` with Pages.
Task<> ServePages(const TcpListener& listener, const Handler& handler) {
  for (;;) Spawn(ServeConnection(co_await listener.Accept(), handler));
}

// Runs `test` on an event loop alongside a server. `test` is given the port
// the server is listening on.
template <typename Test>
void RunWithServer(Test test) {
  absl::StatusOr<EventLoop> loop = EventLoop::Create();
  ABSL_ASSERT_OK(loop);
  const absl::StatusOr<TcpListener> listener =
      TcpListener::Listen("127.0.0.1", 0);
  ABSL_ASSERT_OK(listener);
  const Handler handler = Pages;
  loop->Run([](Test test, const TcpListener& listener,
               const Handler& handler) -> Task<> {
    Spawn(ServePages(listener, handler));
    co_await test(listener.port());
  }(std::move(test), *listener, handler));
}

// Connect, then Get: the result carries the status and the body.
TEST(HttpClientTest, GetsAPage) {
  RunWithServer([](uint16_t port) -> Task<> {
    absl::StatusOr<HttpClient> client =
        co_await HttpClient::Connect("127.0.0.1", port);
    ABSL_EXPECT_OK(client);
    const absl::StatusOr<HttpClient::Result> page =
        co_await client->Get("/hello");
    ABSL_EXPECT_OK(page);
    EXPECT_EQ(page->status, 200);
    EXPECT_EQ(page->body, "you asked for /hello");
  });
}

// One client is one connection, reused for every request.
TEST(HttpClientTest, MakesSeveralRequestsOnOneConnection) {
  RunWithServer([](uint16_t port) -> Task<> {
    absl::StatusOr<HttpClient> client =
        co_await HttpClient::Connect("127.0.0.1", port);
    ABSL_EXPECT_OK(client);
    for (const std::string path : {
             "/a",
             "/b",
             "/c",
         }) {
      const absl::StatusOr<HttpClient::Result> page =
          co_await client->Get(path);
      ABSL_EXPECT_OK(page);
      EXPECT_EQ(page->body, "you asked for " + path);
    }
  });
}

// The server saying "no" is an answer, not a failure of the client.
TEST(HttpClientTest, ErrorStatusIsStillAResult) {
  RunWithServer([](uint16_t port) -> Task<> {
    absl::StatusOr<HttpClient> client =
        co_await HttpClient::Connect("127.0.0.1", port);
    ABSL_EXPECT_OK(client);
    const absl::StatusOr<HttpClient::Result> page =
        co_await client->Get("/missing");
    ABSL_EXPECT_OK(page);
    EXPECT_EQ(page->status, 404);
    EXPECT_EQ(page->body, "Not Found\n");
  });
}

// Bodies far larger than one network packet arrive whole.
TEST(HttpClientTest, GetsLargeBodies) {
  RunWithServer([](uint16_t port) -> Task<> {
    absl::StatusOr<HttpClient> client =
        co_await HttpClient::Connect("127.0.0.1", port);
    ABSL_EXPECT_OK(client);
    const absl::StatusOr<HttpClient::Result> page =
        co_await client->Get("/big");
    ABSL_EXPECT_OK(page);
    EXPECT_EQ(page->body, std::string(200'000, 'x'));
  });
}

Task<> ConnectToNobody(uint16_t port) {
  EXPECT_THAT(co_await HttpClient::Connect("127.0.0.1", port),
              StatusIs(absl::StatusCode::kUnavailable));
}

TEST(HttpClientTest, ConnectFailsWhenNobodyListens) {
  // The listener is gone by the end of this line, leaving its port free.
  const uint16_t free_port = TcpListener::Listen("127.0.0.1", 0)->port();
  absl::StatusOr<EventLoop> loop = EventLoop::Create();
  ABSL_ASSERT_OK(loop);
  loop->Run(ConnectToNobody(free_port));
}

// -----------------------------------------------------------------------------
// Benchmarks
// -----------------------------------------------------------------------------
//
// What a request costs from the client's side, with and without keep-alive.
//
//   bazel run -c opt //http:client_test -- --benchmark_filter=all
//
// Get reuses one connection; ConnectAndGet opens a new one for every
// request, which is what a client or server without keep-alive does. The
// difference is the cost of setting up and tearing down a TCP connection.

namespace {

Response Hello(const Request&) {
  return Response{
      .body = Body::Borrowed("hello"),
  };
}

Task<> GetInLoop(benchmark::State& state) {
  const Handler handler = Hello;
  const TcpListener listener = *TcpListener::Listen("127.0.0.1", 0);
  Spawn(ServePages(listener, handler));

  HttpClient client =
      *co_await HttpClient::Connect("127.0.0.1", listener.port());
  for (auto _ : state) benchmark::DoNotOptimize(co_await client.Get("/"));
}

// Requests over one connection that stays open.
void BM_Get(benchmark::State& state) {
  EventLoop::Create()->Run(GetInLoop(state));
}
BENCHMARK(BM_Get);

Task<> ConnectAndGetInLoop(benchmark::State& state) {
  const Handler handler = Hello;
  const TcpListener listener = *TcpListener::Listen("127.0.0.1", 0);
  Spawn(ServePages(listener, handler));

  for (auto _ : state) {
    HttpClient client =
        *co_await HttpClient::Connect("127.0.0.1", listener.port());
    benchmark::DoNotOptimize(co_await client.Get("/"));
  }
}

// A new connection for every request.
void BM_ConnectAndGet(benchmark::State& state) {
  EventLoop::Create()->Run(ConnectAndGetInLoop(state));
}
BENCHMARK(BM_ConnectAndGet);

}  // namespace

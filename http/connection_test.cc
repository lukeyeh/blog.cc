// ServeConnection by example: what a client can send over one connection and
// what comes back. Each test is the client's side of a conversation with a
// server whose handler echoes the request.

#include "http/connection.h"

#include <benchmark/benchmark.h>

#include <cstddef>
#include <string>
#include <string_view>
#include <utility>

#include "absl/status/status.h"
#include "absl/status/status_matchers.h"
#include "async/task.h"
#include "gmock/gmock.h"
#include "gtest/gtest.h"
#include "http/message.h"
#include "http/server.h"
#include "net/event_loop.h"
#include "net/tcp.h"

using absl_testing::IsOkAndHolds;
using absl_testing::StatusIs;
using testing::HasSubstr;
using testing::StartsWith;

// Answers every request with "<method> <path>\n".
Response Echo(const Request& request) {
  return Response{
      .body = std::string(request.method) + " " + request.path + "\n",
  };
}

// Runs `client` against a server answering with Echo, on one event loop.
// `client` is given a connection to the server.
template <typename Client>
void RunAgainstServer(Client client) {
  absl::StatusOr<EventLoop> loop = EventLoop::Create();
  ABSL_ASSERT_OK(loop);
  const Handler handler = Echo;
  loop->Run([](Client client, const Handler& handler) -> Task<> {
    const absl::StatusOr<TcpListener> listener =
        TcpListener::Listen("127.0.0.1", 0);
    ABSL_EXPECT_OK(listener);
    absl::StatusOr<TcpConnection> connection =
        co_await TcpConnection::Connect("127.0.0.1", listener->port());
    ABSL_EXPECT_OK(connection);
    // The server's side runs as its own task while the client talks to it.
    Spawn(ServeConnection(co_await listener->Accept(), handler));
    co_await client(*connection);
  }(std::move(client), handler));
}

// Reads one response whose body is a single line, and returns its head.
// Checks that the body is `expected_body`.
Task<std::string> ReadResponse(TcpConnection& connection,
                               std::string expected_body) {
  const absl::StatusOr<std::string_view> head =
      co_await connection.ReadUntil("\r\n\r\n", 1000);
  ABSL_EXPECT_OK(head);
  const std::string result(head.ok() ? *head : "");
  EXPECT_THAT(co_await connection.ReadUntil("\n", 1000),
              IsOkAndHolds(expected_body));
  co_return result;
}

// Keep-alive: the connection stays open after a response, and nothing in the
// response says otherwise.
TEST(ServeConnectionTest, AnswersSeveralRequestsOnOneConnection) {
  RunAgainstServer([](TcpConnection& connection) -> Task<> {
    co_await connection.Write("GET /one HTTP/1.1\r\nHost: x\r\n\r\n");
    const std::string first = co_await ReadResponse(connection, "GET /one");
    EXPECT_THAT(first, StartsWith("HTTP/1.1 200 OK\r\n"));
    EXPECT_THAT(first, HasSubstr("Content-Length: 9"));
    EXPECT_THAT(first, Not(HasSubstr("Connection: close")));

    co_await connection.Write("GET /two HTTP/1.1\r\n\r\n");
    co_await ReadResponse(connection, "GET /two");
  });
}

// Pipelining: a client need not wait for one answer before asking again.
TEST(ServeConnectionTest, AnswersRequestsSentTogetherInOrder) {
  RunAgainstServer([](TcpConnection& connection) -> Task<> {
    co_await connection.Write("GET /a HTTP/1.1\r\n\r\nGET /b HTTP/1.1\r\n\r\n");
    co_await ReadResponse(connection, "GET /a");
    co_await ReadResponse(connection, "GET /b");
  });
}

// HEAD asks what a GET would return, without the content: same headers,
// including the length, but no body.
TEST(ServeConnectionTest, HeadGetsHeadersWithoutBody) {
  RunAgainstServer([](TcpConnection& connection) -> Task<> {
    co_await connection.Write(
        "HEAD /x HTTP/1.1\r\n\r\nGET /y HTTP/1.1\r\n\r\n");
    EXPECT_THAT(co_await connection.ReadUntil("\r\n\r\n", 1000),
                IsOkAndHolds(HasSubstr("Content-Length: 8")));
    // If the HEAD response had a body it would come before this one.
    const std::string next = co_await ReadResponse(connection, "GET /y");
    EXPECT_THAT(next, StartsWith("HTTP/1.1 200 OK\r\n"));
  });
}

// A client that wants the connection closed gets told that it will be, and
// then it is.
TEST(ServeConnectionTest, ClosesWhenClientAsks) {
  RunAgainstServer([](TcpConnection& connection) -> Task<> {
    co_await connection.Write("GET /bye HTTP/1.1\r\nConnection: close\r\n\r\n");
    const std::string head = co_await ReadResponse(connection, "GET /bye");
    EXPECT_THAT(head, HasSubstr("Connection: close"));
    EXPECT_THAT(co_await connection.ReadUntil("\n", 1000),
                StatusIs(absl::StatusCode::kUnavailable));
  });
}

// Text that is not HTTP never reaches the handler. The server cannot tell
// where the next request would start, so it also hangs up.
TEST(ServeConnectionTest, MalformedRequestGets400AndClose) {
  RunAgainstServer([](TcpConnection& connection) -> Task<> {
    co_await connection.Write("nonsense\r\n\r\n");
    const std::string head = co_await ReadResponse(connection, "Bad Request");
    EXPECT_THAT(head, StartsWith("HTTP/1.1 400 Bad Request\r\n"));
    EXPECT_THAT(co_await connection.ReadUntil("\n", 1000),
                StatusIs(absl::StatusCode::kUnavailable));
  });
}

// The server will not buffer an unbounded request head.
TEST(ServeConnectionTest, OversizedHeadGets431) {
  RunAgainstServer([](TcpConnection& connection) -> Task<> {
    const std::string huge(20'000, 'a');
    co_await connection.Write("GET / HTTP/1.1\r\nX-Big: ", huge);
    const std::string head =
        co_await ReadResponse(connection, "Request Header Fields Too Large");
    EXPECT_THAT(head, StartsWith("HTTP/1.1 431 "));
  });
}

// -----------------------------------------------------------------------------
// Benchmarks
// -----------------------------------------------------------------------------
//
// What the server spends on one request, start to finish.
//
//   bazel run -c opt //http:connection_test -- --benchmark_filter=all
//
// One client and one ServeConnection share an event loop, so every request
// is: client writes, server reads and parses, handler runs, server writes,
// client reads. There is no second thread and no contention, which makes
// this the cleanest view of the per-request path. Profile this one to see
// where a request's time goes.

namespace {

constexpr size_t kNoLimit = size_t{1} << 30;

Task<> RequestInLoop(benchmark::State& state) {
  // The page served: as many bytes as the benchmark's argument says.
  const std::string page(static_cast<size_t>(state.range(0)), 'x');
  const Handler handler = [&page](const Request&) {
    return Response{
        .body = Body::Borrowed(page),
    };
  };

  const TcpListener listener = *TcpListener::Listen("127.0.0.1", 0);
  TcpConnection client =
      *co_await TcpConnection::Connect("127.0.0.1", listener.port());
  Spawn(ServeConnection(co_await listener.Accept(), handler));

  for (auto _ : state) {
    co_await client.Write("GET /page HTTP/1.1\r\nHost: x\r\n\r\n");
    benchmark::DoNotOptimize(co_await client.ReadUntil("\r\n\r\n", kNoLimit));
    benchmark::DoNotOptimize(co_await client.Read(page.size()));
  }
  state.SetBytesProcessed(state.iterations() * state.range(0));
}

// One request and its response on an open connection, for several page
// sizes. The body is never copied by the server, so time should grow with
// size only as fast as the kernel's own copying does.
void BM_RequestAndResponse(benchmark::State& state) {
  EventLoop::Create()->Run(RequestInLoop(state));
}
BENCHMARK(BM_RequestAndResponse)->Arg(100)->Arg(10'000)->Arg(1'000'000);

}  // namespace

// The HTTP message format by example: what request text parses to, and what
// a response looks like on the wire.

#include "http/message.h"

#include <benchmark/benchmark.h>

#include <cstdint>
#include <optional>
#include <string>
#include <string_view>

#include "absl/status/status.h"
#include "absl/status/status_matchers.h"
#include "gmock/gmock.h"
#include "gtest/gtest.h"

using absl_testing::StatusIs;

// A request head is a request line, "<method> <target> <version>", then
// headers. The target splits at '?' into a path, which is percent-decoded,
// and a query, which is left as sent.
TEST(ParseRequestTest, ParsesRequestLine) {
  const auto request = ParseRequest(
      "GET /a%20b/c.html?x=1 HTTP/1.1\r\nHost: example.com\r\nX-Thing:  y ");
  ABSL_ASSERT_OK(request);
  EXPECT_EQ(request->method, "GET");
  EXPECT_EQ(request->path, "/a b/c.html");
  EXPECT_EQ(request->query, "x=1");
  EXPECT_TRUE(request->keep_alive);
}

// Headers are optional.
TEST(ParseRequestTest, ParsesRequestWithoutHeaders) {
  const auto request = ParseRequest("HEAD / HTTP/1.1");
  ABSL_ASSERT_OK(request);
  EXPECT_EQ(request->method, "HEAD");
  EXPECT_EQ(request->path, "/");
  EXPECT_EQ(request->query, "");
}

// Anything else is an error, which the server answers with 400.
TEST(ParseRequestTest, RejectsMalformedInput) {
  // Too few parts, an empty method, a space inside the target.
  EXPECT_THAT(ParseRequest(""), StatusIs(absl::StatusCode::kInvalidArgument));
  EXPECT_THAT(ParseRequest("GET /"),
              StatusIs(absl::StatusCode::kInvalidArgument));
  EXPECT_THAT(ParseRequest(" / HTTP/1.1"),
              StatusIs(absl::StatusCode::kInvalidArgument));
  EXPECT_THAT(ParseRequest("GET /a b HTTP/1.1"),
              StatusIs(absl::StatusCode::kInvalidArgument));
  // Not HTTP/1.x.
  EXPECT_THAT(ParseRequest("GET / SPDY/3"),
              StatusIs(absl::StatusCode::kInvalidArgument));
  // Only paths are supported as targets, not full URLs or host names.
  EXPECT_THAT(ParseRequest("GET example.com HTTP/1.1"),
              StatusIs(absl::StatusCode::kInvalidArgument));
  // %zz is not a valid escape.
  EXPECT_THAT(ParseRequest("GET /%zz HTTP/1.1"),
              StatusIs(absl::StatusCode::kInvalidArgument));
  EXPECT_THAT(ParseRequest("GET / HTTP/1.1\r\nno colon here"),
              StatusIs(absl::StatusCode::kInvalidArgument));
}

// Whether a request with the given first line and one header leaves the
// connection usable.
bool KeepsAlive(const std::string& request_line, const std::string& header) {
  const std::string head = request_line + "\r\n" + header;
  const auto request = ParseRequest(head);
  ABSL_EXPECT_OK(request);
  return request->keep_alive;
}

// `keep_alive` answers one question for the server: can another request
// follow this one on the same connection?
TEST(ParseRequestTest, KeepAliveFollowsVersionAndHeaders) {
  // HTTP/1.1 connections persist by default; HTTP/1.0 ones do not.
  EXPECT_TRUE(KeepsAlive("GET / HTTP/1.1", "Host: x"));
  EXPECT_TRUE(KeepsAlive("GET / HTTP/1.1", "Content-Length: 0"));
  EXPECT_FALSE(KeepsAlive("GET / HTTP/1.0", "Host: x"));
  // The client may ask to close. Header names and values ignore case.
  EXPECT_FALSE(KeepsAlive("GET / HTTP/1.1", "connection: Close"));
  // A request with a body: the server never reads bodies, so it could not
  // find the start of the next request.
  EXPECT_FALSE(KeepsAlive("POST / HTTP/1.1", "Content-Length: 12"));
  EXPECT_FALSE(KeepsAlive("POST / HTTP/1.1", "Transfer-Encoding: chunked"));
}

// A client needs two things from a response head: the status, and how much
// body to read.
TEST(ParseResponseHeadTest, ParsesStatusAndContentLength) {
  const auto head = ParseResponseHead(
      "HTTP/1.1 404 Not Found\r\nContent-Type: text/plain\r\n"
      "content-length: 10");
  ABSL_ASSERT_OK(head);
  EXPECT_EQ(head->status, 404);
  EXPECT_EQ(head->content_length, 10);
}

// A response with no Content-Length has no body, as after a HEAD request's
// 304 or 204.
TEST(ParseResponseHeadTest, ContentLengthDefaultsToZero) {
  const auto head = ParseResponseHead("HTTP/1.1 204 No Content");
  ABSL_ASSERT_OK(head);
  EXPECT_EQ(head->status, 204);
  EXPECT_EQ(head->content_length, 0);
}

TEST(ParseResponseHeadTest, RejectsMalformedInput) {
  EXPECT_THAT(ParseResponseHead("nonsense"),
              StatusIs(absl::StatusCode::kInvalidArgument));
  EXPECT_THAT(ParseResponseHead("HTTP/1.1 abc Oops"),
              StatusIs(absl::StatusCode::kInvalidArgument));
  EXPECT_THAT(ParseResponseHead("HTTP/1.1 200 OK\r\nContent-Length: many"),
              StatusIs(absl::StatusCode::kInvalidArgument));
}

// Chunked bodies announce their length piece by piece. This server never
// sends them, and the client does not read them.
TEST(ParseResponseHeadTest, ChunkedResponsesAreUnsupported) {
  EXPECT_THAT(
      ParseResponseHead("HTTP/1.1 200 OK\r\nTransfer-Encoding: chunked"),
      StatusIs(absl::StatusCode::kUnimplemented));
}

// %XX stands for the byte with hexadecimal value XX, in either case.
TEST(PercentDecodeTest, Decodes) {
  EXPECT_EQ(PercentDecode("/plain"), "/plain");
  EXPECT_EQ(PercentDecode("%2e%2E/%41"), "../A");
  // '+' means a space only in form data, not in paths.
  EXPECT_EQ(PercentDecode("a+b"), "a+b");
}

// An encoded NUL is refused because it would cut a path short wherever the
// path is later treated as a C string.
TEST(PercentDecodeTest, RejectsTruncatedInvalidAndNul) {
  EXPECT_EQ(PercentDecode("%"), std::nullopt);
  EXPECT_EQ(PercentDecode("%4"), std::nullopt);
  EXPECT_EQ(PercentDecode("%g0"), std::nullopt);
  EXPECT_EQ(PercentDecode("a%00b"), std::nullopt);
}

// The head is everything before the body. It is appended to whatever the
// string already holds.
TEST(AppendResponseHeadTest, WritesStatusLineAndHeaders) {
  Response response{
      .status = 404,
      .body = "nope",
  };
  response.headers.emplace_back("X-Extra", "1");
  std::string head = "previous|";
  AppendResponseHead(response, /*keep_alive=*/true, &head);
  EXPECT_EQ(head,
            "previous|"
            "HTTP/1.1 404 Not Found\r\n"
            "Content-Type: text/plain; charset=utf-8\r\n"
            "Content-Length: 4\r\n"
            "X-Extra: 1\r\n"
            "\r\n");
}

// Staying open is the default in HTTP/1.1, so only closing is announced.
TEST(AppendResponseHeadTest, AnnouncesClosingConnection) {
  std::string head;
  AppendResponseHead(Response{}, /*keep_alive=*/false, &head);
  EXPECT_THAT(head, testing::HasSubstr("\r\nConnection: close\r\n"));
}

// A Body either refers to bytes held elsewhere or carries its own.
TEST(BodyTest, OwnsOrBorrows) {
  const std::string elsewhere = "borrowed bytes";
  const Body borrowed = Body::Borrowed(elsewhere);
  // The very same memory: nothing was copied.
  EXPECT_EQ(borrowed.view().data(), elsewhere.data());

  const Body owned = std::string("owned bytes");
  EXPECT_EQ(owned.view(), "owned bytes");
  EXPECT_EQ(Body().view(), "");
}

// -----------------------------------------------------------------------------
// Benchmarks
// -----------------------------------------------------------------------------
//
// What understanding and producing HTTP text costs.
//
//   bazel run -c opt //http:message_test -- --benchmark_filter=all
//
// This is pure computation, done once per request on top of the I/O. Compare
// it with //net:tcp_benchmark to see how small a share of a request it is.

namespace {

// What a browser really sends for a page.
constexpr std::string_view kBrowserRequest =
    "GET /blog/2026/a-post.html?utm_source=feed HTTP/1.1\r\n"
    "Host: example.com\r\n"
    "User-Agent: Mozilla/5.0 (X11; Linux x86_64; rv:130.0) Gecko/20100101 "
    "Firefox/130.0\r\n"
    "Accept: "
    "text/html,application/xhtml+xml,application/xml;q=0.9,*/*;q=0.8\r\n"
    "Accept-Language: en-GB,en;q=0.5\r\n"
    "Accept-Encoding: gzip, deflate, br, zstd\r\n"
    "Connection: keep-alive\r\n"
    "Upgrade-Insecure-Requests: 1\r\n"
    "Sec-Fetch-Dest: document\r\n"
    "Sec-Fetch-Mode: navigate\r\n"
    "Sec-Fetch-Site: none";

// The least a client can send: what the load generator and curl-like tools
// use.
void BM_ParseMinimalRequest(benchmark::State& state) {
  for (auto _ : state) {
    benchmark::DoNotOptimize(ParseRequest("GET / HTTP/1.1\r\nHost: x"));
  }
}
BENCHMARK(BM_ParseMinimalRequest);

// A realistic request. Most of the difference from the minimal one is
// walking the headers.
void BM_ParseBrowserRequest(benchmark::State& state) {
  for (auto _ : state) benchmark::DoNotOptimize(ParseRequest(kBrowserRequest));
  state.SetBytesProcessed(state.iterations() *
                          static_cast<int64_t>(kBrowserRequest.size()));
}
BENCHMARK(BM_ParseBrowserRequest);

// A path with %XX escapes takes a slower route that builds a decoded copy.
void BM_ParseRequestWithEscapedPath(benchmark::State& state) {
  for (auto _ : state) {
    benchmark::DoNotOptimize(
        ParseRequest("GET /my%20files/a%2Fb%20c.html HTTP/1.1\r\nHost: x"));
  }
}
BENCHMARK(BM_ParseRequestWithEscapedPath);

void BM_PercentDecode(benchmark::State& state) {
  for (auto _ : state) {
    benchmark::DoNotOptimize(PercentDecode("/my%20files/a%2Fb%20c.html"));
  }
}
BENCHMARK(BM_PercentDecode);

// Writing a response's status line and headers into a string that is reused
// from one response to the next, as the server does.
void BM_AppendResponseHead(benchmark::State& state) {
  const Response response = {
      .content_type = "text/html; charset=utf-8",
      .body = Body::Borrowed("<h1>hello</h1>"),
  };
  std::string head;
  for (auto _ : state) {
    head.clear();
    AppendResponseHead(response, /*keep_alive=*/true, &head);
    benchmark::DoNotOptimize(head);
  }
}
BENCHMARK(BM_AppendResponseHead);

// The client's side: reading the status and length out of a response head.
void BM_ParseResponseHead(benchmark::State& state) {
  for (auto _ : state) {
    benchmark::DoNotOptimize(
        ParseResponseHead("HTTP/1.1 200 OK\r\n"
                          "Content-Type: text/html; charset=utf-8\r\n"
                          "Content-Length: 330"));
  }
}
BENCHMARK(BM_ParseResponseHead);

}  // namespace

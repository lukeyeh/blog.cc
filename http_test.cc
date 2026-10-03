#include "http.h"

#include "gtest/gtest.h"

TEST(ParseRequestTest, ParsesRequestLineAndHeaders) {
  auto request = ParseRequest(
      "GET /a%20b/c.html?x=1 HTTP/1.1\r\nHost: example.com\r\nX-Thing:  y ");
  ASSERT_TRUE(request.ok()) << request.status();
  EXPECT_EQ(request->method, "GET");
  EXPECT_EQ(request->path, "/a b/c.html");
  EXPECT_EQ(request->query, "x=1");
  EXPECT_EQ(request->headers.at("host"), "example.com");
  EXPECT_EQ(request->headers.at("x-thing"), "y");
}

TEST(ParseRequestTest, RejectsMalformedInput) {
  EXPECT_FALSE(ParseRequest("").ok());
  EXPECT_FALSE(ParseRequest("GET /").ok());
  EXPECT_FALSE(ParseRequest("GET / SPDY/3").ok());
  EXPECT_FALSE(ParseRequest("GET example.com HTTP/1.1").ok());
  EXPECT_FALSE(ParseRequest("GET /%zz HTTP/1.1").ok());
  EXPECT_FALSE(ParseRequest("GET / HTTP/1.1\r\nno colon here").ok());
}

TEST(PercentDecodeTest, Decodes) {
  EXPECT_EQ(PercentDecode("/plain"), "/plain");
  EXPECT_EQ(PercentDecode("%2e%2E/%41"), "../A");
  EXPECT_EQ(PercentDecode("a+b"), "a+b");
}

TEST(PercentDecodeTest, RejectsTruncatedInvalidAndNul) {
  EXPECT_EQ(PercentDecode("%"), std::nullopt);
  EXPECT_EQ(PercentDecode("%4"), std::nullopt);
  EXPECT_EQ(PercentDecode("%g0"), std::nullopt);
  EXPECT_EQ(PercentDecode("a%00b"), std::nullopt);
}

TEST(SerializeResponseTest, WritesStatusLineHeadersAndBody) {
  Response response{.status = 404, .body = "nope"};
  response.headers.emplace_back("X-Extra", "1");
  EXPECT_EQ(SerializeResponse(response),
            "HTTP/1.1 404 Not Found\r\n"
            "Content-Type: text/plain; charset=utf-8\r\n"
            "Content-Length: 4\r\n"
            "Connection: close\r\n"
            "X-Extra: 1\r\n"
            "\r\n"
            "nope");
}

TEST(SerializeResponseTest, HeadKeepsContentLengthButDropsBody) {
  std::string wire = SerializeResponse(Response{.body = "hello"}, false);
  EXPECT_NE(wire.find("Content-Length: 5\r\n"), std::string::npos);
  EXPECT_TRUE(wire.ends_with("\r\n\r\n"));
}

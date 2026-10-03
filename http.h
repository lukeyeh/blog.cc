// The HTTP/1.1 message format: converting between the text that travels over
// a connection and the Request and Response values the rest of the program
// works with. Knows nothing about connections or about what a request means.

#ifndef HTTP_H_
#define HTTP_H_

#include <map>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "absl/status/statusor.h"

struct Request {
  std::string method;
  // Percent-decoded, always starts with '/'.
  std::string path;
  // Raw text after '?', not decoded.
  std::string query;
  // Names are lower-cased.
  std::map<std::string, std::string> headers;
};

struct Response {
  int status = 200;
  std::string content_type = "text/plain; charset=utf-8";
  std::string body;
  // Extra headers. Content-Type, Content-Length and Connection are added by
  // SerializeResponse.
  std::vector<std::pair<std::string, std::string>> headers;
};

// The bytes that mark the end of a request head. A head is the request line
// and headers; everything a client sends before this marker.
inline constexpr std::string_view kHeadTerminator = "\r\n\r\n";

// Parses a request head, given without its kHeadTerminator. Fails with
// InvalidArgument, and a message saying what is wrong, if `head` is not a
// well-formed HTTP/1.x request.
absl::StatusOr<Request> ParseRequest(std::string_view head);

// Returns the bytes to put on the wire. HEAD responses pass
// include_body = false, which keeps Content-Length but drops the body.
std::string SerializeResponse(const Response& response, bool include_body = true);

// Decodes %XX escapes. Returns nullopt for malformed escapes and for an
// encoded NUL byte.
std::optional<std::string> PercentDecode(std::string_view text);

std::string_view ReasonPhrase(int status);

// A plain-text response whose body is the reason phrase, e.g. "Not Found\n".
Response StatusResponse(int status);

#endif  // HTTP_H_

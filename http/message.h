// The HTTP/1.1 message format: converting between the text that travels over
// a connection and the Request and Response values the rest of the program
// works with. Knows nothing about connections or about what a request means.

#ifndef HTTP_MESSAGE_H_
#define HTTP_MESSAGE_H_

#include <cstddef>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <variant>
#include <vector>

#include "absl/status/statusor.h"

// A request as a handler sees it.
//
// To avoid copying, `method` and `query` refer to the text the request was
// parsed from, so a Request must not outlive that text. Handlers receive a
// request that is valid for the duration of the call.
struct Request {
  std::string_view method;
  // Percent-decoded, always starts with '/'.
  std::string path;
  // Raw text after '?', not decoded.
  std::string_view query;
  // Whether the client can receive another response on the same connection
  // after this one. False if the client asked to close, speaks HTTP/1.0, or
  // sent a request body, which this server does not read.
  bool keep_alive = true;
};

// The content of a response. Either owns its bytes, which suits content
// computed for one request, or borrows bytes that live elsewhere, which lets
// content that is served repeatedly be sent without ever being copied.
class Body {
 public:
  Body() = default;

  // An owning body. Implicit so that `.body = "text"` and
  // `.body = std::move(string)` read naturally.
  Body(std::string text) : bytes_(std::move(text)) {}    // NOLINT
  Body(const char* text) : bytes_(std::string(text)) {}  // NOLINT

  // A body that refers to `bytes` without copying them. `bytes` must stay
  // valid and unchanged until the response has been sent.
  static Body Borrowed(std::string_view bytes) {
    Body body;
    body.bytes_ = bytes;
    return body;
  }

  std::string_view view() const {
    if (const std::string* const owned = std::get_if<std::string>(&bytes_)) {
      return *owned;
    }
    return std::get<std::string_view>(bytes_);
  }

 private:
  std::variant<std::string, std::string_view> bytes_;
};

struct Response {
  int status = 200;
  // Must refer to text that outlives the response; in practice a literal.
  std::string_view content_type = "text/plain; charset=utf-8";
  Body body;
  // Extra headers. Content-Type, Content-Length and Connection are added by
  // AppendResponseHead.
  std::vector<std::pair<std::string, std::string>> headers;
};

// The bytes that mark the end of a request head. A head is the request line
// and headers; everything a client sends before this marker.
inline constexpr std::string_view kHeadTerminator = "\r\n\r\n";

// Parses a request head, given without its kHeadTerminator. Fails with
// InvalidArgument, and a message saying what is wrong, if `head` is not a
// well-formed HTTP/1.x request.
//
// The result refers to `head`; see Request.
absl::StatusOr<Request> ParseRequest(std::string_view head);

// What the head of a response tells a client: the outcome, and how many
// bytes of body follow.
struct ResponseHead {
  int status = 0;
  size_t content_length = 0;
};

// Parses a response head, given without its kHeadTerminator. The client-side
// counterpart of ParseRequest.
//
// Fails with InvalidArgument if `head` is not a well-formed HTTP/1.x
// response, and with Unimplemented if the body's length is not given by a
// Content-Length header, as with chunked responses.
absl::StatusOr<ResponseHead> ParseResponseHead(std::string_view head);

// Appends to `head` everything of `response` that goes on the wire before the
// body: status line, headers, and the blank line that ends them. To send the
// response, write this and then, unless the request was a HEAD, the body.
//
// `keep_alive` says whether the connection stays open afterwards, which the
// client has to be told.
//
// Appends rather than returns so that a caller answering many requests can
// reuse one string's storage.
void AppendResponseHead(const Response& response, bool keep_alive,
                        std::string* head);

// Decodes %XX escapes. Returns nullopt for malformed escapes and for an
// encoded NUL byte.
std::optional<std::string> PercentDecode(std::string_view text);

std::string_view ReasonPhrase(int status);

// A plain-text response whose body is the reason phrase, e.g. "Not Found\n".
Response StatusResponse(int status);

#endif  // HTTP_MESSAGE_H_

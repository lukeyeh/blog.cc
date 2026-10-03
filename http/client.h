// An HTTP client: the other end of the conversation from server.h. Enough to
// fetch pages from a server, for tests, tools and measurements; not a
// general-purpose client.

#ifndef HTTP_CLIENT_H_
#define HTTP_CLIENT_H_

#include <cstdint>
#include <string>
#include <string_view>

#include "absl/status/statusor.h"
#include "async/task.h"
#include "net/tcp.h"

// One connection to an HTTP server, over which any number of requests can be
// made one after another.
//
// Belongs to the event loop of the thread that connected it, and must not be
// moved or destroyed while a request is in progress.
class HttpClient {
 public:
  // What came back for one request.
  struct Result {
    int status = 0;
    // Refers to memory inside the client; valid until the next request.
    std::string_view body;
  };

  // Connects to the server at address:port. `address` is a dotted IPv4
  // address.
  static Task<absl::StatusOr<HttpClient>> Connect(std::string address,
                                                  uint16_t port);

  HttpClient(HttpClient&&) = default;
  HttpClient& operator=(HttpClient&&) = default;

  // Requests `path`, which must start with '/', and waits for the complete
  // response. A response with an error status such as 404 is still a
  // successful Get; look at Result::status.
  //
  // Fails if the server disconnects or answers with something that is not a
  // response this client understands. After a failure the connection is
  // unusable.
  Task<absl::StatusOr<Result>> Get(std::string_view path);

 private:
  HttpClient(TcpConnection connection, std::string host);

  TcpConnection connection_;

  // The value of the Host header every request must carry.
  std::string host_;

  // The text of the request being sent. A member so that its storage is
  // reused from one request to the next.
  std::string request_;
};

#endif  // HTTP_CLIENT_H_

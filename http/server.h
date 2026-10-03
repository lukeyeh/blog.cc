// An HTTP server that owns everything about talking to clients, so that the
// rest of the program only has to say what the response to a request is.

#ifndef HTTP_SERVER_H_
#define HTTP_SERVER_H_

#include <cstdint>
#include <functional>
#include <memory>
#include <string>

#include "absl/status/status.h"
#include "absl/status/statusor.h"
#include "http/message.h"

class TcpListener;

// Decides the response to one request. Called concurrently from several
// threads, so it must be thread-safe, and for every request, so it should be
// quick: a handler that blocks delays every other client on its thread.
//
// The request is valid only for the duration of the call.
using Handler = std::function<Response(const Request&)>;

// An HTTP server. Starting one takes two steps, so that a program can find
// out whether, and on which port, it is listening before it commits a thread
// to serving:
//
//   ABSL_ASSIGN_OR_RETURN(HttpServer server,
//                         HttpServer::Listen("127.0.0.1", 8080));
//   return server.Run(handler);
class HttpServer {
 public:
  // Claims address:port. Clients can connect from this point on, and wait
  // until Run starts answering. Port 0 asks for any free port; port() reports
  // which one was chosen.
  //
  // Fails if the address is invalid or the port cannot be claimed, typically
  // because another program is using it or it requires privileges.
  static absl::StatusOr<HttpServer> Listen(const std::string& address,
                                           uint16_t port);

  HttpServer(HttpServer&&);
  HttpServer& operator=(HttpServer&&);
  ~HttpServer();

  // The port being listened on.
  uint16_t port() const;

  // Answers every request with whatever `handler` returns, using every CPU
  // core. A client may send any number of requests over one connection.
  //
  // Clients that send something that is not a valid request are answered with
  // the appropriate error status here; the handler only ever sees well-formed
  // requests.
  //
  // Does not return while the server is running. Returns only if serving
  // could not start, with the reason.
  absl::Status Run(const Handler& handler);

 private:
  explicit HttpServer(std::unique_ptr<TcpListener> listener);

  // Held by pointer so that this header does not expose the network layer.
  std::unique_ptr<TcpListener> listener_;
};

#endif  // HTTP_SERVER_H_

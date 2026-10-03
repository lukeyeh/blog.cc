// An HTTP server that owns everything about talking to clients, so that the
// rest of the program only has to say what the response to a request is.

#ifndef SERVER_H_
#define SERVER_H_

#include <cstdint>
#include <functional>
#include <string>

#include "absl/status/status.h"
#include "http.h"

// Decides the response to one request. Called concurrently from several
// threads, so it must be thread-safe.
using Handler = std::function<Response(const Request&)>;

// Runs an HTTP server on address:port, answering every request with whatever
// `handler` returns. Each connection carries one request and is then closed.
//
// Clients that send something that is not a valid request are answered with
// the appropriate error status here; the handler only ever sees well-formed
// requests.
//
// Does not return while the server is running. Returns only if the server
// could not start, with the reason.
absl::Status Serve(const std::string& address, uint16_t port,
                   const Handler& handler);

#endif  // SERVER_H_

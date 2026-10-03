#ifndef SERVER_H_
#define SERVER_H_

#include <cstdint>
#include <functional>
#include <string>

#include "absl/status/status.h"
#include "http.h"

using Handler = std::function<Response(const Request&)>;

// Listens on address:port and answers each connection with handler, one
// request per connection. The handler is called from multiple threads. Only
// returns on a setup failure.
absl::Status Serve(const std::string& address, uint16_t port,
                   const Handler& handler);

#endif  // SERVER_H_

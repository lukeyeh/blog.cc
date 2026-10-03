#include "http/connection.h"

#include <cstddef>
#include <string>
#include <string_view>

#include "absl/log/log.h"
#include "absl/status/status.h"
#include "absl/status/statusor.h"
#include "async/task.h"
#include "http/message.h"
#include "http/server.h"
#include "net/tcp.h"

namespace {

// Largest request head accepted. Real browsers send well under this; the
// limit only stops a client from making the server buffer data indefinitely.
constexpr size_t kMaxHeadBytes = size_t{16} * 1024;

}  // namespace

Task<> ServeConnection(TcpConnection connection, const Handler& handler) {
  // Reused for every response on this connection to avoid reallocating.
  std::string head;
  for (;;) {
    const absl::StatusOr<std::string_view> request_head =
        co_await connection.ReadUntil(kHeadTerminator, kMaxHeadBytes);
    // The client left or went silent: there is nobody to answer.
    if (absl::IsUnavailable(request_head.status())) co_return;

    // What to answer when the request cannot be understood. After either
    // error the position of the next request in the stream is unknown, so the
    // connection cannot be kept.
    Response response;
    bool keep_alive = false;
    bool send_body = true;
    if (!request_head.ok()) {
      response = StatusResponse(431);
    } else if (const absl::StatusOr<Request> request =
                   ParseRequest(*request_head);
               !request.ok()) {
      VLOG(1) << "bad request: " << request.status().message();
      response = StatusResponse(400);
    } else {
      response = handler(*request);
      keep_alive = request->keep_alive;
      send_body = request->method != "HEAD";
      VLOG(1) << request->method << " " << request->path << " "
              << response.status;
    }

    head.clear();
    AppendResponseHead(response, keep_alive, &head);
    co_await connection.Write(
        head, send_body ? response.body.view() : std::string_view());
    if (!keep_alive) co_return;
  }
}

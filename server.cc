#include "server.h"

#include <cstdint>
#include <string>
#include <thread>
#include <utility>

#include "absl/log/log.h"
#include "absl/status/status.h"
#include "absl/status/statusor.h"
#include "http.h"
#include "net.h"

namespace {

// Largest request head accepted. Real browsers send well under this; the
// limit only stops a client from making the server buffer data indefinitely.
constexpr size_t kMaxHeadBytes = 16 * 1024;

// Reads one request from `connection` and writes back the response. The
// connection is closed on return.
void AnswerOneRequest(TcpConnection connection, const Handler& handler) {
  absl::StatusOr<std::string> head =
      connection.ReadUntil(kHeadTerminator, kMaxHeadBytes);
  if (absl::IsResourceExhausted(head.status())) {
    connection.Write(SerializeResponse(StatusResponse(431)));
    return;
  }
  // The client left or went silent mid-request: there is nobody to answer.
  if (!head.ok()) return;

  absl::StatusOr<Request> request = ParseRequest(*head);
  if (!request.ok()) {
    LOG(INFO) << "bad request: " << request.status().message();
    connection.Write(SerializeResponse(StatusResponse(400)));
    return;
  }

  Response response = handler(*request);
  LOG(INFO) << request->method << " " << request->path << " "
            << response.status;
  connection.Write(SerializeResponse(response, request->method != "HEAD"));
}

}  // namespace

absl::Status Serve(const std::string& address, uint16_t port,
                   const Handler& handler) {
  absl::StatusOr<TcpListener> listener = TcpListener::Listen(address, port);
  if (!listener.ok()) return listener.status();
  LOG(INFO) << "listening on http://" << address << ":" << listener->port();

  // One thread per connection, so a slow client delays nobody else.
  for (;;) {
    std::thread(AnswerOneRequest, listener->Accept(), handler).detach();
  }
}

#include "http/client.h"

#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>
#include <utility>

#include "absl/status/status.h"
#include "absl/status/statusor.h"
#include "absl/strings/str_cat.h"
#include "async/task.h"
#include "http/message.h"
#include "net/tcp.h"

namespace {

// Largest response head and body accepted. They stop a misbehaving server
// from making the client buffer without bound.
constexpr size_t kMaxHeadBytes = size_t{64} * 1024;
constexpr size_t kMaxBodyBytes = size_t{256} * 1024 * 1024;

}  // namespace

HttpClient::HttpClient(TcpConnection connection, std::string host)
    : connection_(std::move(connection)), host_(std::move(host)) {}

Task<absl::StatusOr<HttpClient>> HttpClient::Connect(std::string address,
                                                     uint16_t port) {
  absl::StatusOr<TcpConnection> connection =
      co_await TcpConnection::Connect(address, port);
  if (!connection.ok()) co_return connection.status();
  co_return HttpClient(std::move(*connection),
                       absl::StrCat(address, ":", port));
}

Task<absl::StatusOr<HttpClient::Result>> HttpClient::Get(
    std::string_view path) {
  request_.clear();
  absl::StrAppend(&request_, "GET ", path, " HTTP/1.1\r\nHost: ", host_,
                  "\r\n\r\n");
  co_await connection_.Write(request_);

  const absl::StatusOr<std::string_view> head_text =
      co_await connection_.ReadUntil(kHeadTerminator, kMaxHeadBytes);
  if (!head_text.ok()) co_return head_text.status();
  // Parsed into values, not views, because reading the body invalidates
  // head_text.
  const absl::StatusOr<ResponseHead> head = ParseResponseHead(*head_text);
  if (!head.ok()) co_return head.status();
  if (head->content_length > kMaxBodyBytes) {
    co_return absl::ResourceExhaustedError(absl::StrCat(
        "response body of ", head->content_length, " bytes is too large"));
  }

  const absl::StatusOr<std::string_view> body =
      co_await connection_.Read(head->content_length);
  if (!body.ok()) co_return body.status();
  co_return Result{
      .status = head->status,
      .body = *body,
  };
}

#include "net/tcp.h"

#include <algorithm>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <span>
#include <string>
#include <string_view>
#include <utility>

#include "absl/log/log.h"
#include "absl/status/status.h"
#include "absl/status/status_macros.h"
#include "absl/status/statusor.h"
#include "absl/strings/str_cat.h"
#include "async/task.h"
#include "os/io.h"
#include "os/socket.h"

namespace {

// How long a read, write or connect may make no progress before it gives up.
constexpr std::chrono::seconds kIdleTimeout(10);

// Initial size of a connection's receive buffer, and so the most that one
// receive can return until the buffer has had to grow.
constexpr size_t kInitialBufferBytes = 4096;

// How long Accept pauses after a failure that will not clear by itself
// immediately, so that it does not spin while the condition lasts.
constexpr std::chrono::milliseconds kAcceptRetryDelay(100);

}  // namespace

TcpConnection::TcpConnection(os::Socket socket) : socket_(std::move(socket)) {
  // Every Write here is already a complete message, so there is nothing to
  // gain from the system delaying it. Failing to set this costs only speed.
  socket_.Enable(os::SocketOption::kNoDelay).IgnoreError();
}

Task<absl::StatusOr<TcpConnection>> TcpConnection::Connect(std::string address,
                                                           uint16_t port) {
  const absl::StatusOr<os::SocketAddress> peer =
      os::SocketAddress::Parse(address, port);
  if (!peer.ok()) co_return peer.status();
  absl::StatusOr<os::Socket> socket = os::Socket::CreateTcp();
  if (!socket.ok()) co_return socket.status();

  const absl::Status connected =
      co_await os::Connect(*socket, *peer, kIdleTimeout);
  if (!connected.ok()) co_return connected;
  co_return TcpConnection(std::move(*socket));
}

Task<absl::StatusOr<std::string_view>> TcpConnection::ReadUntil(
    std::string_view delimiter, size_t max_bytes) {
  for (;;) {
    const std::string_view unread(buffer_.data() + consumed_,
                                  filled_ - consumed_);
    const size_t length = unread.find(delimiter);
    if (length != std::string_view::npos && length <= max_bytes) {
      consumed_ += length + delimiter.size();
      co_return unread.substr(0, length);
    }
    // Checked before reading more so that a peer sending an endless piece
    // cannot make the buffer grow without bound.
    if (unread.size() > max_bytes) {
      co_return absl::ResourceExhaustedError(
          absl::StrCat("no delimiter within ", max_bytes, " bytes"));
    }

    MakeRoom();
    const absl::Status more = NoteReceived(co_await os::Receive(
        socket_, std::span<char>(buffer_).subspan(filled_), kIdleTimeout));
    if (!more.ok()) co_return more;
  }
}

Task<absl::StatusOr<std::string_view>> TcpConnection::Read(size_t bytes) {
  while (filled_ - consumed_ < bytes) {
    MakeRoom();
    const absl::Status more = NoteReceived(co_await os::Receive(
        socket_, std::span<char>(buffer_).subspan(filled_), kIdleTimeout));
    if (!more.ok()) co_return more;
  }
  const std::string_view result(buffer_.data() + consumed_, bytes);
  consumed_ += bytes;
  co_return result;
}

void TcpConnection::MakeRoom() {
  if (consumed_ > 0) {
    std::copy(buffer_.data() + consumed_, buffer_.data() + filled_,
              buffer_.data());
    filled_ -= consumed_;
    consumed_ = 0;
  }
  if (filled_ == buffer_.size()) {
    buffer_.resize(std::max(kInitialBufferBytes, 2 * buffer_.size()));
  }
}

absl::Status TcpConnection::NoteReceived(
    const absl::StatusOr<size_t>& received) {
  // Whatever went wrong, to the caller it means the peer is not there.
  if (!received.ok()) {
    return absl::UnavailableError(received.status().message());
  }
  if (*received == 0) {
    return absl::UnavailableError("peer closed the connection");
  }
  filled_ += *received;
  return absl::OkStatus();
}

Task<> TcpConnection::Write(std::string_view first, std::string_view second) {
  while (!first.empty() || !second.empty()) {
    const absl::StatusOr<size_t> sent =
        co_await os::Send(socket_, first, second, kIdleTimeout);
    if (!sent.ok() || *sent == 0) co_return;

    // Normally everything was sent. If not, go round again with the rest.
    const size_t sent_of_first = std::min(*sent, first.size());
    first.remove_prefix(sent_of_first);
    second.remove_prefix(*sent - sent_of_first);
  }
}

TcpListener::TcpListener(os::Socket socket, uint16_t port)
    : socket_(std::move(socket)), port_(port) {}

absl::StatusOr<TcpListener> TcpListener::Listen(const std::string& address,
                                                uint16_t port) {
  ABSL_ASSIGN_OR_RETURN(const os::SocketAddress local,
                        os::SocketAddress::Parse(address, port));
  ABSL_ASSIGN_OR_RETURN(os::Socket socket, os::Socket::CreateTcp());
  ABSL_RETURN_IF_ERROR(socket.Enable(os::SocketOption::kReuseAddress));
  ABSL_RETURN_IF_ERROR(socket.Bind(local));
  ABSL_RETURN_IF_ERROR(socket.Listen());
  // Ask which port was assigned, in case the caller passed 0.
  ABSL_ASSIGN_OR_RETURN(const os::SocketAddress bound, socket.LocalAddress());
  return TcpListener(std::move(socket), bound.port());
}

Task<TcpConnection> TcpListener::Accept() const {
  for (;;) {
    absl::StatusOr<os::Socket> socket = co_await os::Accept(socket_);
    if (socket.ok()) co_return TcpConnection(std::move(*socket));
    // Usually the process or machine is out of sockets; existing connections
    // finishing will fix that.
    LOG(WARNING) << socket.status().message();
    co_await os::Sleep(kAcceptRetryDelay);
  }
}

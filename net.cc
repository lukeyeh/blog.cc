#include "net.h"

#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <unistd.h>

#include <cerrno>
#include <chrono>
#include <cstdint>
#include <cstring>
#include <string>
#include <string_view>
#include <thread>
#include <utility>

#include "absl/log/log.h"
#include "absl/status/status.h"
#include "absl/status/statusor.h"
#include "absl/strings/str_cat.h"

namespace {

using net_internal::Socket;

// How long a read or write may make no progress before it gives up.
constexpr int kIdleTimeoutSeconds = 10;

// How long Accept pauses after a failure that will not clear by itself
// immediately, so that it does not spin while the condition lasts.
constexpr auto kAcceptRetryDelay = std::chrono::milliseconds(100);

// Describes the most recent failed system call. Must be called before
// anything else can overwrite errno.
absl::Status LastError(std::string_view what) {
  return absl::UnavailableError(absl::StrCat(what, ": ", strerror(errno)));
}

// Converts an address and port to the form the socket API wants.
absl::StatusOr<sockaddr_in> ToSocketAddress(const std::string& address,
                                            uint16_t port) {
  sockaddr_in result{};
  result.sin_family = AF_INET;
  result.sin_port = htons(port);
  if (inet_pton(AF_INET, address.c_str(), &result.sin_addr) != 1) {
    return absl::InvalidArgumentError(
        absl::StrCat("not an IPv4 address: ", address));
  }
  return result;
}

absl::StatusOr<Socket> NewTcpSocket() {
  int descriptor = socket(AF_INET, SOCK_STREAM, 0);
  if (descriptor < 0) return LastError("cannot create socket");
  return Socket(descriptor);
}

}  // namespace

namespace net_internal {

Socket& Socket::operator=(Socket&& other) noexcept {
  std::swap(descriptor_, other.descriptor_);
  return *this;
}

Socket::~Socket() {
  if (descriptor_ >= 0) close(descriptor_);
}

}  // namespace net_internal

TcpConnection::TcpConnection(Socket socket) : socket_(std::move(socket)) {
  timeval timeout{.tv_sec = kIdleTimeoutSeconds, .tv_usec = 0};
  setsockopt(socket_.descriptor(), SOL_SOCKET, SO_RCVTIMEO, &timeout,
             sizeof(timeout));
  setsockopt(socket_.descriptor(), SOL_SOCKET, SO_SNDTIMEO, &timeout,
             sizeof(timeout));
}

absl::StatusOr<TcpConnection> TcpConnection::Connect(
    const std::string& address, uint16_t port) {
  absl::StatusOr<sockaddr_in> peer = ToSocketAddress(address, port);
  if (!peer.ok()) return peer.status();
  absl::StatusOr<Socket> socket = NewTcpSocket();
  if (!socket.ok()) return socket.status();

  if (connect(socket->descriptor(), reinterpret_cast<sockaddr*>(&*peer),
              sizeof(*peer)) < 0) {
    return LastError(absl::StrCat("cannot connect to ", address, ":", port));
  }
  return TcpConnection(std::move(*socket));
}

absl::StatusOr<std::string> TcpConnection::ReadUntil(
    std::string_view delimiter, size_t max_bytes) {
  for (;;) {
    size_t length = unread_.find(delimiter);
    if (length != std::string::npos && length <= max_bytes) {
      std::string piece = unread_.substr(0, length);
      unread_.erase(0, length + delimiter.size());
      return piece;
    }
    // Checked before reading more so that a peer sending an endless piece
    // cannot make the buffer grow without bound.
    if (unread_.size() > max_bytes) {
      return absl::ResourceExhaustedError(
          absl::StrCat("no delimiter within ", max_bytes, " bytes"));
    }

    char chunk[4096];
    ssize_t received = recv(socket_.descriptor(), chunk, sizeof(chunk), 0);
    if (received > 0) {
      unread_.append(chunk, received);
    } else if (received == 0) {
      return absl::UnavailableError("peer closed the connection");
    } else if (errno != EINTR) {
      // Includes the idle timeout, which the OS reports as an error.
      return LastError("cannot read from peer");
    }
  }
}

void TcpConnection::Write(std::string_view data) {
  while (!data.empty()) {
    // MSG_NOSIGNAL: by default, writing to a peer that has hung up kills the
    // whole process with SIGPIPE.
    ssize_t sent =
        send(socket_.descriptor(), data.data(), data.size(), MSG_NOSIGNAL);
    if (sent > 0) {
      data.remove_prefix(sent);
    } else if (errno != EINTR) {
      return;
    }
  }
}

TcpListener::TcpListener(Socket socket, uint16_t port)
    : socket_(std::move(socket)), port_(port) {}

absl::StatusOr<TcpListener> TcpListener::Listen(const std::string& address,
                                                uint16_t port) {
  absl::StatusOr<sockaddr_in> local = ToSocketAddress(address, port);
  if (!local.ok()) return local.status();
  absl::StatusOr<Socket> socket = NewTcpSocket();
  if (!socket.ok()) return socket.status();

  // Without this, restarting the program fails for a minute or so while the
  // OS keeps the port reserved for the previous run's lingering connections.
  int enable = 1;
  setsockopt(socket->descriptor(), SOL_SOCKET, SO_REUSEADDR, &enable,
             sizeof(enable));

  if (bind(socket->descriptor(), reinterpret_cast<sockaddr*>(&*local),
           sizeof(*local)) < 0 ||
      listen(socket->descriptor(), SOMAXCONN) < 0) {
    return LastError(absl::StrCat("cannot listen on ", address, ":", port));
  }

  // Ask which port was assigned, in case the caller passed 0.
  socklen_t size = sizeof(*local);
  if (getsockname(socket->descriptor(), reinterpret_cast<sockaddr*>(&*local),
                  &size) < 0) {
    return LastError("cannot determine listening port");
  }
  return TcpListener(std::move(*socket), ntohs(local->sin_port));
}

TcpConnection TcpListener::Accept() {
  for (;;) {
    int descriptor = accept(socket_.descriptor(), nullptr, nullptr);
    if (descriptor >= 0) return TcpConnection(Socket(descriptor));
    if (errno == EINTR) continue;
    // Usually the process or machine is out of sockets; existing connections
    // finishing will fix that.
    LOG(WARNING) << "cannot accept connection: " << strerror(errno);
    std::this_thread::sleep_for(kAcceptRetryDelay);
  }
}

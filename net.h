// TCP networking for programs that want to exchange bytes with a peer without
// dealing with the operating system. This is the only module that talks to
// the socket API: file descriptors, address structures, errno, interrupted
// calls, partial reads and writes, and signals are all handled here and never
// appear in the interface.

#ifndef NET_H_
#define NET_H_

#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>
#include <utility>

#include "absl/status/statusor.h"

namespace net_internal {

// Owns one operating system socket and releases it on destruction. Exists so
// the classes below get correct move and cleanup behaviour for free.
class Socket {
 public:
  explicit Socket(int descriptor) : descriptor_(descriptor) {}
  Socket(Socket&& other) noexcept
      : descriptor_(std::exchange(other.descriptor_, -1)) {}
  Socket& operator=(Socket&& other) noexcept;
  ~Socket();

  int descriptor() const { return descriptor_; }

 private:
  // -1 once ownership has been moved elsewhere.
  int descriptor_;
};

}  // namespace net_internal

// One end of an established TCP connection: an ordered stream of bytes in
// each direction. The connection is closed when the object is destroyed.
//
// A peer that stops responding cannot block the caller forever: any read or
// write that makes no progress for several seconds gives up.
//
// Not thread-safe; use each connection from one thread at a time.
class TcpConnection {
 public:
  // Opens a connection to a listening peer. `address` is a dotted IPv4
  // address such as "127.0.0.1"; host names are not resolved.
  static absl::StatusOr<TcpConnection> Connect(const std::string& address,
                                               uint16_t port);

  TcpConnection(TcpConnection&&) = default;
  TcpConnection& operator=(TcpConnection&&) = default;

  // Returns the incoming bytes up to the next occurrence of `delimiter`,
  // waiting for more to arrive if necessary. The delimiter is consumed but
  // not returned, and anything the peer sent after it is kept for the next
  // call, so consecutive calls split the stream into delimited pieces.
  //
  // Fails with:
  //   ResourceExhausted  the piece would be longer than `max_bytes`. The
  //                      peer is still connected and can be written to.
  //   Unavailable        the peer disconnected or went silent before the
  //                      delimiter arrived.
  absl::StatusOr<std::string> ReadUntil(std::string_view delimiter,
                                        size_t max_bytes);

  // Sends all of `data` to the peer. There is no error to handle: if the
  // peer has gone away the data is discarded, because the only thing a
  // caller could do about it is stop writing, and destroying the connection
  // does that anyway.
  void Write(std::string_view data);

 private:
  friend class TcpListener;

  // Takes ownership of an already-connected socket.
  explicit TcpConnection(net_internal::Socket socket);

  net_internal::Socket socket_;

  // Bytes received from the peer that no ReadUntil call has returned yet.
  std::string unread_;
};

// Waits for peers to connect to a local address. Listening stops when the
// object is destroyed.
class TcpListener {
 public:
  // Starts listening. `address` is a dotted IPv4 address: "127.0.0.1"
  // accepts connections from this machine only, "0.0.0.0" from anywhere.
  // Port 0 asks for any free port; port() reports which one was chosen.
  //
  // Fails if the address is invalid or the port cannot be claimed, typically
  // because another program is using it or it requires privileges.
  static absl::StatusOr<TcpListener> Listen(const std::string& address,
                                            uint16_t port);

  TcpListener(TcpListener&&) = default;
  TcpListener& operator=(TcpListener&&) = default;

  // Blocks until a peer connects and returns the connection to it. Never
  // fails: temporary problems, such as the process running out of sockets,
  // are waited out internally.
  TcpConnection Accept();

  // The port actually being listened on.
  uint16_t port() const { return port_; }

 private:
  TcpListener(net_internal::Socket socket, uint16_t port);

  net_internal::Socket socket_;
  uint16_t port_;
};

#endif  // NET_H_

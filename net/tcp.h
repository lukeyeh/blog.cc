// TCP networking for programs that want to exchange bytes with a peer:
// connections as delimited streams of text, with buffering, partial reads and
// writes, idle peers and retries handled here and absent from the interface.
//
// Operations that may wait for the peer are asynchronous (see task.h) and
// must be called from a task running on an EventLoop.

#ifndef NET_TCP_H_
#define NET_TCP_H_

#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>

#include "absl/status/status.h"
#include "absl/status/statusor.h"
#include "async/task.h"
#include "os/socket.h"

// One end of an established TCP connection: an ordered stream of bytes in
// each direction. The connection is closed when the object is destroyed.
//
// A peer that stops responding cannot hold a connection open forever: any
// read or write that makes no progress for several seconds gives up.
//
// A connection belongs to the event loop of the thread that obtained it, and
// must not be moved or destroyed while one of its operations is in progress.
class TcpConnection {
 public:
  // Opens a connection to a listening peer. `address` is a dotted IPv4
  // address such as "127.0.0.1"; host names are not resolved.
  static Task<absl::StatusOr<TcpConnection>> Connect(std::string address,
                                                     uint16_t port);

  TcpConnection(TcpConnection&&) = default;
  TcpConnection& operator=(TcpConnection&&) = default;

  // Returns the incoming bytes up to the next occurrence of `delimiter`,
  // waiting for more to arrive if necessary. The delimiter is consumed but
  // not returned, and anything the peer sent after it is kept for the next
  // call, so consecutive calls split the stream into delimited pieces.
  //
  // The returned text is not a copy: it refers to memory inside the
  // connection and is valid only until the next call to ReadUntil.
  //
  // Fails with:
  //   ResourceExhausted  the piece would be longer than `max_bytes`. The
  //                      peer is still connected and can be written to.
  //   Unavailable        the peer disconnected or went silent before the
  //                      delimiter arrived.
  Task<absl::StatusOr<std::string_view>> ReadUntil(std::string_view delimiter,
                                                   size_t max_bytes);

  // Returns exactly the next `bytes` incoming bytes, waiting for them to
  // arrive if necessary. For content whose length is known in advance, where
  // ReadUntil is for content that ends with a marker. The two can be mixed
  // freely on one connection.
  //
  // Like ReadUntil, the returned text is valid only until the next read, and
  // the call fails with Unavailable if the peer disconnects or goes silent
  // first.
  Task<absl::StatusOr<std::string_view>> Read(size_t bytes);

  // Sends `first` followed by `second` to the peer, without copying either.
  // Taking two pieces lets a caller send a header it has just built followed
  // by a body that lives elsewhere.
  //
  // There is no error to handle: if the peer has gone away the data is
  // discarded, because the only thing a caller could do about it is stop
  // writing, and destroying the connection does that anyway.
  Task<> Write(std::string_view first, std::string_view second = {});

 private:
  friend class TcpListener;

  // Takes ownership of an already-connected socket.
  explicit TcpConnection(os::Socket socket);

  // Arranges for there to be free space after filled_, by reusing the space
  // of bytes already returned or, failing that, growing the buffer.
  void MakeRoom();

  // Accounts for the outcome of receiving into the free space. Returns OK if
  // more bytes are now available, and otherwise why there will be no more.
  absl::Status NoteReceived(const absl::StatusOr<size_t>& received);

  os::Socket socket_;

  // Storage for incoming bytes. Bytes [consumed_, filled_) have been received
  // but not yet returned by ReadUntil; bytes from filled_ on are free space.
  std::string buffer_;
  size_t consumed_ = 0;
  size_t filled_ = 0;
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

  // Waits until a peer connects and returns the connection to it. Never
  // fails: temporary problems, such as the process running out of sockets,
  // are waited out internally.
  //
  // Several threads may call Accept on the same listener at once, each from
  // its own event loop. Each peer is handed to exactly one of them, which is
  // how a server spreads connections over cores.
  Task<TcpConnection> Accept() const;

  // The port actually being listened on.
  uint16_t port() const { return port_; }

 private:
  TcpListener(os::Socket socket, uint16_t port);

  os::Socket socket_;
  uint16_t port_;
};

#endif  // NET_TCP_H_

#include "net.h"

#include <string>
#include <utility>

#include "absl/status/status.h"
#include "gtest/gtest.h"

// A connected pair on the loopback interface.
struct Pair {
  TcpConnection client;
  TcpConnection server;
};

Pair Connect() {
  absl::StatusOr<TcpListener> listener = TcpListener::Listen("127.0.0.1", 0);
  EXPECT_TRUE(listener.ok()) << listener.status();
  absl::StatusOr<TcpConnection> client =
      TcpConnection::Connect("127.0.0.1", listener->port());
  EXPECT_TRUE(client.ok()) << client.status();
  return Pair{std::move(*client), listener->Accept()};
}

TEST(NetTest, ReadUntilSplitsStreamIntoPieces) {
  Pair pair = Connect();
  pair.client.Write("hello\nworld\n");
  EXPECT_EQ(*pair.server.ReadUntil("\n", 100), "hello");
  EXPECT_EQ(*pair.server.ReadUntil("\n", 100), "world");
}

TEST(NetTest, BytesFlowInBothDirections) {
  Pair pair = Connect();
  pair.server.Write("pong;");
  EXPECT_EQ(*pair.client.ReadUntil(";", 100), "pong");
}

TEST(NetTest, ReadUntilReportsOversizedPiece) {
  Pair pair = Connect();
  pair.client.Write("0123456789\n");
  EXPECT_TRUE(absl::IsResourceExhausted(
      pair.server.ReadUntil("\n", 4).status()));
}

TEST(NetTest, ReadUntilReportsDisconnectedPeer) {
  absl::StatusOr<TcpListener> listener = TcpListener::Listen("127.0.0.1", 0);
  ASSERT_TRUE(listener.ok());
  TcpConnection server = [&] {
    auto client = TcpConnection::Connect("127.0.0.1", listener->port());
    EXPECT_TRUE(client.ok());
    client->Write("unfinished");
    return listener->Accept();
  }();  // The client is destroyed here, closing its end.
  EXPECT_TRUE(absl::IsUnavailable(server.ReadUntil("\n", 100).status()));
}

TEST(NetTest, WriteToDisconnectedPeerIsHarmless) {
  Pair pair = Connect();
  { TcpConnection gone = std::move(pair.client); }
  pair.server.Write(std::string(1 << 20, 'x'));
  pair.server.Write("more");
}

TEST(NetTest, ListenFailsForBadAddressOrBusyPort) {
  EXPECT_FALSE(TcpListener::Listen("not-an-address", 0).ok());
  absl::StatusOr<TcpListener> first = TcpListener::Listen("127.0.0.1", 0);
  ASSERT_TRUE(first.ok());
  EXPECT_FALSE(TcpListener::Listen("127.0.0.1", first->port()).ok());
}

TEST(NetTest, ConnectFailsWhenNobodyListens) {
  uint16_t free_port = TcpListener::Listen("127.0.0.1", 0)->port();
  EXPECT_FALSE(TcpConnection::Connect("127.0.0.1", free_port).ok());
}

// TcpListener and TcpConnection by example: how two programs exchange bytes.
// Every test plays both ends of a connection over the loopback interface.

#include "net/tcp.h"

#include <benchmark/benchmark.h>

#include <cstddef>
#include <cstdint>
#include <string>
#include <utility>

#include "absl/status/status.h"
#include "absl/status/status_matchers.h"
#include "async/task.h"
#include "gmock/gmock.h"
#include "gtest/gtest.h"
#include "net/event_loop.h"

using absl_testing::IsOkAndHolds;
using absl_testing::StatusIs;

// Runs an asynchronous test body to completion on a fresh event loop. Test
// bodies use EXPECT rather than ASSERT, which cannot be used in a coroutine.
void RunOnEventLoop(Task<> body) {
  absl::StatusOr<EventLoop> loop = EventLoop::Create();
  ABSL_ASSERT_OK(loop);
  loop->Run(std::move(body));
}

// Port 0 asks the system for any free port, which keeps tests from clashing.
TcpListener ListenOnFreePort() {
  absl::StatusOr<TcpListener> listener = TcpListener::Listen("127.0.0.1", 0);
  ABSL_EXPECT_OK(listener);
  return std::move(*listener);
}

// A connected pair on the loopback interface.
struct Pair {
  TcpConnection client;
  TcpConnection server;
};

// The whole life of a connection's setup: one side listens, the other
// connects, and the listener accepts.
Task<Pair> Connect() {
  const TcpListener listener = ListenOnFreePort();
  absl::StatusOr<TcpConnection> client =
      co_await TcpConnection::Connect("127.0.0.1", listener.port());
  ABSL_EXPECT_OK(client);
  co_return Pair{
      std::move(*client),
      co_await listener.Accept(),
  };
}

// TCP delivers a stream with no message boundaries. ReadUntil imposes them:
// here one write carrying two lines, split across its two arguments, is read
// back as two lines.
TEST(TcpTest, ReadUntilSplitsStreamIntoPieces) {
  RunOnEventLoop([]() -> Task<> {
    Pair pair = co_await Connect();
    co_await pair.client.Write("hello\nwor", "ld\n");
    EXPECT_THAT(co_await pair.server.ReadUntil("\n", 100),
                IsOkAndHolds("hello"));
    EXPECT_THAT(co_await pair.server.ReadUntil("\n", 100),
                IsOkAndHolds("world"));
  }());
}

// "Client" and "server" only describe who connected to whom; either end can
// send.
TEST(TcpTest, BytesFlowInBothDirections) {
  RunOnEventLoop([]() -> Task<> {
    Pair pair = co_await Connect();
    co_await pair.server.Write("pong;");
    EXPECT_THAT(co_await pair.client.ReadUntil(";", 100), IsOkAndHolds("pong"));
  }());
}

// A piece may be far bigger than one network packet or the connection's
// initial buffer; ReadUntil keeps receiving until it has all of it.
TEST(TcpTest, ReadUntilHandlesLargePieces) {
  RunOnEventLoop([]() -> Task<> {
    Pair pair = co_await Connect();
    const std::string big(100'000, 'x');
    // Spawned, because a write this large cannot finish until the other end
    // starts reading.
    Spawn([](TcpConnection& client, const std::string& big) -> Task<> {
      co_await client.Write(big, ";tail;");
    }(pair.client, big));
    EXPECT_THAT(co_await pair.server.ReadUntil(";", 200'000),
                IsOkAndHolds(big));
    EXPECT_THAT(co_await pair.server.ReadUntil(";", 100), IsOkAndHolds("tail"));
  }());
}

// Read takes a fixed number of bytes instead of looking for a delimiter. This
// is how a message with a length in its header is read: the header with
// ReadUntil, the content with Read.
TEST(TcpTest, ReadTakesExactlyTheBytesAskedFor) {
  RunOnEventLoop([]() -> Task<> {
    Pair pair = co_await Connect();
    co_await pair.client.Write("5\nhelloREST\n");
    EXPECT_THAT(co_await pair.server.ReadUntil("\n", 100), IsOkAndHolds("5"));
    EXPECT_THAT(co_await pair.server.Read(5), IsOkAndHolds("hello"));
    EXPECT_THAT(co_await pair.server.ReadUntil("\n", 100),
                IsOkAndHolds("REST"));
  }());
}

// Read waits until all the bytes have arrived, however many pieces the
// network delivers them in.
TEST(TcpTest, ReadWaitsForAllBytes) {
  RunOnEventLoop([]() -> Task<> {
    Pair pair = co_await Connect();
    const std::string big(100'000, 'x');
    Spawn([](TcpConnection& client, const std::string& big) -> Task<> {
      co_await client.Write(big);
    }(pair.client, big));
    EXPECT_THAT(co_await pair.server.Read(big.size()), IsOkAndHolds(big));
  }());
}

TEST(TcpTest, ReadReportsDisconnectedPeer) {
  RunOnEventLoop([]() -> Task<> {
    Pair pair = co_await Connect();
    co_await pair.client.Write("only 12 here");
    {
      const TcpConnection closed = std::move(pair.client);
    }
    EXPECT_THAT(co_await pair.server.Read(100),
                StatusIs(absl::StatusCode::kUnavailable));
  }());
}

// `max_bytes` protects the reader's memory from a peer that never sends the
// delimiter.
TEST(TcpTest, ReadUntilReportsOversizedPiece) {
  RunOnEventLoop([]() -> Task<> {
    Pair pair = co_await Connect();
    co_await pair.client.Write("0123456789\n");
    EXPECT_THAT(co_await pair.server.ReadUntil("\n", 4),
                StatusIs(absl::StatusCode::kResourceExhausted));
  }());
}

// A peer that leaves mid-piece is an error, not a short piece.
TEST(TcpTest, ReadUntilReportsDisconnectedPeer) {
  RunOnEventLoop([]() -> Task<> {
    Pair pair = co_await Connect();
    co_await pair.client.Write("unfinished");
    // Destroying a connection closes it.
    {
      const TcpConnection closed = std::move(pair.client);
    }
    EXPECT_THAT(co_await pair.server.ReadUntil("\n", 100),
                StatusIs(absl::StatusCode::kUnavailable));
  }());
}

// Write has no error to check: writing to a peer that has gone is silently
// dropped, and in particular does not kill the process.
TEST(TcpTest, WriteToDisconnectedPeerIsHarmless) {
  RunOnEventLoop([]() -> Task<> {
    Pair pair = co_await Connect();
    {
      const TcpConnection closed = std::move(pair.client);
    }
    const std::string big(1 << 20, 'x');
    co_await pair.server.Write(big);
    co_await pair.server.Write("more");
  }());
}

// Listening is the one step that commonly fails, and it says why.
TEST(TcpTest, ListenFailsForBadAddressOrBusyPort) {
  EXPECT_THAT(TcpListener::Listen("not-an-address", 0),
              StatusIs(absl::StatusCode::kInvalidArgument));
  const TcpListener first = ListenOnFreePort();
  EXPECT_THAT(TcpListener::Listen("127.0.0.1", first.port()),
              StatusIs(absl::StatusCode::kFailedPrecondition));
}

TEST(TcpTest, ConnectFailsWhenNobodyListens) {
  // The listener is gone by the end of this line, leaving a port that was
  // free a moment ago.
  const uint16_t free_port = ListenOnFreePort().port();
  RunOnEventLoop([](uint16_t port) -> Task<> {
    EXPECT_THAT(co_await TcpConnection::Connect("127.0.0.1", port),
                StatusIs(absl::StatusCode::kUnavailable));
  }(free_port));
}

// -----------------------------------------------------------------------------
// Benchmarks
// -----------------------------------------------------------------------------
//
// What TCP connections cost: opening one, and moving text over one.
//
//   bazel run -c opt //net:tcp_test -- --benchmark_filter=all
//
// ConnectAndAccept against WriteAndReadUntil shows what keep-alive saves: a
// server that takes one request per connection pays the first for every
// request, on top of the second.

namespace {

constexpr size_t kNoLimit = size_t{1} << 30;

// Connects to `listener` rather than to a listener of its own, so that a
// benchmark can open many connections to the same one.
Task<Pair> ConnectTo(const TcpListener& listener) {
  TcpConnection client =
      *co_await TcpConnection::Connect("127.0.0.1", listener.port());
  co_return Pair{
      std::move(client),
      co_await listener.Accept(),
  };
}

Task<> ConnectInLoop(benchmark::State& state) {
  const TcpListener listener = *TcpListener::Listen("127.0.0.1", 0);
  // Both ends are closed when `pair` goes out of scope.
  for (auto _ : state) const Pair pair = co_await ConnectTo(listener);
}

// Opening a connection, accepting it, and closing both ends.
void BM_ConnectAndAccept(benchmark::State& state) {
  EventLoop::Create()->Run(ConnectInLoop(state));
}
BENCHMARK(BM_ConnectAndAccept);

Task<> WriteAndReadUntilInLoop(benchmark::State& state) {
  const TcpListener listener = *TcpListener::Listen("127.0.0.1", 0);
  Pair pair = co_await ConnectTo(listener);
  const std::string line(static_cast<size_t>(state.range(0)), 'x');
  for (auto _ : state) {
    co_await pair.client.Write(line, "\n");
    benchmark::DoNotOptimize(co_await pair.server.ReadUntil("\n", kNoLimit));
  }
  state.SetBytesProcessed(state.iterations() * state.range(0));
}

// One line sent and received on an open connection, for several line
// lengths. The shape of one request or one response.
void BM_WriteAndReadUntil(benchmark::State& state) {
  EventLoop::Create()->Run(WriteAndReadUntilInLoop(state));
}
BENCHMARK(BM_WriteAndReadUntil)->Arg(64)->Arg(4096)->Arg(65536);

Task<> WriteAndReadInLoop(benchmark::State& state) {
  const TcpListener listener = *TcpListener::Listen("127.0.0.1", 0);
  Pair pair = co_await ConnectTo(listener);
  const size_t size = static_cast<size_t>(state.range(0));
  const std::string content(size, 'x');
  for (auto _ : state) {
    co_await pair.client.Write(content);
    benchmark::DoNotOptimize(co_await pair.server.Read(size));
  }
  state.SetBytesProcessed(state.iterations() * state.range(0));
}

// The same with Read, which knows the length and so does not have to search
// for a delimiter. The shape of a response body.
void BM_WriteAndRead(benchmark::State& state) {
  EventLoop::Create()->Run(WriteAndReadInLoop(state));
}
BENCHMARK(BM_WriteAndRead)->Arg(64)->Arg(4096)->Arg(65536);

Task<> ReadBufferedLinesInLoop(benchmark::State& state) {
  constexpr int kLines = 100;
  const TcpListener listener = *TcpListener::Listen("127.0.0.1", 0);
  Pair pair = co_await ConnectTo(listener);
  std::string lines;
  for (int i = 0; i < kLines; ++i) lines += "a line of ordinary length\n";
  for (auto _ : state) {
    co_await pair.client.Write(lines);
    for (int i = 0; i < kLines; ++i) {
      benchmark::DoNotOptimize(co_await pair.server.ReadUntil("\n", kNoLimit));
    }
  }
  state.SetItemsProcessed(state.iterations() * kLines);
}

// Many lines arriving together. Only the first ReadUntil of each batch waits
// for the network; the rest are served from the connection's buffer. The
// per-item time is roughly what a pipelined request costs to read.
void BM_ReadUntilWithLinesAlreadyBuffered(benchmark::State& state) {
  EventLoop::Create()->Run(ReadBufferedLinesInLoop(state));
}
BENCHMARK(BM_ReadUntilWithLinesAlreadyBuffered);

}  // namespace

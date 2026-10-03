// EventLoop and Spawn by example: how tasks get run, and how several of them
// share one thread.

#include "net/event_loop.h"

#include <benchmark/benchmark.h>

#include <chrono>
#include <string>
#include <thread>
#include <utility>
#include <vector>

#include "absl/status/status_matchers.h"
#include "absl/status/statusor.h"
#include "async/task.h"
#include "gmock/gmock.h"
#include "gtest/gtest.h"
#include "net/tcp.h"

using testing::ElementsAre;

EventLoop NewLoop() {
  absl::StatusOr<EventLoop> loop = EventLoop::Create();
  ABSL_EXPECT_OK(loop);
  return std::move(*loop);
}

Task<> SetFlag(bool& flag) {
  flag = true;
  co_return;
}

// Two ends of one connection, to give tasks some real I/O to wait for.
struct Pair {
  TcpConnection client;
  TcpConnection server;
};

Task<Pair> Connect() {
  const absl::StatusOr<TcpListener> listener =
      TcpListener::Listen("127.0.0.1", 0);
  ABSL_EXPECT_OK(listener);
  absl::StatusOr<TcpConnection> client =
      co_await TcpConnection::Connect("127.0.0.1", listener->port());
  ABSL_EXPECT_OK(client);
  co_return Pair{
      std::move(*client),
      co_await listener->Accept(),
  };
}

// Sends a line each way and checks that it arrives.
Task<> ExchangeLines() {
  Pair pair = co_await Connect();
  co_await pair.client.Write("ping\n");
  EXPECT_EQ(*co_await pair.server.ReadUntil("\n", 100), "ping");
  co_await pair.server.Write("pong\n");
  EXPECT_EQ(*co_await pair.client.ReadUntil("\n", 100), "pong");
}

Task<int> Add(int a, int b) { co_return a + b; }

// A loop says how it does its I/O, which depends on the --io_backend flag and
// on what the system allows.
TEST(EventLoopTest, ReportsItsIoBackend) {
  EXPECT_THAT(NewLoop().io_backend(), testing::AnyOf("io_uring", "epoll"));
}

// Run blocks the calling thread until the task it was given has finished.
TEST(EventLoopTest, RunReturnsWhenMainFinishes) {
  bool ran = false;
  NewLoop().Run(SetFlag(ran));
  EXPECT_TRUE(ran);
}

// Run is the bridge from ordinary code to asynchronous code: it hands back
// whatever the task produced.
TEST(EventLoopTest, RunReturnsTheTasksResult) {
  EXPECT_EQ(NewLoop().Run(Add(2, 3)), 5);
}

// Spawn does not wait for the event loop to get round to the task: it runs
// straight away, up to the first point where it has to wait.
TEST(EventLoopTest, SpawnStartsTaskImmediately) {
  NewLoop().Run([]() -> Task<> {
    bool ran = false;
    Spawn(SetFlag(ran));
    EXPECT_TRUE(ran);
    co_return;
  }());
}

// The point of an event loop: while one task waits for I/O, the thread runs
// another.
TEST(EventLoopTest, TasksTakeTurnsWhileWaiting) {
  NewLoop().Run([]() -> Task<> {
    Pair pair = co_await Connect();
    std::vector<std::string> events;

    Spawn(
        [](TcpConnection& server, std::vector<std::string>& events) -> Task<> {
          events.push_back("reader waits");
          const auto line = co_await server.ReadUntil("\n", 100);
          events.push_back("reader got line");
          co_await server.Write("ack\n");
        }(pair.server, events));

    // The reader is now suspended, and control is back here.
    events.push_back("main continues");
    co_await pair.client.Write("line\n");
    // Waiting for the reply gives the reader its turn.
    const auto ack = co_await pair.client.ReadUntil("\n", 100);

    EXPECT_THAT(events, ElementsAre("reader waits", "main continues",
                                    "reader got line"));
  }());
}

// Sleep pauses one task, not the thread: the other task gets to finish
// first even though it was started second.
TEST(EventLoopTest, SleepLetsOtherTasksRun) {
  NewLoop().Run([]() -> Task<> {
    std::vector<std::string> events;
    Spawn([](std::vector<std::string>& events) -> Task<> {
      co_await Sleep(std::chrono::milliseconds(5));
      events.push_back("sleeper woke");
    }(events));
    events.push_back("main continues");
    co_await Sleep(std::chrono::milliseconds(20));

    EXPECT_THAT(events, ElementsAre("main continues", "sleeper woke"));
  }());
}

// Run does not wait for spawned tasks. Those still waiting when main
// finishes are dropped, never resumed.
TEST(EventLoopTest, RunAbandonsTasksStillWaitingWhenMainFinishes) {
  NewLoop().Run([]() -> Task<> {
    Pair pair = co_await Connect();
    Spawn([](TcpConnection server) -> Task<> {
      // Waits for a newline that never comes.
      const auto never = co_await server.ReadUntil("\n", 100);
      ADD_FAILURE() << "abandoned task was resumed";
    }(std::move(pair.server)));
    co_await pair.client.Write("no newline");
  }());
}

// A server creates its loops up front, where failure is easy to report, and
// then hands each to its own thread.
TEST(EventLoopTest, RunsOnADifferentThreadThanItWasCreatedOn) {
  EventLoop loop = NewLoop();
  std::jthread([&loop] { loop.Run(ExchangeLines()); });
}

// Every thread has its own loop; they do not interfere.
TEST(EventLoopTest, SeveralThreadsEachRunTheirOwnLoop) {
  std::vector<std::jthread> threads;
  threads.reserve(4);
  for (int i = 0; i < 4; ++i) {
    threads.emplace_back([] { NewLoop().Run(ExchangeLines()); });
  }
}

// -----------------------------------------------------------------------------
// Benchmarks
// -----------------------------------------------------------------------------
//
// What the event loop costs: setting one up, and each turn of it.
//
//   bazel run -c opt //net:event_loop_test -- --benchmark_filter=all

namespace {

Task<> Nothing() { co_return; }

// Creating a loop and running a task that finishes at once. Dominated by
// setting up and tearing down the loop's io_uring, which is why the server
// creates its loops once, at startup.
void BM_CreateAndRun(benchmark::State& state) {
  for (auto _ : state) EventLoop::Create()->Run(Nothing());
}
BENCHMARK(BM_CreateAndRun);

Task<> SleepInLoop(benchmark::State& state) {
  for (auto _ : state) co_await Sleep(std::chrono::nanoseconds(0));
}

// One turn of the loop: a task waits for something that is already due, the
// loop goes to the kernel, comes back, and wakes it. The least any wait for
// I/O can cost.
void BM_OneTurnOfTheLoop(benchmark::State& state) {
  EventLoop::Create()->Run(SleepInLoop(state));
}
BENCHMARK(BM_OneTurnOfTheLoop);

Task<> SpawnInLoop(benchmark::State& state) {
  for (auto _ : state) Spawn(Nothing());
  co_return;
}

// Starting an independent task on a running loop, as the server does for
// each connection.
void BM_Spawn(benchmark::State& state) {
  EventLoop::Create()->Run(SpawnInLoop(state));
}
BENCHMARK(BM_Spawn);

}  // namespace

// Task<T> by example: how asynchronous functions are written and combined.
// None of these wait for anything, so a TaskScope is enough to run them; no
// event loop is involved.

#include "async/task.h"

#include <benchmark/benchmark.h>

#include <memory>

#include "async/task_scope.h"
#include "gtest/gtest.h"

// The simplest asynchronous functions: `co_return` is what makes them one.
Task<int> Add(int a, int b) { co_return a + b; }

Task<> SetFlag(bool& flag) {
  flag = true;
  co_return;
}

Task<std::unique_ptr<int>> MakeBox(int value) {
  co_return std::make_unique<int>(value);
}

// `co_await` runs a task and evaluates to what it co_returned.
Task<> AwaitYieldsResult() { EXPECT_EQ(co_await Add(2, 3), 5); }

TEST(TaskTest, AwaitYieldsResult) { TaskScope().Spawn(AwaitYieldsResult()); }

// Calling an asynchronous function only creates the task. Nothing in its
// body runs until the task is awaited.
Task<> DoesNothingUntilAwaited() {
  bool ran = false;
  const Task<> task = SetFlag(ran);
  EXPECT_FALSE(ran);
  co_await task;
  EXPECT_TRUE(ran);
}

TEST(TaskTest, DoesNothingUntilAwaited) {
  TaskScope().Spawn(DoesNothingUntilAwaited());
}

// A task that is never awaited is dropped without running.
TEST(TaskTest, DroppedTaskNeverRuns) {
  bool ran = false;
  {
    const Task<> task = SetFlag(ran);
  }
  EXPECT_FALSE(ran);
}

// Results are moved out of the task, so they need not be copyable.
Task<> ResultMayBeMoveOnly() {
  const std::unique_ptr<int> box = co_await MakeBox(7);
  EXPECT_EQ(*box, 7);
}

TEST(TaskTest, ResultMayBeMoveOnly) {
  TaskScope().Spawn(ResultMayBeMoveOnly());
}

// Awaiting a task that finishes without waiting does not deepen the stack,
// so a loop can await as many as it likes.
Task<> AwaitMillionTasks() {
  int total = 0;
  for (int i = 0; i < 1'000'000; ++i) total = co_await Add(total, 1);
  EXPECT_EQ(total, 1'000'000);
}

TEST(TaskTest, AwaitingInALoopDoesNotOverflowTheStack) {
  TaskScope().Spawn(AwaitMillionTasks());
}

// -----------------------------------------------------------------------------
// Benchmarks
// -----------------------------------------------------------------------------
//
// What it costs to call an asynchronous function.
//
//   bazel run -c opt //async:task_test -- --benchmark_filter=all
//
// The numbers to compare are PlainCall, an ordinary function call, and
// AwaitTask, the same work done by a task. The difference is the price of
// making a function asynchronous: allocating the task's state, starting it,
// and handing its result back.

namespace {

// The work being done is trivial on purpose, so that the measurements are of
// the call and not of the callee. noinline keeps the compiler from removing
// the call altogether.
[[gnu::noinline]] int AddPlain(int a, int b) { return a + b; }

// A task that awaits `depth` further tasks, one inside the other.
Task<int> Nested(int depth) {
  if (depth == 0) co_return 0;
  co_return 1 + co_await Nested(depth - 1);
}

// The baseline: an ordinary call.
void BM_PlainCall(benchmark::State& state) {
  int total = 0;
  for (auto _ : state) {
    total = AddPlain(total, 1);
    benchmark::DoNotOptimize(total);
  }
}
BENCHMARK(BM_PlainCall);

// Awaiting has to happen inside a task, so the measured loop lives in one.
Task<> AwaitInLoop(benchmark::State& state) {
  int total = 0;
  for (auto _ : state) {
    total = co_await Add(total, 1);
    benchmark::DoNotOptimize(total);
  }
}

// One `co_await` of a task that finishes without waiting.
void BM_AwaitTask(benchmark::State& state) {
  TaskScope().Spawn(AwaitInLoop(state));
}
BENCHMARK(BM_AwaitTask);

// Creating a task and dropping it without running it: the part of AwaitTask
// that is allocating and freeing the task's state.
void BM_CreateAndDropTask(benchmark::State& state) {
  for (auto _ : state) {
    Task<int> task = Add(1, 2);
    benchmark::DoNotOptimize(task);
  }
}
BENCHMARK(BM_CreateAndDropTask);

Task<> AwaitNestedInLoop(benchmark::State& state) {
  const int depth = static_cast<int>(state.range(0));
  for (auto _ : state) {
    benchmark::DoNotOptimize(co_await Nested(depth));
  }
  // Reported per task as well as per chain, to show how the cost of one task
  // changes with the depth of the chain it is in.
  state.SetItemsProcessed(state.iterations() * depth);
}

// Tasks awaiting tasks, as in ServeConnection -> ReadUntil -> Receive.
void BM_AwaitNestedTasks(benchmark::State& state) {
  TaskScope().Spawn(AwaitNestedInLoop(state));
}
BENCHMARK(BM_AwaitNestedTasks)->Arg(1)->Arg(8)->Arg(64);

}  // namespace

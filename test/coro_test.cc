// Copyright (c) 2024-2026 Nikolas Ioannou
// SPDX-License-Identifier: BSD-3-Clause

#include "udepot/coro.h"

#include <gtest/gtest.h>

using udepot::CoroTask;

namespace {

CoroTask<int> return_42() { co_return 42; }

CoroTask<int> return_negative() { co_return -1; }

CoroTask<int> add(int a, int b) { co_return a + b; }

CoroTask<int> call_inner() {
    int val = co_await return_42();
    co_return val + 8;
}

CoroTask<int> nested_three_deep() {
    int a = co_await return_42();
    int b = co_await add(a, 10);
    co_return b;
}

CoroTask<int> chain(int depth) {
    if (depth <= 0) co_return 0;
    int inner = co_await chain(depth - 1);
    co_return inner + 1;
}

}  // namespace

TEST(CoroTask, EagerStartCompletesImmediately) {
    auto task = return_42();
    EXPECT_TRUE(task.done());
}

TEST(CoroTask, RunSyncReturnsValue) {
    EXPECT_EQ(return_42().run_sync(), 42);
}

TEST(CoroTask, RunSyncNegative) {
    EXPECT_EQ(return_negative().run_sync(), -1);
}

TEST(CoroTask, RunSyncWithArgs) {
    EXPECT_EQ(add(10, 20).run_sync(), 30);
}

TEST(CoroTask, AwaitInnerCoroutine) {
    EXPECT_EQ(call_inner().run_sync(), 50);
}

TEST(CoroTask, NestedThreeDeep) {
    EXPECT_EQ(nested_three_deep().run_sync(), 52);
}

TEST(CoroTask, RecursiveChain) {
    EXPECT_EQ(chain(0).run_sync(), 0);
    EXPECT_EQ(chain(1).run_sync(), 1);
    EXPECT_EQ(chain(10).run_sync(), 10);
    EXPECT_EQ(chain(100).run_sync(), 100);
}

TEST(CoroTask, MoveConstruct) {
    auto a = return_42();
    auto b = std::move(a);
    EXPECT_TRUE(b.done());
    EXPECT_EQ(b.run_sync(), 42);
}

TEST(CoroTask, MoveAssign) {
    auto a = return_42();
    auto b = return_negative();
    b = std::move(a);
    EXPECT_EQ(b.run_sync(), 42);
}

TEST(CoroTask, DifferentReturnTypes) {
    auto si = []() -> CoroTask<ssize_t> { co_return 1024; };
    EXPECT_EQ(si().run_sync(), 1024);

    auto ui = []() -> CoroTask<uint64_t> { co_return 0xdeadbeef; };
    EXPECT_EQ(ui().run_sync(), 0xdeadbeef);
}

// Regression: run_sync formerly used a separate completed_ flag that was
// set BEFORE the continuation_ exchange — the frame could be destroyed
// between the two, causing use-after-free.  The fix removed completed_
// and spins on continuation_ == completed_tag().  This test exercises
// run_sync from a separate thread on a coroutine that actually suspends,
// maximising the window for the old race.
TEST(CoroTask, RunSyncFromAnotherThread) {
    // A coroutine that suspends once (co_await on an inner task that
    // itself completes eagerly).  The outer frame suspends at the
    // co_await point and is resumed by the inner task's FinalAwaitable.
    auto outer = []() -> CoroTask<int> {
        int v = co_await add(100, 200);
        co_return v;
    };

    constexpr int kIterations = 10000;
    for (int i = 0; i < kIterations; ++i) {
        auto task = outer();
        // run_sync spins until continuation_ is tagged — if the old
        // completed_ flag were still used, this could read freed memory.
        int r = task.run_sync();
        EXPECT_EQ(r, 300);
    }
}

// Regression: verify that run_sync on a truly eager coroutine (no
// suspension point) still works — the continuation_ exchange happens
// before run_sync even starts spinning.
TEST(CoroTask, RunSyncEagerNoSuspend) {
    constexpr int kIterations = 10000;
    for (int i = 0; i < kIterations; ++i) {
        EXPECT_EQ(return_42().run_sync(), 42);
    }
}

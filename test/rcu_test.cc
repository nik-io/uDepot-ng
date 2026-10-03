// Copyright (c) 2024-2026 Nikolas Ioannou
// SPDX-License-Identifier: BSD-3-Clause

#include "udepot/rcu.h"

#include <atomic>
#include <barrier>
#include <chrono>
#include <condition_variable>
#include <cstdio>
#include <cstdlib>
#include <deque>
#include <future>
#include <mutex>
#include <thread>
#include <vector>

#include <gtest/gtest.h>

using udepot::Rcu;

namespace {

// Runs synchronize() and fails the whole binary if it does not return in
// time: a hung grace period must fail the build, not stall CTest.
void synchronize_or_die(Rcu& rcu, std::chrono::seconds limit) {
    auto done = std::async(std::launch::async, [&] { rcu.synchronize(); });
    if (done.wait_for(limit) != std::future_status::ready) {
        std::fprintf(stderr, "synchronize() did not return within %llds\n",
                     static_cast<long long>(limit.count()));
        std::_Exit(1);
    }
}

}  // namespace

TEST(Rcu, ReadLockUnlock) {
    Rcu rcu;
    uint32_t idx = rcu.read_lock();
    rcu.read_unlock(idx);
    synchronize_or_die(rcu, std::chrono::seconds(10));
}

TEST(Rcu, NestedReadLocks) {
    Rcu rcu;
    uint32_t a = rcu.read_lock();
    uint32_t b = rcu.read_lock();
    uint32_t c = rcu.read_lock();
    rcu.read_unlock(c);
    rcu.read_unlock(b);
    rcu.read_unlock(a);
    synchronize_or_die(rcu, std::chrono::seconds(10));
}

TEST(Rcu, SynchronizeWithNoReaders) {
    Rcu rcu;
    rcu.synchronize();
    rcu.synchronize();
}

TEST(Rcu, ReadGuardScoping) {
    Rcu rcu;
    {
        Rcu::ReadGuard guard(rcu);
    }
    synchronize_or_die(rcu, std::chrono::seconds(10));
}

TEST(Rcu, SynchronizeWaitsForReader) {
    Rcu rcu;
    std::atomic<bool> entered{false};
    std::atomic<bool> released{false};

    std::thread reader([&] {
        uint32_t idx = rcu.read_lock();
        entered.store(true);
        std::this_thread::sleep_for(std::chrono::milliseconds(50));
        released.store(true);
        rcu.read_unlock(idx);
    });
    while (!entered.load()) std::this_thread::yield();

    rcu.synchronize();
    EXPECT_TRUE(released.load()) << "synchronize returned during a reader";
    reader.join();
}

// A coroutine enters its critical section on its caller's thread and often
// leaves it on an I/O poller. The grace period must cover it until then.
TEST(Rcu, SynchronizeWaitsForReaderThatUnlocksOnAnotherThread) {
    Rcu rcu;
    uint32_t idx = rcu.read_lock();  // entered on this thread
    std::atomic<bool> released{false};

    std::thread finisher([&] {
        std::this_thread::sleep_for(std::chrono::milliseconds(50));
        released.store(true);
        rcu.read_unlock(idx);  // left on another
    });
    std::thread writer([&] {
        rcu.synchronize();
        EXPECT_TRUE(released.load()) << "grace period ended early";
    });
    finisher.join();
    writer.join();
}

// Regression: the previous RCU kept a per-thread nesting count that the
// unlocking thread decremented. With many sections entered on one thread and
// left on another (batched coroutines on an async backend) the two threads
// raced on it; the count was lost, the entering thread looked like it was
// in a critical section forever, and synchronize() -- called by close() --
// hung.
TEST(Rcu, CrossThreadUnlockUnderLoad) {
    Rcu rcu;
    constexpr int kSections = 200000;
    std::mutex mu;
    std::condition_variable cv;
    std::deque<uint32_t> handoff;
    bool done = false;

    std::thread finisher([&] {
        for (;;) {
            std::unique_lock<std::mutex> lk(mu);
            cv.wait(lk, [&] { return !handoff.empty() || done; });
            if (handoff.empty()) return;
            uint32_t idx = handoff.front();
            handoff.pop_front();
            lk.unlock();
            rcu.read_unlock(idx);
        }
    });
    std::atomic<bool> stop_writer{false};
    std::thread writer([&] {
        while (!stop_writer.load()) rcu.synchronize();
    });

    for (int i = 0; i < kSections; ++i) {
        uint32_t idx = rcu.read_lock();
        if (i % 3 == 0) {
            rcu.read_unlock(idx);  // some finish on the entering thread
            continue;
        }
        std::lock_guard<std::mutex> lk(mu);
        handoff.push_back(idx);
        cv.notify_one();
    }
    {
        std::lock_guard<std::mutex> lk(mu);
        done = true;
        cv.notify_one();
    }
    finisher.join();
    stop_writer.store(true);
    writer.join();

    synchronize_or_die(rcu, std::chrono::seconds(10));
}

TEST(Rcu, ConcurrentReadersDoNotBlock) {
    Rcu rcu;
    constexpr int kNumReaders = 8;
    constexpr int kIterations = 10000;
    std::atomic<int> completed{0};

    std::vector<std::thread> threads;
    for (int i = 0; i < kNumReaders; ++i) {
        threads.emplace_back([&] {
            for (int j = 0; j < kIterations; ++j) {
                uint32_t idx = rcu.read_lock();
                rcu.read_unlock(idx);
            }
            completed.fetch_add(1, std::memory_order_relaxed);
        });
    }
    for (auto& th : threads) th.join();
    EXPECT_EQ(completed.load(), kNumReaders);
}

TEST(Rcu, WriterSeesAllReaderExits) {
    Rcu rcu;
    constexpr int kNumReaders = 4;
    constexpr int kIterations = 1000;
    // Each reader publishes the value it saw on entry and checks on exit
    // that a writer which observed it inside has not yet "reclaimed" it.
    std::atomic<int> generation{0};
    std::atomic<int> violations{0};

    std::vector<std::thread> readers;
    for (int i = 0; i < kNumReaders; ++i) {
        readers.emplace_back([&] {
            for (int j = 0; j < kIterations; ++j) {
                uint32_t idx = rcu.read_lock();
                int g = generation.load(std::memory_order_acquire);
                std::this_thread::yield();
                // The writer alternates bump, synchronize. After our read,
                // one bump can complete, but the synchronize that follows it
                // began after we entered and must wait for us; so seeing
                // g + 2 here means a grace period ended early.
                if (generation.load(std::memory_order_acquire) >= g + 2)
                    violations.fetch_add(1);
                rcu.read_unlock(idx);
            }
        });
    }
    for (int j = 0; j < 100; ++j) {
        generation.fetch_add(1, std::memory_order_release);
        rcu.synchronize();
        generation.fetch_add(1, std::memory_order_release);
        rcu.synchronize();
    }
    for (auto& th : readers) th.join();
    EXPECT_EQ(violations.load(), 0);
}

// Regression: a thread's slot was never released when it exited, so a
// server creating threads per connection ran out of them.
TEST(Rcu, ThreadChurnReleasesSlots) {
    Rcu rcu;
    for (uint32_t i = 0; i < 2 * Rcu::kMaxThreads; ++i) {
        std::thread th([&] {
            Rcu::ReadGuard guard(rcu);
        });
        th.join();
    }
    EXPECT_LE(rcu.thread_count(), 2u);
    synchronize_or_die(rcu, std::chrono::seconds(10));
}

// Regression: a long-lived thread kept an entry for every Rcu it had ever
// used (one per store opened), and scanned them all on each new one.
TEST(Rcu, ThreadForgetsDestroyedInstances) {
    Rcu kept;
    Rcu::ReadGuard{kept};
    for (int i = 0; i < 1000; ++i) {
        Rcu rcu;
        Rcu::ReadGuard guard(rcu);
    }
    {
        Rcu rcu;
        Rcu::ReadGuard guard(rcu);
        EXPECT_EQ(Rcu::this_thread_instances(), 2u);  // kept and this one
    }
    Rcu::ReadGuard again(kept);  // still known: no new slot claimed
    EXPECT_EQ(kept.thread_count(), 1u);
}

// With more threads than slots, the rest share one atomically updated slot;
// grace periods must still cover all of them.
TEST(Rcu, MoreThreadsThanSlots) {
    Rcu rcu;
    constexpr int kThreads = Rcu::kMaxThreads + 16;
    std::barrier inside(kThreads + 1);
    std::atomic<bool> release{false};
    std::atomic<int> still_inside{0};

    std::vector<std::thread> threads;
    for (int t = 0; t < kThreads; ++t) {
        threads.emplace_back([&] {
            uint32_t idx = rcu.read_lock();
            still_inside.fetch_add(1);
            inside.arrive_and_wait();
            while (!release.load()) std::this_thread::yield();
            still_inside.fetch_sub(1);
            rcu.read_unlock(idx);
        });
    }
    inside.arrive_and_wait();
    EXPECT_EQ(rcu.thread_count(), Rcu::kMaxThreads);

    std::thread writer([&] {
        rcu.synchronize();
        EXPECT_EQ(still_inside.load(), 0) << "grace period ended early";
    });
    std::this_thread::sleep_for(std::chrono::milliseconds(20));
    release.store(true);
    writer.join();
    for (auto& th : threads) th.join();
}

TEST(Rcu, ReaderFenceModeIsReported) {
    // Exercised in both modes by CTest (rcu_test and rcu_test_reader_fence).
    const char* force = std::getenv("UDEPOT_RCU_READER_FENCE");
    if (force && force[0] == '1') {
        EXPECT_TRUE(Rcu::reader_fence());
    }
#if !defined(__linux__)
    EXPECT_TRUE(Rcu::reader_fence());
#endif
}

// call_rcu: a callback waits for every read section that began before it
// was queued, including one still open on the queuing thread.
TEST(Rcu, CallRunsOnlyAfterPreexistingReaders) {
    Rcu rcu;
    std::atomic<bool> ran{false};
    uint32_t idx = rcu.read_lock();
    rcu.call([&] { ran.store(true); });
    std::this_thread::sleep_for(std::chrono::milliseconds(50));
    EXPECT_FALSE(ran.load());
    rcu.read_unlock(idx);
    rcu.barrier();
    EXPECT_TRUE(ran.load());
}

TEST(Rcu, BarrierWaitsForEveryQueuedCallback) {
    Rcu rcu;
    std::atomic<int> count{0};
    std::vector<std::thread> threads;
    for (int t = 0; t < 4; ++t)
        threads.emplace_back([&] {
            for (int i = 0; i < 1000; ++i)
                rcu.call([&] { count.fetch_add(1); });
        });
    for (auto& t : threads) t.join();
    rcu.barrier();
    EXPECT_EQ(count.load(), 4000);
}

TEST(Rcu, DestructorRunsPendingCallbacks) {
    std::atomic<int> count{0};
    {
        Rcu rcu;
        for (int i = 0; i < 10; ++i) rcu.call([&] { count.fetch_add(1); });
    }
    EXPECT_EQ(count.load(), 10);
}

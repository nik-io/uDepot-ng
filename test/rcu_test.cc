// Copyright (c) 2024-2026 Nikolas Ioannou
// SPDX-License-Identifier: BSD-3-Clause

#include "udepot/rcu.h"

#include <atomic>
#include <memory>
#include <thread>
#include <vector>

#include <gtest/gtest.h>

using udepot::Rcu;

TEST(Rcu, RegisterAndUnregister) {
    Rcu rcu;
    EXPECT_EQ(rcu.thread_count(), 0u);
    auto t = rcu.register_thread();
    EXPECT_TRUE(t.valid());
    EXPECT_EQ(rcu.thread_count(), 1u);
    rcu.unregister_thread(t);
}

TEST(Rcu, ReadLockUnlock) {
    Rcu rcu;
    auto t = rcu.register_thread();
    rcu.read_lock(t);
    rcu.read_unlock(t);
    rcu.unregister_thread(t);
}

TEST(Rcu, NestedReadLocks) {
    Rcu rcu;
    auto t = rcu.register_thread();
    rcu.read_lock(t);
    rcu.read_lock(t);
    rcu.read_lock(t);
    rcu.read_unlock(t);
    rcu.read_unlock(t);
    rcu.read_unlock(t);
    rcu.unregister_thread(t);
}

TEST(Rcu, SynchronizeWithNoReaders) {
    Rcu rcu;
    auto t = rcu.register_thread();
    rcu.synchronize();
    rcu.unregister_thread(t);
}

TEST(Rcu, SynchronizeWaitsForReader) {
    Rcu rcu;

    auto writer_token = rcu.register_thread();
    std::atomic<bool> reader_entered{false};
    std::atomic<bool> sync_done{false};

    std::thread reader([&] {
        auto t = rcu.register_thread();
        rcu.read_lock(t);
        reader_entered.store(true, std::memory_order_release);

        // Hold the read lock until the writer starts synchronize
        while (!sync_done.load(std::memory_order_acquire)) {
            std::this_thread::yield();
        }
        // Small delay to ensure synchronize is actually waiting
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
        rcu.read_unlock(t);
        rcu.unregister_thread(t);
    });

    // Wait for reader to enter critical section
    while (!reader_entered.load(std::memory_order_acquire)) {
        std::this_thread::yield();
    }

    sync_done.store(true, std::memory_order_release);
    rcu.synchronize();

    // If we got here, the reader has exited its critical section
    reader.join();
    rcu.unregister_thread(writer_token);
}

TEST(Rcu, ReadGuardScoping) {
    Rcu rcu;
    auto t = rcu.register_thread();
    {
        Rcu::ReadGuard guard(rcu, t);
        // Inside critical section
    }
    // Outside — synchronize should complete immediately
    rcu.synchronize();
    rcu.unregister_thread(t);
}

TEST(Rcu, ConcurrentReadersDoNotBlock) {
    Rcu rcu;
    constexpr int kNumReaders = 8;
    constexpr int kIterations = 10000;
    std::atomic<int> completed{0};

    std::vector<std::thread> threads;
    for (int i = 0; i < kNumReaders; ++i) {
        threads.emplace_back([&] {
            auto t = rcu.register_thread();
            for (int j = 0; j < kIterations; ++j) {
                rcu.read_lock(t);
                rcu.read_unlock(t);
            }
            completed.fetch_add(1, std::memory_order_relaxed);
            rcu.unregister_thread(t);
        });
    }

    for (auto& th : threads) th.join();
    EXPECT_EQ(completed.load(), kNumReaders);
}

TEST(Rcu, WriterSeesAllReaderExits) {
    Rcu rcu;
    constexpr int kNumReaders = 4;
    constexpr int kIterations = 1000;

    std::atomic<int> phase{0};
    std::vector<std::thread> readers;

    for (int i = 0; i < kNumReaders; ++i) {
        readers.emplace_back([&] {
            auto t = rcu.register_thread();
            for (int j = 0; j < kIterations; ++j) {
                rcu.read_lock(t);
                // Simulate work
                std::this_thread::yield();
                rcu.read_unlock(t);
            }
            rcu.unregister_thread(t);
        });
    }

    // Writer thread does synchronize repeatedly
    auto wt = rcu.register_thread();
    for (int j = 0; j < 100; ++j) {
        rcu.synchronize();
        phase.fetch_add(1, std::memory_order_relaxed);
    }
    rcu.unregister_thread(wt);

    for (auto& th : readers) th.join();
    EXPECT_GE(phase.load(), 100);
}

TEST(Rcu, MultipleTokensPerThread) {
    Rcu rcu;
    auto t1 = rcu.register_thread();
    auto t2 = rcu.register_thread();
    EXPECT_EQ(rcu.thread_count(), 2u);
    rcu.read_lock(t1);
    rcu.read_lock(t2);
    rcu.read_unlock(t2);
    rcu.read_unlock(t1);
    rcu.unregister_thread(t1);
    rcu.unregister_thread(t2);
}

// Regression: register_thread formerly used a monotonic counter and
// never reused slots.  After kMaxThreads cumulative registrations (even
// with only a few concurrent threads) the assert fired.  The fix scans
// for freed slots under a mutex before allocating new ones.
TEST(Rcu, SlotReuseDoesNotExhaust) {
    Rcu rcu;
    // Register and unregister more than kMaxThreads times, but only
    // one thread at a time — this would have crashed before the fix.
    for (uint32_t i = 0; i < Rcu::kMaxThreads + 100; ++i) {
        auto t = rcu.register_thread();
        ASSERT_TRUE(t.valid()) << "iteration " << i;
        rcu.read_lock(t);
        rcu.read_unlock(t);
        rcu.unregister_thread(t);
    }
    // High-water mark stays at 1 since we reused the same slot each time.
    EXPECT_EQ(rcu.thread_count(), 1u);
}

// Regression: verify slot reuse with concurrent threads registering and
// unregistering.  The mutex must serialize slot allocation correctly.
TEST(Rcu, ConcurrentSlotReuse) {
    Rcu rcu;
    constexpr int kThreads = 8;
    constexpr int kCycles = 200;
    std::atomic<int> completed{0};

    std::vector<std::thread> threads;
    for (int i = 0; i < kThreads; ++i) {
        threads.emplace_back([&] {
            for (int c = 0; c < kCycles; ++c) {
                auto t = rcu.register_thread();
                ASSERT_TRUE(t.valid());
                rcu.read_lock(t);
                std::this_thread::yield();
                rcu.read_unlock(t);
                rcu.unregister_thread(t);
            }
            completed.fetch_add(1, std::memory_order_relaxed);
        });
    }
    for (auto& th : threads) th.join();
    EXPECT_EQ(completed.load(), kThreads);
    // High-water mark should be at most kThreads (the peak concurrency).
    EXPECT_LE(rcu.thread_count(), static_cast<uint32_t>(kThreads));
}

// Regression: exhausting the slots used to be an assert, compiled out in
// release builds, after which register_thread wrote past threads_. It now
// returns an invalid token.
TEST(Rcu, ExhaustionReturnsInvalidToken) {
    Rcu rcu;
    std::vector<Rcu::Token> tokens;
    for (uint32_t i = 0; i < Rcu::kMaxThreads; ++i) {
        tokens.push_back(rcu.register_thread());
        ASSERT_TRUE(tokens.back().valid()) << i;
    }
    EXPECT_FALSE(rcu.register_thread().valid());

    rcu.unregister_thread(tokens.back());
    tokens.pop_back();
    auto again = rcu.register_thread();
    EXPECT_TRUE(again.valid()) << "a freed slot must be reusable";
    tokens.push_back(again);
    for (auto t : tokens) rcu.unregister_thread(t);
}

// unregister_if_alive is how a thread's tokens are released at thread
// exit, which can happen after the Rcu instance is gone.
TEST(Rcu, UnregisterIfAliveFreesSlotOnlyWhileAlive) {
    Rcu rcu;
    auto t = rcu.register_thread();
    Rcu::unregister_if_alive(rcu.id(), t);
    auto t2 = rcu.register_thread();
    EXPECT_EQ(rcu.thread_count(), 1u) << "slot was not freed";
    rcu.unregister_thread(t2);

    auto dead = std::make_unique<Rcu>();
    uint64_t dead_id = dead->id();
    auto dead_tok = dead->register_thread();
    dead.reset();
    Rcu::unregister_if_alive(dead_id, dead_tok);  // must not touch it
}

// Copyright (c) 2024-2026 Nikolas Ioannou
// SPDX-License-Identifier: BSD-3-Clause

#include "udepot/directory.h"

#include "table_test_util.h"

#include <atomic>
#include <chrono>
#include <cstdint>
#include <thread>
#include <vector>

#include <gtest/gtest.h>

using udepot::Directory;
using udepot::HashEntry;
using udepot::Rcu;
using udepot::test::MemTables;
using udepot::hash_to_tag;

static uint64_t make_hash(uint64_t bucket, uint8_t tag, uint32_t index_bits) {
    uint64_t mask = (1ULL << index_bits) - 1;
    return (static_cast<uint64_t>(tag) << 56) | (bucket & mask);
}

class DirectoryTest : public ::testing::Test {
protected:
    Rcu rcu_;
    uint32_t rcu_idx_ = 0;
};

TEST_F(DirectoryTest, InsertAndLookup) {
    MemTables source(10);
    Directory dir(rcu_, source, source.initial(4));

    rcu_idx_ = rcu_.read_lock();
    uint64_t hash = make_hash(42, 0xCC, 10);
    EXPECT_EQ(dir.insert(hash, 5, 1000), 0);

    HashEntry found = dir.lookup(hash);
    EXPECT_FALSE(found.empty());
    EXPECT_EQ(found.key_tag(), 0xCC);
    EXPECT_EQ(found.pba(), 1000u);
    rcu_.read_unlock(rcu_idx_);
}

TEST_F(DirectoryTest, LookupMiss) {
    MemTables source(10);
    Directory dir(rcu_, source, source.initial(4));

    rcu_idx_ = rcu_.read_lock();
    uint64_t hash = make_hash(42, 0xCC, 10);
    EXPECT_TRUE(dir.lookup(hash).empty());
    rcu_.read_unlock(rcu_idx_);
}

TEST_F(DirectoryTest, InsertAndRemove) {
    MemTables source(10);
    Directory dir(rcu_, source, source.initial(4));

    rcu_idx_ = rcu_.read_lock();
    uint64_t hash = make_hash(42, 0xCC, 10);
    EXPECT_EQ(dir.insert(hash, 5, 1000), 0);
    EXPECT_TRUE(dir.remove(hash, 1000));
    EXPECT_TRUE(dir.lookup(hash).empty());
    rcu_.read_unlock(rcu_idx_);
}

TEST_F(DirectoryTest, MultipleTablesRouteCorrectly) {
    MemTables source(10);
    Directory dir(rcu_, source, source.initial(8));

    rcu_idx_ = rcu_.read_lock();
    // Insert entries that should land in different tables.
    for (uint64_t i = 0; i < 32; ++i) {
        uint64_t hash = make_hash(i, static_cast<uint8_t>(i + 1), 10);
        EXPECT_EQ(dir.insert(hash, 1, i * 100), 0);
    }

    // Verify all are findable.
    for (uint64_t i = 0; i < 32; ++i) {
        uint64_t hash = make_hash(i, static_cast<uint8_t>(i + 1), 10);
        HashEntry found = dir.lookup(hash);
        EXPECT_FALSE(found.empty()) << "i=" << i;
        EXPECT_EQ(found.pba(), i * 100);
    }
    rcu_.read_unlock(rcu_idx_);
}

TEST_F(DirectoryTest, GrowPreservesEntries) {
    MemTables source(10);
    Directory dir(rcu_, source, source.initial(2));

    rcu_idx_ = rcu_.read_lock();
    // Insert entries.
    for (uint64_t i = 0; i < 20; ++i) {
        uint64_t hash = make_hash(i * 7, static_cast<uint8_t>(i + 1), 10);
        EXPECT_EQ(dir.insert(hash, 1, i + 100), 0);
    }
    rcu_.read_unlock(rcu_idx_);

    EXPECT_EQ(dir.num_tables(), 2u);
    EXPECT_EQ(dir.grow(), 0);
    EXPECT_EQ(dir.num_tables(), 4u);

    // All entries must still be findable after grow.
    rcu_idx_ = rcu_.read_lock();
    for (uint64_t i = 0; i < 20; ++i) {
        uint64_t hash = make_hash(i * 7, static_cast<uint8_t>(i + 1), 10);
        HashEntry found = dir.lookup(hash);
        EXPECT_FALSE(found.empty()) << "i=" << i;
        EXPECT_EQ(found.pba(), i + 100);
    }
    rcu_.read_unlock(rcu_idx_);
}

TEST_F(DirectoryTest, GrowTwice) {
    MemTables source(8);
    Directory dir(rcu_, source, source.initial(1));

    rcu_idx_ = rcu_.read_lock();
    for (uint64_t i = 0; i < 50; ++i) {
        uint64_t hash = make_hash(i, static_cast<uint8_t>(i % 254 + 1), 8);
        EXPECT_EQ(dir.insert(hash, 1, i + 200), 0);
    }
    rcu_.read_unlock(rcu_idx_);

    EXPECT_EQ(dir.grow(), 0);
    EXPECT_EQ(dir.num_tables(), 2u);
    EXPECT_EQ(dir.grow(), 0);
    EXPECT_EQ(dir.num_tables(), 4u);

    rcu_idx_ = rcu_.read_lock();
    for (uint64_t i = 0; i < 50; ++i) {
        uint64_t hash = make_hash(i, static_cast<uint8_t>(i % 254 + 1), 8);
        HashEntry found = dir.lookup(hash);
        EXPECT_FALSE(found.empty()) << "i=" << i;
        EXPECT_EQ(found.pba(), i + 200);
    }
    rcu_.read_unlock(rcu_idx_);
}

TEST_F(DirectoryTest, ConcurrentReadsAndGrow) {
    MemTables source(12);
    Directory dir(rcu_, source, source.initial(2));
    constexpr int kEntries = 500;

    // Pre-populate.
    rcu_idx_ = rcu_.read_lock();
    for (int i = 0; i < kEntries; ++i) {
        uint64_t hash =
            make_hash(i * 3, static_cast<uint8_t>(i % 254 + 1), 12);
        EXPECT_EQ(dir.insert(hash, 1, i + 1000), 0);
    }
    rcu_.read_unlock(rcu_idx_);

    std::atomic<bool> stop{false};
    std::atomic<uint64_t> reads{0};
    std::atomic<int> started{0};

    // Reader threads doing lookups concurrently with grow.
    std::vector<std::thread> readers;
    for (int r = 0; r < 4; ++r) {
        readers.emplace_back([&, r] {
            uint64_t local = 0;
            do {
                uint32_t rcu_idx = rcu_.read_lock();
                int idx = (r * 100 + local) % kEntries;
                uint64_t hash = make_hash(
                    idx * 3, static_cast<uint8_t>(idx % 254 + 1), 12);
                HashEntry entry = dir.lookup(hash);
                if (!entry.empty()) {
                    EXPECT_EQ(entry.pba(),
                              static_cast<uint64_t>(idx + 1000));
                }
                rcu_.read_unlock(rcu_idx);
                if (++local == 1) started.fetch_add(1);
            } while (!stop.load(std::memory_order_relaxed));
            reads.fetch_add(local, std::memory_order_relaxed);
        });
    }

    // Grow while readers are active. Wait for all of them first: under load
    // the grows could otherwise finish before any reader is scheduled.
    while (started.load() < 4) std::this_thread::yield();
    EXPECT_EQ(dir.grow(), 0);
    EXPECT_EQ(dir.grow(), 0);

    stop.store(true, std::memory_order_relaxed);
    for (auto& t : readers) t.join();

    EXPECT_GT(reads.load(), 0u);
    EXPECT_EQ(dir.num_tables(), 8u);

    // Final check: all entries still present.
    rcu_idx_ = rcu_.read_lock();
    for (int i = 0; i < kEntries; ++i) {
        uint64_t hash =
            make_hash(i * 3, static_cast<uint8_t>(i % 254 + 1), 12);
        HashEntry found = dir.lookup(hash);
        EXPECT_FALSE(found.empty()) << "i=" << i;
        EXPECT_EQ(found.pba(), static_cast<uint64_t>(i + 1000));
    }
    rcu_.read_unlock(rcu_idx_);
}

// Writers racing grows lose nothing: grow() waits out every writer that
// missed the frozen flag before it copies, and a writer that sees the flag
// retries on the new snapshot. No stripe lock is held by grow().
TEST_F(DirectoryTest, ConcurrentWritersAndGrowLoseNothing) {
    constexpr uint32_t kBits = 14;
    MemTables source(kBits);
    Directory dir(rcu_, source, source.initial(1));
    constexpr int kWriters = 4;
    constexpr int kPerWriter = 2500;  // 10000 entries in 16384 buckets
    std::atomic<int> started{0};
    std::atomic<int> frozen_waits{0};

    auto hash_of = [](int w, int i) {
        uint64_t n = static_cast<uint64_t>(w) * kPerWriter + i;
        return make_hash(n * 7919, static_cast<uint8_t>(n % 251 + 1), kBits);
    };
    auto pba_of = [](int w, int i) {
        return static_cast<uint64_t>(w) * kPerWriter + i + 1;
    };

    std::vector<std::thread> writers;
    for (int w = 0; w < kWriters; ++w) {
        writers.emplace_back([&, w] {
            started.fetch_add(1);
            for (int i = 0; i < kPerWriter; ++i) {
                for (;;) {
                    uint64_t gen;
                    {
                        Rcu::ReadGuard guard(rcu_);
                        auto locked = dir.lock_for(hash_of(w, i));
                        if (!locked.frozen()) {
                            // A writer can be descheduled between the flag
                            // check and its insert; make that common, so a
                            // grow that does not wait it out is caught.
                            if (i % 8 == 0)
                                std::this_thread::sleep_for(
                                    std::chrono::microseconds(20));
                            ASSERT_EQ(locked.table->insert_locked(
                                          hash_of(w, i), 1, pba_of(w, i)),
                                      0);
                            break;
                        }
                        gen = locked.snapshot->generation;
                    }
                    frozen_waits.fetch_add(1);
                    dir.wait_for_grow(gen);
                }
                if (i % 64 == 0) std::this_thread::yield();
            }
        });
    }

    while (started.load() < kWriters) std::this_thread::yield();
    for (int g = 0; g < 4; ++g) {
        EXPECT_EQ(dir.grow(), 0);
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
    for (auto& t : writers) t.join();
    EXPECT_EQ(dir.num_tables(), 16u);

    Rcu::ReadGuard guard(rcu_);
    int lost = 0;
    for (int w = 0; w < kWriters; ++w)
        for (int i = 0; i < kPerWriter; ++i)
            if (dir.entry_at(hash_of(w, i), pba_of(w, i)).empty()) ++lost;
    EXPECT_EQ(lost, 0) << "frozen waits: " << frozen_waits.load();
}

// As uDepot's grow(), each new table needs a segment of its own. Without
// one the grow fails, returns the tables it did get, and loses nothing.
TEST_F(DirectoryTest, GrowWithoutSpaceFailsAndKeepsEntries) {
    MemTables source(8, /*budget=*/3);  // room for 1 + 2, not 2 + 4
    Directory dir(rcu_, source, source.initial(1));
    {
        Rcu::ReadGuard guard(rcu_);
        for (uint64_t i = 0; i < 100; ++i)
            ASSERT_EQ(dir.insert(make_hash(i, static_cast<uint8_t>(i), 8), 1,
                                 i),
                      0);
    }
    ASSERT_EQ(dir.grow(), 0);
    EXPECT_EQ(dir.num_tables(), 2u);
    EXPECT_EQ(dir.grow_failures(), 0u);

    EXPECT_EQ(dir.grow(), -ENOSPC);
    EXPECT_EQ(dir.grow_failures(), 1u);
    EXPECT_EQ(dir.num_tables(), 2u);
    EXPECT_EQ(source.live(), 2u) << "a failed grow kept a table";
    Rcu::ReadGuard guard(rcu_);
    for (uint64_t i = 0; i < 100; ++i)
        EXPECT_EQ(dir.lookup(make_hash(i, static_cast<uint8_t>(i), 8)).pba(),
                  i);
}

// The old tables leave the directory with the grow: their segments are
// retired then, while readers may still use their memory.
TEST_F(DirectoryTest, GrowRetiresTheOldTables) {
    MemTables source(8);
    Directory dir(rcu_, source, source.initial(2));
    ASSERT_EQ(dir.grow(), 0);
    EXPECT_EQ(source.retired(), 2u);
    EXPECT_EQ(source.live(), 4u);
    ASSERT_EQ(dir.grow(), 0);
    EXPECT_EQ(source.retired(), 6u);
    EXPECT_EQ(source.live(), 8u);
}

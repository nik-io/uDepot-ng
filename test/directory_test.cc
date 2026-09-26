// Copyright (c) 2024-2026 Nikolas Ioannou
// SPDX-License-Identifier: BSD-3-Clause

#include "udepot/directory.h"

#include <atomic>
#include <cstdint>
#include <thread>
#include <vector>

#include <gtest/gtest.h>

using udepot::Directory;
using udepot::HashEntry;
using udepot::Rcu;
using udepot::hash_to_tag;

static uint64_t make_hash(uint64_t bucket, uint8_t tag, uint32_t index_bits) {
    uint64_t mask = (1ULL << index_bits) - 1;
    return (static_cast<uint64_t>(tag) << 56) | (bucket & mask);
}

class DirectoryTest : public ::testing::Test {
protected:
    void SetUp() override {
        token_ = rcu_.register_thread();
    }

    void TearDown() override {
        rcu_.unregister_thread(token_);
    }

    Rcu rcu_;
    Rcu::Token token_{};
};

TEST_F(DirectoryTest, InsertAndLookup) {
    Directory dir(rcu_, 4, 10);

    rcu_.read_lock(token_);
    uint64_t hash = make_hash(42, 0xCC, 10);
    EXPECT_EQ(dir.insert(hash, 5, 1000), 0);

    HashEntry found = dir.lookup(hash);
    EXPECT_FALSE(found.empty());
    EXPECT_EQ(found.key_tag(), 0xCC);
    EXPECT_EQ(found.pba(), 1000u);
    rcu_.read_unlock(token_);
}

TEST_F(DirectoryTest, LookupMiss) {
    Directory dir(rcu_, 4, 10);

    rcu_.read_lock(token_);
    uint64_t hash = make_hash(42, 0xCC, 10);
    EXPECT_TRUE(dir.lookup(hash).empty());
    rcu_.read_unlock(token_);
}

TEST_F(DirectoryTest, InsertAndRemove) {
    Directory dir(rcu_, 4, 10);

    rcu_.read_lock(token_);
    uint64_t hash = make_hash(42, 0xCC, 10);
    EXPECT_EQ(dir.insert(hash, 5, 1000), 0);
    EXPECT_TRUE(dir.remove(hash, 1000));
    EXPECT_TRUE(dir.lookup(hash).empty());
    rcu_.read_unlock(token_);
}

TEST_F(DirectoryTest, MultipleTablesRouteCorrectly) {
    Directory dir(rcu_, 8, 10);

    rcu_.read_lock(token_);
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
    rcu_.read_unlock(token_);
}

TEST_F(DirectoryTest, GrowPreservesEntries) {
    Directory dir(rcu_, 2, 10);

    rcu_.read_lock(token_);
    // Insert entries.
    for (uint64_t i = 0; i < 20; ++i) {
        uint64_t hash = make_hash(i * 7, static_cast<uint8_t>(i + 1), 10);
        EXPECT_EQ(dir.insert(hash, 1, i + 100), 0);
    }
    rcu_.read_unlock(token_);

    EXPECT_EQ(dir.num_tables(), 2u);
    EXPECT_EQ(dir.grow(), 0);
    EXPECT_EQ(dir.num_tables(), 4u);

    // All entries must still be findable after grow.
    rcu_.read_lock(token_);
    for (uint64_t i = 0; i < 20; ++i) {
        uint64_t hash = make_hash(i * 7, static_cast<uint8_t>(i + 1), 10);
        HashEntry found = dir.lookup(hash);
        EXPECT_FALSE(found.empty()) << "i=" << i;
        EXPECT_EQ(found.pba(), i + 100);
    }
    rcu_.read_unlock(token_);
}

TEST_F(DirectoryTest, GrowTwice) {
    Directory dir(rcu_, 1, 8);

    rcu_.read_lock(token_);
    for (uint64_t i = 0; i < 50; ++i) {
        uint64_t hash = make_hash(i, static_cast<uint8_t>(i % 254 + 1), 8);
        EXPECT_EQ(dir.insert(hash, 1, i + 200), 0);
    }
    rcu_.read_unlock(token_);

    EXPECT_EQ(dir.grow(), 0);
    EXPECT_EQ(dir.num_tables(), 2u);
    EXPECT_EQ(dir.grow(), 0);
    EXPECT_EQ(dir.num_tables(), 4u);

    rcu_.read_lock(token_);
    for (uint64_t i = 0; i < 50; ++i) {
        uint64_t hash = make_hash(i, static_cast<uint8_t>(i % 254 + 1), 8);
        HashEntry found = dir.lookup(hash);
        EXPECT_FALSE(found.empty()) << "i=" << i;
        EXPECT_EQ(found.pba(), i + 200);
    }
    rcu_.read_unlock(token_);
}

TEST_F(DirectoryTest, ConcurrentReadsAndGrow) {
    Directory dir(rcu_, 2, 12);
    constexpr int kEntries = 500;

    // Pre-populate.
    rcu_.read_lock(token_);
    for (int i = 0; i < kEntries; ++i) {
        uint64_t hash =
            make_hash(i * 3, static_cast<uint8_t>(i % 254 + 1), 12);
        EXPECT_EQ(dir.insert(hash, 1, i + 1000), 0);
    }
    rcu_.read_unlock(token_);

    std::atomic<bool> stop{false};
    std::atomic<uint64_t> reads{0};

    // Reader threads doing lookups concurrently with grow.
    std::vector<std::thread> readers;
    for (int r = 0; r < 4; ++r) {
        readers.emplace_back([&, r] {
            Rcu::Token tok = rcu_.register_thread();
            uint64_t local = 0;
            while (!stop.load(std::memory_order_relaxed)) {
                rcu_.read_lock(tok);
                int idx = (r * 100 + local) % kEntries;
                uint64_t hash = make_hash(
                    idx * 3, static_cast<uint8_t>(idx % 254 + 1), 12);
                HashEntry entry = dir.lookup(hash);
                if (!entry.empty()) {
                    EXPECT_EQ(entry.pba(),
                              static_cast<uint64_t>(idx + 1000));
                }
                rcu_.read_unlock(tok);
                ++local;
            }
            reads.fetch_add(local, std::memory_order_relaxed);
            rcu_.unregister_thread(tok);
        });
    }

    // Grow while readers are active.
    EXPECT_EQ(dir.grow(), 0);
    EXPECT_EQ(dir.grow(), 0);

    stop.store(true, std::memory_order_relaxed);
    for (auto& t : readers) t.join();

    EXPECT_GT(reads.load(), 0u);
    EXPECT_EQ(dir.num_tables(), 8u);

    // Final check: all entries still present.
    rcu_.read_lock(token_);
    for (int i = 0; i < kEntries; ++i) {
        uint64_t hash =
            make_hash(i * 3, static_cast<uint8_t>(i % 254 + 1), 12);
        HashEntry found = dir.lookup(hash);
        EXPECT_FALSE(found.empty()) << "i=" << i;
        EXPECT_EQ(found.pba(), static_cast<uint64_t>(i + 1000));
    }
    rcu_.read_unlock(token_);
}

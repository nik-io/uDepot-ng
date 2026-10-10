// Copyright (c) 2024-2026 Nikolas Ioannou
// SPDX-License-Identifier: BSD-3-Clause

#include "udepot/directory.h"

#include <atomic>
#include <chrono>
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
    Rcu rcu_;
    uint32_t rcu_idx_ = 0;
};

TEST_F(DirectoryTest, InsertAndLookup) {
    Directory dir(rcu_, 4, 10);

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
    Directory dir(rcu_, 4, 10);

    rcu_idx_ = rcu_.read_lock();
    uint64_t hash = make_hash(42, 0xCC, 10);
    EXPECT_TRUE(dir.lookup(hash).empty());
    rcu_.read_unlock(rcu_idx_);
}

TEST_F(DirectoryTest, InsertAndRemove) {
    Directory dir(rcu_, 4, 10);

    rcu_idx_ = rcu_.read_lock();
    uint64_t hash = make_hash(42, 0xCC, 10);
    EXPECT_EQ(dir.insert(hash, 5, 1000), 0);
    EXPECT_TRUE(dir.remove(hash, 1000));
    EXPECT_TRUE(dir.lookup(hash).empty());
    rcu_.read_unlock(rcu_idx_);
}

TEST_F(DirectoryTest, MultipleTablesRouteCorrectly) {
    Directory dir(rcu_, 8, 10);

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
    Directory dir(rcu_, 2, 10);

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
    Directory dir(rcu_, 1, 8);

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
    Directory dir(rcu_, 2, 12);
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

// Writers racing incremental resizes lose nothing. Each resize() only
// publishes the new tables; writers migrate the stripes they lock, and the
// next resize() finishes whatever is left on demand (as the space waker
// does when a table fills mid-resize). No writer ever waits.
TEST_F(DirectoryTest, ConcurrentWritersAndResizeLoseNothing) {
    constexpr uint32_t kBits = 14;
    Directory dir(rcu_, 1, kBits);
    constexpr int kWriters = 4;
    constexpr int kPerWriter = 2500;  // 10000 entries in 16384 buckets
    std::atomic<int> started{0};

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
                Rcu::ReadGuard guard(rcu_);
                auto locked = dir.lock_for(hash_of(w, i));
                // A writer can be descheduled holding its stripes; make that
                // common, so a migration that does not exclude it is caught.
                if (i % 8 == 0)
                    std::this_thread::sleep_for(std::chrono::microseconds(20));
                ASSERT_EQ(locked.table->insert_locked(hash_of(w, i), 1,
                                                      pba_of(w, i)),
                          0);
                if (i % 64 == 0) std::this_thread::yield();
            }
        });
    }

    while (started.load() < kWriters) std::this_thread::yield();
    for (int g = 0; g < 4; ++g) {
        EXPECT_EQ(dir.resize(), 0);
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
    for (auto& t : writers) t.join();
    dir.complete();
    EXPECT_FALSE(dir.resizing());
    EXPECT_EQ(dir.num_tables(), 16u);

    Rcu::ReadGuard guard(rcu_);
    int lost = 0;
    for (int w = 0; w < kWriters; ++w)
        for (int i = 0; i < kPerWriter; ++i)
            if (dir.entry_at(hash_of(w, i), pba_of(w, i)).empty()) ++lost;
    EXPECT_EQ(lost, 0);
}

// Readers never miss an entry while a resize migrates stripes under them
// and writers displace entries across stripe boundaries in the new tables.
TEST_F(DirectoryTest, ReadersNeverMissEntriesDuringResize) {
    constexpr uint32_t kBits = 14;
    Directory dir(rcu_, 1, kBits);
    constexpr int kKept = 3000;
    auto kept_hash = [](int i) {
        return make_hash(static_cast<uint64_t>(i) * 5,
                         static_cast<uint8_t>(i % 251 + 1), kBits);
    };
    {
        Rcu::ReadGuard guard(rcu_);
        for (int i = 0; i < kKept; ++i)
            ASSERT_EQ(dir.insert(kept_hash(i), 1, i + 1), 0);
    }

    std::atomic<bool> stop{false};
    std::atomic<int> misses{0};
    std::atomic<uint64_t> reads{0};
    std::vector<std::thread> threads;
    for (int r = 0; r < 3; ++r) {
        threads.emplace_back([&, r] {
            uint64_t n = 0;
            while (!stop.load(std::memory_order_relaxed)) {
                const int i = static_cast<int>((n * 7 + r) % kKept);
                Rcu::ReadGuard guard(rcu_);
                if (dir.lookup(kept_hash(i)).pba() != uint64_t(i + 1))
                    misses.fetch_add(1);
                ++n;
            }
            reads.fetch_add(n);
        });
    }
    // Writers fill buckets near the kept ones, forcing displacement.
    std::atomic<int> inserted{0};
    for (int w = 0; w < 2; ++w) {
        threads.emplace_back([&, w] {
            for (int i = 0; i < 4000 && !stop.load(); ++i) {
                const uint64_t n = static_cast<uint64_t>(w) * 4000 + i;
                const uint64_t h = make_hash(
                    n * 5 + 1, static_cast<uint8_t>(n % 249 + 3), kBits);
                Rcu::ReadGuard guard(rcu_);
                if (dir.insert(h, 1, 100000 + n) == 0) inserted.fetch_add(1);
            }
        });
    }
    for (int g = 0; g < 3; ++g) {
        EXPECT_EQ(dir.resize(), 0);
        std::this_thread::sleep_for(std::chrono::milliseconds(2));
    }
    std::this_thread::sleep_for(std::chrono::milliseconds(20));
    stop.store(true);
    for (auto& t : threads) t.join();
    dir.complete();
    EXPECT_GT(reads.load(), 0u);
    EXPECT_GT(inserted.load(), 0);
    EXPECT_EQ(misses.load(), 0);
}

namespace {

// The stripe geometry of a table with kBits index bits: 16416 slots in 4
// stripes of 4104 (each covering a write's 2112-slot reach).
constexpr uint32_t kGeoBits = 14;
constexpr uint64_t kStripe = 4104;

}  // namespace

// A resize migrates only the stripes writes lock (paper §4.3). A lookup
// whose neighborhood straddles a migrated and an unmigrated stripe reads
// each slot from the right table: the migrated stripe from the new one
// (where a key written since lives only), the rest from the old one.
TEST_F(DirectoryTest, ResizeMigratesOnlyLockedStripes) {
    {
        // The geometry this test is built on.
        udepot::HashTable t(kGeoBits);
        ASSERT_EQ(t.num_stripes(), 4u);
        ASSERT_EQ(t.stripe_end(0), kStripe);
    }
    Directory dir(rcu_, 1, kGeoBits);
    // Ten keys of one bucket, two slots before stripe 2 starts: they fill
    // slots 8206..8215, across the stripe 1 / stripe 2 boundary.
    const uint64_t bucket = 2 * kStripe - 2;
    auto key = [&](int i) {
        return make_hash(bucket, static_cast<uint8_t>(0x10 + i), kGeoBits);
    };
    {
        Rcu::ReadGuard guard(rcu_);
        for (int i = 0; i < 10; ++i) ASSERT_EQ(dir.insert(key(i), 1, i + 1), 0);
    }

    ASSERT_EQ(dir.resize(), 0);
    EXPECT_TRUE(dir.resizing());
    EXPECT_EQ(dir.num_tables(), 2u);

    // A key early in stripe 1: its reach (32 slots below its bucket, 2080
    // from it) stays inside stripe 1, so writing it migrates stripe 1 only.
    // It then exists in the new table only.
    const uint64_t mid = make_hash(kStripe + 100, 0x10, kGeoBits);
    {
        udepot::HashTable t(kGeoBits);
        ASSERT_EQ(t.stripes_for(mid), (std::pair<uint64_t, uint64_t>{1, 1}));
    }
    {
        Rcu::ReadGuard guard(rcu_);
        ASSERT_EQ(dir.insert(mid, 1, 500), 0);
    }
    EXPECT_TRUE(dir.resizing());  // stripes 0, 2 and 3 have not migrated

    {
        Rcu::ReadGuard guard(rcu_);
        EXPECT_EQ(dir.lookup(mid).pba(), 500u);  // new table only
        for (int i = 0; i < 10; ++i) {
            // Keys 0 and 1 sit in (migrated) stripe 1, the rest in stripe 2.
            EXPECT_EQ(dir.lookup(key(i)).pba(), uint64_t(i + 1)) << i;
            EXPECT_FALSE(dir.entry_at(key(i), i + 1).empty()) << i;
        }
    }

    dir.complete();
    EXPECT_FALSE(dir.resizing());
    Rcu::ReadGuard guard(rcu_);
    EXPECT_EQ(dir.lookup(mid).pba(), 500u);
    for (int i = 0; i < 10; ++i)
        EXPECT_EQ(dir.lookup(key(i)).pba(), uint64_t(i + 1)) << i;
}

// The write that migrates the last stripe publishes the final snapshot,
// whose writers take the old stripes too until a grace period has passed
// (the handover); then they take only their own.
TEST_F(DirectoryTest, LastMigrationPublishesFinalWithHandover) {
    Directory dir(rcu_, 1, kGeoBits);
    ASSERT_EQ(dir.resize(), 0);
    ASSERT_TRUE(dir.resizing());
    {
        // One write in the middle of each stripe migrates them all.
        Rcu::ReadGuard guard(rcu_);
        for (uint64_t s = 0; s < 4; ++s)
            ASSERT_EQ(dir.insert(make_hash(s * kStripe + kStripe / 2, 0x80,
                                           kGeoBits),
                                 1, s + 1),
                      0);
        EXPECT_FALSE(dir.resizing());
        auto locked = dir.lock_for(make_hash(10, 0x80, kGeoBits));
        ASSERT_NE(locked.snapshot->handover, nullptr);
        EXPECT_TRUE(locked.snapshot->handover->active.load());
    }
    rcu_.barrier();  // the grace period after the final snapshot
    Rcu::ReadGuard guard(rcu_);
    auto locked = dir.lock_for(make_hash(10, 0x80, kGeoBits));
    EXPECT_FALSE(locked.snapshot->handover->active.load());
    for (uint64_t s = 0; s < 4; ++s)
        EXPECT_EQ(dir.lookup(make_hash(s * kStripe + kStripe / 2, 0x80,
                                       kGeoBits))
                      .pba(),
                  s + 1);
}

// A table filling up mid-resize (the space waker's resize() for the
// resizing snapshot) finishes the resize on demand and starts the next.
TEST_F(DirectoryTest, ResizeMidResizeFinishesItAndStartsNext) {
    Directory dir(rcu_, 1, kGeoBits);
    {
        Rcu::ReadGuard guard(rcu_);
        for (int i = 0; i < 200; ++i)
            ASSERT_EQ(dir.insert(make_hash(i * 61, static_cast<uint8_t>(i),
                                           kGeoBits),
                                 1, i + 1),
                      0);
    }
    ASSERT_EQ(dir.resize(), 0);
    ASSERT_TRUE(dir.resizing());
    ASSERT_EQ(dir.num_tables(), 2u);
    ASSERT_EQ(dir.resize(), 0);  // nothing migrated yet: all on demand
    EXPECT_TRUE(dir.resizing());
    EXPECT_EQ(dir.num_tables(), 4u);
    dir.complete();
    Rcu::ReadGuard guard(rcu_);
    for (int i = 0; i < 200; ++i)
        EXPECT_EQ(dir.lookup(make_hash(i * 61, static_cast<uint8_t>(i),
                                       kGeoBits))
                      .pba(),
                  uint64_t(i + 1))
            << i;
}

// A resize request for a snapshot that is no longer current does nothing:
// someone else already resized.
TEST_F(DirectoryTest, StaleResizeRequestIsANoOp) {
    Directory dir(rcu_, 1, kGeoBits);
    uint64_t gen;
    {
        Rcu::ReadGuard guard(rcu_);
        gen = dir.lock_for(make_hash(1, 1, kGeoBits)).snapshot->generation;
    }
    ASSERT_EQ(dir.resize(gen), 0);
    ASSERT_EQ(dir.num_tables(), 2u);
    ASSERT_EQ(dir.resize(gen), 0);  // stale
    EXPECT_EQ(dir.num_tables(), 2u);
    EXPECT_TRUE(dir.resizing());
}

// The handover: a writer still on the resizing snapshot holds the old
// stripe lock and writes the new table. A writer on the final snapshot,
// published meanwhile by other writes, must not get the same region until
// it lets go (it takes the old stripe too, for a grace period).
TEST_F(DirectoryTest, HandoverExcludesWritersStillOnTheResizingSnapshot) {
    Directory dir(rcu_, 1, kGeoBits);
    ASSERT_EQ(dir.resize(), 0);
    const uint64_t key = make_hash(100, 0x20, kGeoBits);  // stripe 0 only

    std::atomic<bool> holding{false}, released{false}, b_locked{false};
    std::atomic<bool> b_before_release{false};
    std::thread a([&] {
        Rcu::ReadGuard guard(rcu_);
        auto locked = dir.lock_for(key);  // resizing: old stripe 0, migrated
        ASSERT_TRUE(locked.snapshot->resize != nullptr);
        holding.store(true);
        std::this_thread::sleep_for(std::chrono::milliseconds(100));
        ASSERT_EQ(locked.table->insert_locked(key, 1, 1), 0);
        released.store(true);
    });
    while (!holding.load()) std::this_thread::yield();
    {
        // Migrate stripes 1..3; the last publishes the final snapshot.
        Rcu::ReadGuard guard(rcu_);
        for (uint64_t s = 1; s < 4; ++s)
            ASSERT_EQ(dir.insert(make_hash(s * kStripe + 100, 0x20, kGeoBits),
                                 1, s + 10),
                      0);
    }
    ASSERT_FALSE(dir.resizing());
    std::thread b([&] {
        Rcu::ReadGuard guard(rcu_);
        auto locked = dir.lock_for(key);  // final, handover active
        b_before_release.store(!released.load());
        b_locked.store(true);
        EXPECT_FALSE(locked.table->lookup(key).empty());
    });
    a.join();
    b.join();
    EXPECT_TRUE(b_locked.load());
    EXPECT_FALSE(b_before_release.load())
        << "a writer on the final snapshot got the region while one on the "
           "resizing snapshot still held it";
}

// ResizeMode::kFreeze, the earlier mechanism kept for comparison: writers
// racing grows lose nothing. grow() waits out every writer that missed the
// frozen flag before it copies, and a writer that sees the flag retries on
// the new snapshot. No stripe lock is held by grow().
TEST_F(DirectoryTest, FreezeModeConcurrentWritersAndGrowLoseNothing) {
    constexpr uint32_t kBits = 14;
    Directory dir(rcu_, 1, kBits, udepot::ResizeMode::kFreeze);
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

// kFreeze never leaves a resize in progress: grow() returns with every
// entry in the doubled tables.
TEST_F(DirectoryTest, FreezeModeGrowPreservesEntries) {
    Directory dir(rcu_, 2, 10, udepot::ResizeMode::kFreeze);
    {
        Rcu::ReadGuard guard(rcu_);
        for (uint64_t i = 0; i < 200; ++i)
            ASSERT_EQ(dir.insert(make_hash(i * 7, static_cast<uint8_t>(i + 1),
                                           10),
                                 1, i + 100),
                      0);
    }
    ASSERT_EQ(dir.resize(), 0);
    EXPECT_FALSE(dir.resizing());
    EXPECT_EQ(dir.num_tables(), 4u);
    EXPECT_EQ(dir.grow(), 0);
    EXPECT_EQ(dir.num_tables(), 8u);
    Rcu::ReadGuard guard(rcu_);
    for (uint64_t i = 0; i < 200; ++i)
        EXPECT_EQ(dir.lookup(make_hash(i * 7, static_cast<uint8_t>(i + 1), 10))
                      .pba(),
                  i + 100);
}

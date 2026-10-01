// Copyright (c) 2024-2026 Nikolas Ioannou
// SPDX-License-Identifier: BSD-3-Clause

#include "udepot/hash_table.h"

#include <atomic>
#include <cstdint>
#include <thread>
#include <vector>

#include <gtest/gtest.h>

using udepot::HashEntry;
using udepot::HashTable;
using udepot::hash_to_tag;

// Construct a hash value that maps to a specific bucket and tag.
static uint64_t make_hash(uint64_t bucket, uint8_t tag, uint32_t index_bits) {
    uint64_t mask = (1ULL << index_bits) - 1;
    return (static_cast<uint64_t>(tag) << 56) | (bucket & mask);
}

TEST(HashEntry, PackUnpack) {
    auto e = HashEntry::make(5, 0xAB, 100, 0xDEADBEEF);
    EXPECT_EQ(e.bucket_offset(), 5);
    EXPECT_EQ(e.key_tag(), 0xAB);
    EXPECT_EQ(e.kv_size(), 100);
    EXPECT_EQ(e.pba(), 0xDEADBEEF);
    EXPECT_FALSE(e.empty());
}

TEST(HashEntry, Empty) {
    HashEntry e;
    EXPECT_TRUE(e.empty());
    EXPECT_FALSE(e.deleted());
    EXPECT_EQ(e.raw(), HashEntry::kEmpty);
}

// A deleted entry keeps its tombstone pba with kv_size 0. With an all-zero
// empty encoding, a deleted entry at offset 0 with tag 0 and pba 0 would be
// indistinguishable from an empty slot; uDepot's unused-pba encoding cannot
// collide.
TEST(HashEntry, DeletedEntryIsNeverEmpty) {
    auto e = HashEntry::make(0, 0, 0, 0);
    EXPECT_FALSE(e.empty());
    EXPECT_TRUE(e.deleted());
    EXPECT_FALSE(HashEntry::make(0, 0, 1, 0).deleted());
}

TEST(HashEntry, AtomicLoadStore) {
    std::atomic<uint64_t> slot{0};
    auto e = HashEntry::make(0, 42, 10, 1234);
    HashEntry::store(slot, e);
    auto loaded = HashEntry::load(slot);
    EXPECT_EQ(loaded, e);
}

TEST(HashEntry, MaxValues) {
    // The all-ones pba marks an unused slot, so the largest usable pba is
    // one below it.
    auto e = HashEntry::make(31, 255, 2047, (1ULL << 40) - 2);
    EXPECT_EQ(e.bucket_offset(), 31);
    EXPECT_EQ(e.key_tag(), 255);
    EXPECT_EQ(e.kv_size(), 2047);
    EXPECT_EQ(e.pba(), (1ULL << 40) - 2);
    EXPECT_FALSE(e.empty());
}

TEST(HashTable, InsertAndLookup) {
    HashTable table(10);  // 1024 buckets
    uint64_t hash = make_hash(42, 0xCC, 10);
    EXPECT_EQ(table.insert(hash, 5, 1000), 0);

    HashEntry found = table.lookup(hash);
    EXPECT_FALSE(found.empty());
    EXPECT_EQ(found.key_tag(), 0xCC);
    EXPECT_EQ(found.kv_size(), 5);
    EXPECT_EQ(found.pba(), 1000u);
}

TEST(HashTable, LookupMiss) {
    HashTable table(10);
    uint64_t hash = make_hash(42, 0xCC, 10);
    HashEntry found = table.lookup(hash);
    EXPECT_TRUE(found.empty());
}

TEST(HashTable, InsertAndRemove) {
    HashTable table(10);
    uint64_t hash = make_hash(42, 0xCC, 10);
    EXPECT_EQ(table.insert(hash, 5, 1000), 0);
    EXPECT_TRUE(table.remove(hash, 1000));
    EXPECT_TRUE(table.lookup(hash).empty());
}

TEST(HashTable, RemoveMiss) {
    HashTable table(10);
    uint64_t hash = make_hash(42, 0xCC, 10);
    EXPECT_FALSE(table.remove(hash, 999));
}

TEST(HashTable, MultipleInsertsSameBucket) {
    HashTable table(10);
    // Insert multiple entries that hash to the same bucket but different tags.
    for (uint8_t tag = 1; tag <= 20; ++tag) {
        uint64_t hash = make_hash(100, tag, 10);
        EXPECT_EQ(table.insert(hash, tag, tag * 100), 0);
    }

    // Verify all are findable.
    for (uint8_t tag = 1; tag <= 20; ++tag) {
        uint64_t hash = make_hash(100, tag, 10);
        HashEntry found = table.lookup(hash);
        EXPECT_FALSE(found.empty()) << "tag=" << static_cast<int>(tag);
        EXPECT_EQ(found.key_tag(), tag);
        EXPECT_EQ(found.pba(), static_cast<uint64_t>(tag * 100));
    }
}

TEST(HashTable, FillAndOverflow) {
    // Small table to test overflow.
    HashTable table(4);  // 16 buckets
    int inserted = 0;
    for (int i = 0; i < 100; ++i) {
        uint64_t hash = make_hash(0, static_cast<uint8_t>(i + 1), 4);
        if (table.insert(hash, 1, i) == 0) {
            ++inserted;
        }
    }
    // Should have inserted some but not all (limited by hop range + table size).
    EXPECT_GT(inserted, 0);
    EXPECT_LT(inserted, 100);
}

// Regression: stripe locks allowed two writers to adjacent buckets to claim
// the same empty slot, because hopscotch neighborhoods (32 slots) overlap.
// The fix uses a per-table write mutex.  This test does concurrent inserts
// to adjacent buckets and verifies every entry survives — under the old
// stripe locks, entries would silently be lost.
TEST(HashTable, ConcurrentWritersAdjacentBuckets) {
    constexpr uint32_t kBits = 14;  // 16384 buckets
    HashTable table(kBits);
    constexpr int kWriters = 4;
    constexpr int kEntriesPerWriter = 500;
    std::atomic<int> failures{0};

    std::vector<std::thread> writers;
    for (int w = 0; w < kWriters; ++w) {
        writers.emplace_back([&, w] {
            for (int i = 0; i < kEntriesPerWriter; ++i) {
                // Each writer targets a contiguous range of buckets so
                // neighborhoods overlap with the adjacent writer's range.
                uint64_t bucket = static_cast<uint64_t>(w * kEntriesPerWriter + i)
                                  % table.num_buckets();
                uint8_t tag = static_cast<uint8_t>((i % 254) + 1);
                uint64_t hash = make_hash(bucket, tag, kBits);
                uint64_t pba = static_cast<uint64_t>(w * kEntriesPerWriter + i);
                int rc = table.insert(hash, 1, pba);
                if (rc != 0) failures.fetch_add(1, std::memory_order_relaxed);
            }
        });
    }
    for (auto& w : writers) w.join();

    // Verify: every successfully inserted entry must be findable.
    int found = 0;
    for (int w = 0; w < kWriters; ++w) {
        for (int i = 0; i < kEntriesPerWriter; ++i) {
            uint64_t bucket = static_cast<uint64_t>(w * kEntriesPerWriter + i)
                              % table.num_buckets();
            uint8_t tag = static_cast<uint8_t>((i % 254) + 1);
            uint64_t hash = make_hash(bucket, tag, kBits);
            uint64_t pba = static_cast<uint64_t>(w * kEntriesPerWriter + i);

            // Scan all tag-matching entries (there may be tag collisions).
            uint32_t off = 0;
            bool located = false;
            while (true) {
                HashEntry e = table.lookup(hash, off);
                if (e.empty()) break;
                if (e.pba() == pba) { located = true; break; }
                off = e.bucket_offset() + 1;
            }
            if (located) ++found;
        }
    }
    int expected = kWriters * kEntriesPerWriter - failures.load();
    EXPECT_EQ(found, expected)
        << "data loss: inserted " << expected << " but found " << found;
}

TEST(HashTable, ConcurrentReadsWhileWriting) {
    HashTable table(14);  // 16384 buckets
    constexpr int kNumWriters = 2;
    constexpr int kNumReaders = 4;
    constexpr int kOpsPerThread = 5000;
    std::atomic<bool> stop{false};
    std::atomic<int> writes_done{0};

    // Writers insert entries.
    std::vector<std::thread> writers;
    for (int w = 0; w < kNumWriters; ++w) {
        writers.emplace_back([&, w] {
            for (int i = 0; i < kOpsPerThread; ++i) {
                uint64_t bucket = (w * kOpsPerThread + i) % table.num_buckets();
                uint8_t tag = static_cast<uint8_t>((i % 254) + 1);
                uint64_t hash = make_hash(bucket, tag, table.index_bits());
                table.insert(hash, 1, bucket * 256 + tag);
            }
            writes_done.fetch_add(1, std::memory_order_relaxed);
        });
    }

    // Readers do lock-free lookups concurrently.
    std::vector<std::thread> readers;
    std::atomic<uint64_t> reads{0};
    for (int r = 0; r < kNumReaders; ++r) {
        readers.emplace_back([&, r] {
            uint64_t local_reads = 0;
            // do-while: under load the writers can finish before a reader
            // is first scheduled, and the test must still exercise reads.
            do {
                uint64_t bucket = (r * 1000 + local_reads) % table.num_buckets();
                uint8_t tag = static_cast<uint8_t>((local_reads % 254) + 1);
                uint64_t hash = make_hash(bucket, tag, table.index_bits());
                HashEntry entry = table.lookup(hash);
                // Entry may or may not be found — we just verify no crash
                // and that found entries have valid fields.
                if (!entry.empty()) {
                    EXPECT_EQ(entry.key_tag(), tag);
                }
                ++local_reads;
            } while (!stop.load(std::memory_order_relaxed));
            reads.fetch_add(local_reads, std::memory_order_relaxed);
        });
    }

    for (auto& w : writers) w.join();
    stop.store(true, std::memory_order_relaxed);
    for (auto& r : readers) r.join();

    EXPECT_EQ(writes_done.load(), kNumWriters);
    EXPECT_GT(reads.load(), 0u);
}

TEST(HashTable, DeletedEntriesVisibleOnlyOnRequest) {
    HashTable table(10);
    uint64_t hash = make_hash(42, 0xCC, 10);
    ASSERT_EQ(table.insert(hash, 3, 1000), 0);
    // Delete: the entry stays, pointing at the tombstone.
    ASSERT_TRUE(table.update(hash, 1000, 0, 2000));
    EXPECT_TRUE(table.lookup(hash).empty());
    HashEntry d = table.lookup(hash, 0, /*include_deleted=*/true);
    ASSERT_FALSE(d.empty());
    EXPECT_TRUE(d.deleted());
    EXPECT_EQ(d.pba(), 2000u);
    // Re-put of the key reuses its entry.
    ASSERT_TRUE(table.update(hash, 2000, 4, 3000));
    EXPECT_EQ(table.lookup(hash).pba(), 3000u);
}

// Stripes follow uDepot's rule: as many as fit (up to kMaxStripes) while
// each still covers a write's whole reach, so a write locks at most two.
TEST(HashTable, StripesCoverAWritesReach) {
    HashTable small(10);  // 1024 buckets: smaller than one write's reach
    EXPECT_EQ(small.num_stripes(), 1u);
    HashTable big(20);
    EXPECT_GT(big.num_stripes(), 1u);
    EXPECT_LE(big.num_stripes(), HashTable::kMaxStripes);
    uint64_t reach = HashEntry::kHopRange * (HashTable::kMaxDisplace + 2);
    EXPECT_GE(big.total_slots() / big.num_stripes(), reach);
}

// A full neighborhood borrows a free slot from up to kMaxDisplace
// neighborhoods away by moving entries forward; every entry stays findable.
TEST(HashTable, FullNeighborhoodDisplacesForward) {
    constexpr uint32_t kBits = 12;
    HashTable table(kBits);
    // Fill bucket 100's neighborhood and the next few buckets' slots.
    std::vector<uint64_t> hashes;
    for (int i = 0; i < 48; ++i) {
        uint64_t bucket = 100 + static_cast<uint64_t>(i) / 2;
        uint64_t hash = make_hash(bucket, static_cast<uint8_t>(i + 1), kBits);
        ASSERT_EQ(table.insert(hash, 1, 5000 + i), 0) << i;
        hashes.push_back(hash);
    }
    for (int i = 0; i < 48; ++i) {
        uint32_t off = 0;
        bool found = false;
        for (;;) {
            HashEntry e = table.lookup(hashes[i], off);
            if (e.empty()) break;
            if (e.pba() == static_cast<uint64_t>(5000 + i)) { found = true; break; }
            off = e.bucket_offset() + 1;
        }
        EXPECT_TRUE(found) << i;
    }
}

// The free-slot search is bounded, as in uDepot: a slot further away than
// kMaxDisplace neighborhoods is not used, and the insert reports the table
// full so the directory grows.
TEST(HashTable, InsertSearchIsBounded) {
    constexpr uint32_t kBits = 14;
    HashTable table(kBits);
    uint64_t window = HashEntry::kHopRange * (HashTable::kMaxDisplace + 1);
    // Occupy every slot of bucket 0's search window with entries homed at
    // their own slot, so none can be displaced into bucket 0's
    // neighborhood.
    for (uint64_t b = 0; b < window; ++b) {
        ASSERT_EQ(table.insert(make_hash(b, 1, kBits), 1, b + 1), 0) << b;
    }
    EXPECT_EQ(table.insert(make_hash(0, 2, kBits), 1, 999999), -1);
}

// Writers interleave keys in one band straddling a stripe boundary, so
// neighborhoods overflow and displacement crosses into the next stripe
// while other writers insert there. If a write's lock did not cover every
// slot it touches, two writers would claim the same free slot and entries
// would be lost.
TEST(HashTable, ConcurrentWritersAcrossStripeBoundaries) {
    constexpr uint32_t kBits = 16;
    constexpr int kWriters = 8;
    constexpr int kPerWriter = 1000;
    constexpr int kRounds = 10;
    int lost = 0;

    for (int round = 0; round < kRounds; ++round) {
        HashTable table(kBits);
        ASSERT_GT(table.num_stripes(), 2u);
        const uint64_t boundary = table.total_slots() / table.num_stripes();
        auto bucket_of = [&](int w, int i) {
            // 3 entries per bucket: every neighborhood overflows forward.
            return boundary - 600 +
                   static_cast<uint64_t>(i * kWriters + w) / 3;
        };
        auto hash_of = [&](int w, int i) {
            return make_hash(bucket_of(w, i),
                             static_cast<uint8_t>((i * kWriters + w) % 255 + 1),
                             kBits);
        };
        std::atomic<int> failures{0};
        std::atomic<int> ready{0};
        std::vector<std::thread> writers;
        for (int w = 0; w < kWriters; ++w) {
            writers.emplace_back([&, w] {
                ready.fetch_add(1);
                while (ready.load() < kWriters) std::this_thread::yield();
                for (int i = 0; i < kPerWriter; ++i) {
                    if (table.insert(hash_of(w, i), 1, w * 100000 + i + 1) != 0)
                        failures.fetch_add(1);
                }
            });
        }
        for (auto& t : writers) t.join();

        int found = 0;
        for (int w = 0; w < kWriters; ++w) {
            for (int i = 0; i < kPerWriter; ++i) {
                uint64_t pba = w * 100000 + i + 1;
                for (uint32_t off = 0;;) {
                    HashEntry e = table.lookup(hash_of(w, i), off);
                    if (e.empty()) break;
                    if (e.pba() == pba) { ++found; break; }
                    off = e.bucket_offset() + 1;
                }
            }
        }
        lost += kWriters * kPerWriter - failures.load() - found;
    }
    EXPECT_EQ(lost, 0) << "entries inserted but not findable";
}

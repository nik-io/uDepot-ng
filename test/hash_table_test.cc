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
    EXPECT_EQ(e.raw(), 0u);
}

TEST(HashEntry, AtomicLoadStore) {
    std::atomic<uint64_t> slot{0};
    auto e = HashEntry::make(0, 42, 10, 1234);
    HashEntry::store(slot, e);
    auto loaded = HashEntry::load(slot);
    EXPECT_EQ(loaded, e);
}

TEST(HashEntry, MaxValues) {
    auto e = HashEntry::make(31, 255, 2047, (1ULL << 40) - 1);
    EXPECT_EQ(e.bucket_offset(), 31);
    EXPECT_EQ(e.key_tag(), 255);
    EXPECT_EQ(e.kv_size(), 2047);
    EXPECT_EQ(e.pba(), (1ULL << 40) - 1);
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
            while (!stop.load(std::memory_order_relaxed)) {
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
            }
            reads.fetch_add(local_reads, std::memory_order_relaxed);
        });
    }

    for (auto& w : writers) w.join();
    stop.store(true, std::memory_order_relaxed);
    for (auto& r : readers) r.join();

    EXPECT_EQ(writes_done.load(), kNumWriters);
    EXPECT_GT(reads.load(), 0u);
}

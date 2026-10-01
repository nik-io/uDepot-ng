// Copyright (c) 2024-2026 Nikolas Ioannou
// SPDX-License-Identifier: BSD-3-Clause

// GC and crash-ordering tests: the store has to keep working, and keep its
// answers, once writes exceed the device and segments get reclaimed.

#include "udepot/store.h"
#include "udepot/io/aio.h"
#include "udepot/io/posix.h"

#include <atomic>
#include <cerrno>
#include <cstdint>
#include <filesystem>
#include <map>
#include <string>
#include <thread>
#include <vector>

#include <gtest/gtest.h>

using udepot::AioIO;
using udepot::PosixIO;
using udepot::StoreConfig;
using udepot::UDepot;

namespace {

constexpr size_t kStoreSize = 32 * 1024 * 1024 + 4096;

StoreConfig gc_config(const std::filesystem::path& path) {
    StoreConfig config;
    config.path = path.c_str();
    config.size = kStoreSize;
    config.grain_size = 512;
    config.initial_tables = 4;
    config.index_bits = 12;
    config.segment_size = 2048;  // 1 MiB segments: GC kicks in quickly
    config.force_destroy = true;
    return config;
}

std::string value_for(int key, int round, size_t len) {
    std::string v = "k" + std::to_string(key) + "_r" + std::to_string(round) + "_";
    v.resize(len, static_cast<char>('a' + (key + round) % 26));
    return v;
}

template <typename Store>
std::string get_or_empty(Store& store, const std::string& key) {
    std::string out(16384, '\0');
    size_t n = 0;
    int rc = store.get(key, reinterpret_cast<uint8_t*>(out.data()),
                       out.size(), &n).run_sync();
    if (rc != 0) return rc == -ENOENT ? std::string() : "<error>";
    out.resize(n);
    return out;
}

class StoreGcTest : public ::testing::Test {
protected:
    void SetUp() override {
        path_ = std::filesystem::temp_directory_path() /
                ("udepot_store_gc_test_" + std::to_string(getpid()));
        config_ = gc_config(path_);
    }
    void TearDown() override {
        store_.close();
        std::filesystem::remove(path_);
        std::filesystem::remove(snapshot_path());
    }
    std::filesystem::path snapshot_path() const {
        return path_.string() + ".snapshot";
    }
    void reopen() {
        store_.close();
        config_.force_destroy = false;
        ASSERT_EQ(store_.open(config_), 0);
    }

    std::filesystem::path path_;
    StoreConfig config_;
    UDepot<PosixIO> store_;
};

}  // namespace

// Regression: nothing called salsa's release_grains, so no segment was ever
// sealed and GC never ran; once the device had been written through, puts
// failed for lack of space even though almost all of it was garbage.
TEST_F(StoreGcTest, OverwritesBeyondDeviceSizeKeepWorking) {
    ASSERT_EQ(store_.open(config_), 0);
    constexpr int kKeys = 200;
    constexpr size_t kVal = 4000;
    // ~8x the device: impossible without reclaiming segments.
    const int rounds = static_cast<int>(8 * kStoreSize / (kKeys * kVal));
    for (int r = 0; r < rounds; ++r) {
        for (int k = 0; k < kKeys; ++k) {
            ASSERT_EQ(store_.put("key" + std::to_string(k),
                                 value_for(k, r, kVal)).run_sync(), 0)
                << "round " << r << " key " << k;
        }
    }
    for (int k = 0; k < kKeys; ++k)
        ASSERT_EQ(get_or_empty(store_, "key" + std::to_string(k)),
                  value_for(k, rounds - 1, kVal)) << k;

    // Relocated records must recover too.
    reopen();
    for (int k = 0; k < kKeys; ++k)
        ASSERT_EQ(get_or_empty(store_, "key" + std::to_string(k)),
                  value_for(k, rounds - 1, kVal)) << k;
}

// Deleted keys must stay deleted after GC has gone through every segment
// and the store recovers: their tombstones may only be dropped once no
// older copy of the key can remain on the device.
TEST_F(StoreGcTest, DeletesSurviveGcAndRecovery) {
    ASSERT_EQ(store_.open(config_), 0);
    constexpr int kKeys = 300;
    constexpr size_t kVal = 3000;
    for (int k = 0; k < kKeys; ++k)
        ASSERT_EQ(store_.put("d" + std::to_string(k),
                             value_for(k, 0, kVal)).run_sync(), 0);
    for (int k = 0; k < kKeys; k += 2)
        ASSERT_EQ(store_.del("d" + std::to_string(k)).run_sync(), 0);

    // Churn other keys until the device has been rewritten several times.
    const int rounds = static_cast<int>(6 * kStoreSize / (50 * kVal));
    for (int r = 0; r < rounds; ++r)
        for (int c = 0; c < 50; ++c)
            ASSERT_EQ(store_.put("churn" + std::to_string(c),
                                 value_for(c, r, kVal)).run_sync(), 0);

    auto check = [&] {
        for (int k = 0; k < kKeys; ++k) {
            std::string got = get_or_empty(store_, "d" + std::to_string(k));
            if (k % 2 == 0)
                ASSERT_EQ(got, "") << "deleted key back: " << k;
            else
                ASSERT_EQ(got, value_for(k, 0, kVal)) << k;
        }
    };
    check();
    reopen();
    check();
}

// Regression: put() did not order itself against a tombstone. A put whose
// data was allocated before a concurrent delete's tombstone, but which
// committed after it, was acknowledged as the key's value, yet recovery
// kept the newer tombstone and the key vanished on reopen.
TEST_F(StoreGcTest, PutRacingDeleteRecoversAsAcknowledged) {
    ASSERT_EQ(store_.open(config_), 0);
    constexpr int kKeys = 400;
    std::map<int, std::string> expect;
    for (int k = 0; k < kKeys; ++k) {
        std::string key = "race" + std::to_string(k);
        ASSERT_EQ(store_.put(key, "old").run_sync(), 0);
        std::atomic<int> go{0};
        std::thread deleter([&] {
            go.fetch_add(1);
            while (go.load() < 2) {}
            (void)store_.del(key).run_sync();
        });
        go.fetch_add(1);
        while (go.load() < 2) {}
        ASSERT_EQ(store_.put(key, "new" + std::to_string(k)).run_sync(), 0);
        deleter.join();
        expect[k] = get_or_empty(store_, key);
    }
    reopen();
    int changed = 0;
    for (auto& [k, v] : expect)
        if (get_or_empty(store_, "race" + std::to_string(k)) != v) ++changed;
    EXPECT_EQ(changed, 0) << "keys whose state changed across recovery";
}

// Acknowledged writes must be recoverable from the device as it is at that
// moment, without close(): segment metadata has to be on the device before
// any write into the segment is acknowledged.
TEST_F(StoreGcTest, CrashSnapshotRecoversAcknowledgedWrites) {
    ASSERT_EQ(store_.open(config_), 0);
    constexpr int kThreads = 4;
    constexpr int kPerThread = 300;
    std::vector<std::thread> threads;
    for (int t = 0; t < kThreads; ++t) {
        threads.emplace_back([&, t] {
            for (int i = 0; i < kPerThread; ++i)
                ASSERT_EQ(store_.put("c" + std::to_string(t) + "_" +
                                         std::to_string(i),
                                     value_for(i, t, 1500)).run_sync(), 0);
        });
    }
    for (auto& th : threads) th.join();

    // "Crash": copy the device while the store is still open.
    std::filesystem::copy_file(path_, snapshot_path(),
                               std::filesystem::copy_options::overwrite_existing);
    UDepot<PosixIO> recovered;
    StoreConfig cfg = config_;
    std::string snap = snapshot_path().string();
    cfg.path = snap.c_str();
    cfg.force_destroy = false;
    ASSERT_EQ(recovered.open(cfg), 0);
    int missing = 0;
    for (int t = 0; t < kThreads; ++t)
        for (int i = 0; i < kPerThread; ++i)
            if (get_or_empty(recovered, "c" + std::to_string(t) + "_" +
                                            std::to_string(i)) !=
                value_for(i, t, 1500))
                ++missing;
    recovered.close();
    EXPECT_EQ(missing, 0);
}

// Readers must never see another key's bytes or a torn value while GC
// relocates records and reuses the segments they came from.
TEST(StoreGcAioTest, ReadsStayCorrectWhileGcRelocates) {
    auto path = std::filesystem::temp_directory_path() /
                ("udepot_store_gc_aio_" + std::to_string(getpid()));
    StoreConfig config = gc_config(path);
    UDepot<AioIO> store;
    ASSERT_EQ(store.open(config), 0);

    constexpr int kKeys = 100;
    constexpr size_t kVal = 4000;
    for (int k = 0; k < kKeys; ++k)
        ASSERT_EQ(store.put("r" + std::to_string(k), value_for(k, 0, kVal))
                      .run_sync(), 0);

    std::atomic<bool> stop{false};
    std::atomic<int> bad_reads{0};
    std::atomic<int> reads{0};
    std::vector<std::thread> readers;
    for (int t = 0; t < 3; ++t) {
        readers.emplace_back([&, t] {
            int k = t;
            do {
                std::string key = "r" + std::to_string(k % kKeys);
                std::string got = get_or_empty(store, key);
                // Any round's value of this key is fine; anything else is not.
                std::string prefix = "k" + std::to_string(k % kKeys) + "_r";
                if (got.size() != kVal || got.rfind(prefix, 0) != 0)
                    bad_reads.fetch_add(1);
                reads.fetch_add(1);
                ++k;
            } while (!stop.load());
        });
    }
    // Keys that are never overwritten get relocated by GC while read.
    const int rounds = static_cast<int>(5 * kStoreSize / (50 * kVal));
    for (int r = 0; r < rounds; ++r)
        for (int c = 0; c < 50; ++c)
            ASSERT_EQ(store.put("w" + std::to_string(c),
                                value_for(c, r, kVal)).run_sync(), 0);
    stop.store(true);
    for (auto& th : readers) th.join();

    EXPECT_GT(reads.load(), 0);
    EXPECT_EQ(bad_reads.load(), 0);
    store.close();
    std::filesystem::remove(path);
}

// Regression: a full neighborhood failed the put with ENOSPC; uDepot grows
// the directory instead. Writers racing the grow must not lose entries.
TEST_F(StoreGcTest, DirectoryGrowsUnderConcurrentPuts) {
    config_.initial_tables = 1;
    config_.index_bits = 6;  // 64 buckets: full after a few hundred keys
    ASSERT_EQ(store_.open(config_), 0);
    constexpr int kThreads = 4;
    constexpr int kPerThread = 1500;
    std::atomic<int> errors{0};
    std::vector<std::thread> threads;
    for (int t = 0; t < kThreads; ++t) {
        threads.emplace_back([&, t] {
            for (int i = 0; i < kPerThread; ++i)
                if (store_.put("g" + std::to_string(t) + "_" + std::to_string(i),
                               value_for(i, t, 100)).run_sync() != 0)
                    errors.fetch_add(1);
        });
    }
    for (auto& th : threads) th.join();
    ASSERT_EQ(errors.load(), 0);
    EXPECT_GT(store_.directory().num_tables(), 1u);

    auto check = [&] {
        int missing = 0;
        for (int t = 0; t < kThreads; ++t)
            for (int i = 0; i < kPerThread; ++i)
                if (get_or_empty(store_, "g" + std::to_string(t) + "_" +
                                             std::to_string(i)) !=
                    value_for(i, t, 100))
                    ++missing;
        EXPECT_EQ(missing, 0);
    };
    check();
    reopen();
    check();
}

// Regression: recovery looked at only the first tag-matching entry. With
// another key of the same tag ahead of it in the neighborhood, a key's
// newer record was inserted as a second entry instead of replacing the
// older one, so the stale value could be read, and survived a delete.
TEST_F(StoreGcTest, RecoveryDoesNotDuplicateKeysSharingATag) {
    config_.initial_tables = 1;
    config_.index_bits = 4;
    ASSERT_EQ(store_.open(config_), 0);

    // Two keys with the same tag and bucket.
    auto tag_bucket = [&](const std::string& k) {
        uint64_t h = store_.hash_key(UDepot<PosixIO>::as_bytes(k));
        return (h >> 56) << 8 | (h & 0xF);
    };
    std::map<uint64_t, std::string> seen;
    std::string a, b;
    for (int i = 0; a.empty(); ++i) {
        std::string k = "tag" + std::to_string(i);
        auto [it, inserted] = seen.emplace(tag_bucket(k), k);
        if (!inserted) { a = it->second; b = k; }
    }

    ASSERT_EQ(store_.put(a, "a-value").run_sync(), 0);
    ASSERT_EQ(store_.put(b, "b-old").run_sync(), 0);
    ASSERT_EQ(store_.put(b, "b-new").run_sync(), 0);
    reopen();
    EXPECT_EQ(get_or_empty(store_, a), "a-value");
    EXPECT_EQ(get_or_empty(store_, b), "b-new");
    ASSERT_EQ(store_.del(b).run_sync(), 0);
    EXPECT_EQ(get_or_empty(store_, b), "") << "stale duplicate survived del";
}

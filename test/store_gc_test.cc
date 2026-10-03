// Copyright (c) 2024-2026 Nikolas Ioannou
// SPDX-License-Identifier: BSD-3-Clause

// GC and crash-ordering tests: the store has to keep working, and keep its
// answers, once writes exceed the device and segments get reclaimed.

#include "udepot/store.h"
#include "udepot/io/aio.h"
#include "udepot/io/posix.h"

#include <algorithm>
#include <atomic>
#include <mutex>
#include <deque>
#include <condition_variable>
#include <chrono>
#include <cerrno>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <random>
#include <map>
#include <set>
#include <string>
#include <thread>
#include <vector>

#include <gtest/gtest.h>

#include "record_checksum.h"

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

// A put that finds its table full while running on the AIO poller (where
// its coroutine resumes after the data write) must not grow the directory
// there: the grow waits for a grace period, which reads in flight on that
// same poller hold. It suspends, the waker thread grows, and it retries.
TEST(StoreGcAioTest, DirectoryGrowsFromPollerThread) {
    auto path = std::filesystem::temp_directory_path() /
                ("udepot_store_grow_aio_" + std::to_string(getpid()));
    StoreConfig config = gc_config(path);
    config.initial_tables = 1;
    config.index_bits = 6;
    UDepot<AioIO> store;
    ASSERT_EQ(store.open(config), 0);

    constexpr int kThreads = 4;
    constexpr int kPerThread = 1024;
    constexpr int kBatch = 32;
    std::atomic<int> errors{0};
    std::vector<std::thread> threads;
    for (int t = 0; t < kThreads; ++t) {
        threads.emplace_back([&, t] {
            for (int base = 0; base < kPerThread; base += kBatch) {
                // In flight together, so commits run on the poller while
                // other puts' reads are outstanding.
                std::vector<std::string> keys, vals;
                for (int i = base; i < base + kBatch; ++i) {
                    keys.push_back("p" + std::to_string(t) + "_" +
                                   std::to_string(i));
                    vals.push_back(value_for(i, t, 100));
                }
                std::vector<udepot::CoroTask<int>> ops;
                for (int j = 0; j < kBatch; ++j)
                    ops.push_back(store.put(keys[j], vals[j]));
                for (auto& op : ops)
                    if (op.run_sync() != 0) errors.fetch_add(1);
            }
        });
    }
    for (auto& th : threads) th.join();
    EXPECT_EQ(errors.load(), 0);
    EXPECT_GT(store.directory().num_tables(), 1u);

    int missing = 0;
    for (int t = 0; t < kThreads; ++t)
        for (int i = 0; i < kPerThread; ++i)
            if (get_or_empty(store, "p" + std::to_string(t) + "_" +
                                        std::to_string(i)) !=
                value_for(i, t, 100))
                ++missing;
    EXPECT_EQ(missing, 0);
    store.close();
    std::filesystem::remove(path);
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

// Values may be empty (paper, section 4: "no minimum size"); a record with
// an empty value is not a tombstone. Recovery and GC used to treat
// val_size 0 as one: an empty value vanished on reopen, and GC could purge
// its live entry.
TEST_F(StoreGcTest, EmptyValuesAreNotTombstones) {
    ASSERT_EQ(store_.open(config_), 0);
    constexpr int kKeys = 50;
    auto check = [&] {
        for (int k = 0; k < kKeys; ++k) {
            uint8_t buf[8];
            size_t n = 99;
            ASSERT_EQ(store_.get("empty" + std::to_string(k), buf, sizeof(buf),
                                 &n).run_sync(), 0) << k;
            EXPECT_EQ(n, 0u) << k;
        }
    };
    for (int k = 0; k < kKeys; ++k)
        ASSERT_EQ(store_.put("empty" + std::to_string(k), "").run_sync(), 0);
    check();

    // Rewrite the device several times so GC passes over every segment,
    // with tombstone dropping allowed.
    constexpr size_t kVal = 3000;
    const int rounds = static_cast<int>(6 * kStoreSize / (50 * kVal));
    for (int r = 0; r < rounds; ++r)
        for (int c = 0; c < 50; ++c)
            ASSERT_EQ(store_.put("churn" + std::to_string(c),
                                 value_for(c, r, kVal)).run_sync(), 0);
    check();
    reopen();
    check();
}

TEST_F(StoreGcTest, TombstoneValueSizeIsReserved) {
    ASSERT_EQ(store_.open(config_), 0);
    EXPECT_FALSE(store_.alloc_put_buffer(4, udepot::kTombstoneValSize).valid());
    EXPECT_TRUE(store_.alloc_put_buffer(4, 0).valid());
}

// GC relocating a record while a put of the same key commits: the
// relocated copy can be newer in recovery order than the acknowledged put
// (when GC's relocation segment was opened after the put's), so if the
// copy is left on the device the log scan after a crash brings back the
// older value. GC now holds the key's stripes across the whole relocation,
// as uDepot does, so the put waits and commits after it.
//
// The test seam races every relocation: GC hands the key being moved to a
// racer thread, which overwrites it while the relocation is in flight.
TEST_F(StoreGcTest, GcRelocationRacingPutRecoversAsAcknowledged) {
    ASSERT_EQ(store_.open(config_), 0);
    // ~18 MiB long-lived plus ~3 MiB hot, of ~25 MiB usable: GC cannot
    // get by on fully dead segments and has to move live records.
    constexpr int kKeys = 9000;
    // Enough hot keys that segments do not die whole: GC has to pick
    // partly live ones and relocate.
    constexpr int kHot = 1500;
    constexpr size_t kVal = 2000;
    constexpr int kChurn = 40000;
    constexpr int kRaces = 500;

    static std::mutex mu;
    static std::condition_variable cv;
    static std::deque<std::string> to_race;
    static std::atomic<int> raced{0};
    to_race.clear();
    raced = 0;
    static std::atomic<int> hooked{0};
    hooked = 0;
    UDepot<PosixIO>::gc_relocation_test_hook = [](std::span<const uint8_t> k) {
        hooked.fetch_add(1);
        {
            std::lock_guard<std::mutex> lock(mu);
            to_race.emplace_back(reinterpret_cast<const char*>(k.data()),
                                 k.size());
        }
        cv.notify_one();
        // Give the racer time to land its put inside the window. Never
        // wait for it: with the fix, that put waits for this relocation.
        std::this_thread::sleep_for(std::chrono::microseconds(300));
    };
    struct ResetHook {
        ~ResetHook() { UDepot<PosixIO>::gc_relocation_test_hook = nullptr; }
    } reset_hook;

    // Interleaved, so every segment mixes long-lived records with ones
    // about to die: GC victims then hold live data to move.
    std::map<std::string, std::string> expect;
    for (int k = 0; k < kKeys; ++k) {
        std::string key = "g" + std::to_string(k);
        ASSERT_EQ(store_.put(key, value_for(k, 0, kVal)).run_sync(), 0);
        expect[key] = value_for(k, 0, kVal);
        ASSERT_EQ(store_.put("h" + std::to_string(k % kHot),
                             value_for(k, 0, kVal)).run_sync(), 0);
    }

    {
        std::lock_guard<std::mutex> lock(mu);
        to_race.clear();  // relocations from the fill phase: not raced
    }
    // Racer: overwrites each key GC is relocating. It is the only writer of
    // the long-lived keys from here on, so `expect` stays exact.
    std::atomic<bool> stop{false};
    std::atomic<int> errors{0};
    std::thread racer([&] {
        int n = 0;
        for (;;) {
            std::string key;
            {
                std::unique_lock<std::mutex> lock(mu);
                cv.wait_for(lock, std::chrono::milliseconds(5), [] {
                    return !to_race.empty();
                });
                if (to_race.empty()) {
                    if (stop) return;
                    continue;
                }
                key = std::move(to_race.front());
                to_race.pop_front();
            }
            if (key[0] != 'g') continue;  // hot keys need no tracking
            std::string val = value_for(++n, 1, kVal);
            if (store_.put(key, val).run_sync() == 0) {
                expect[key] = val;
                raced.fetch_add(1);
            } else {
                errors.fetch_add(1);
            }
        }
    });
    // Churn the hot keys so GC keeps reclaiming the mixed segments, until
    // enough relocations have been raced. Stopping early matters: GC only
    // runs under allocation pressure, so once the churn stops it leaves
    // the device alone, and a stale relocated copy is still there for the
    // snapshot to catch rather than reclaimed first.
    for (int i = 0; i < kChurn && raced.load() < kRaces; ++i)
        ASSERT_EQ(store_.put("h" + std::to_string(i % kHot),
                             value_for(i, 2, kVal)).run_sync(), 0);
    stop = true;
    racer.join();
    UDepot<PosixIO>::gc_relocation_test_hook = nullptr;
    ASSERT_EQ(errors.load(), 0);
    ASSERT_GT(raced.load(), 0) << "no relocation was raced; hooked="
                               << hooked.load();

    // "Crash": the log scan must reproduce exactly what was acknowledged.
    std::filesystem::copy_file(path_, snapshot_path(),
                               std::filesystem::copy_options::overwrite_existing);
    UDepot<PosixIO> recovered;
    StoreConfig cfg = config_;
    std::string snap = snapshot_path().string();
    cfg.path = snap.c_str();
    cfg.force_destroy = false;
    ASSERT_EQ(recovered.open(cfg), 0);
    int wrong = 0;
    for (const auto& [key, val] : expect)
        if (get_or_empty(recovered, key) != val) ++wrong;
    recovered.close();
    EXPECT_EQ(wrong, 0) << "of " << raced.load() << " raced relocations";
}

// Regression (PR #3 review, finding 5): salsa recycled a segment the moment
// its last valid grain was invalidated, with no grace period. A get that had
// looked a record up there just before it was overwritten could then read
// the segment's next contents, and report a key that was there all along as
// missing.
TEST_F(StoreGcTest, GetSurvivesItsSegmentBeingRecycled) {
    ASSERT_EQ(store_.open(config_), 0);
    constexpr int kKeys = 50;
    constexpr size_t kVal = 4000;
    constexpr int kReads = 8;
    for (int k = 0; k < kKeys; ++k)
        ASSERT_EQ(store_.put("w" + std::to_string(k), value_for(k, 0, kVal))
                      .run_sync(), 0);

    static std::atomic<bool> armed{false};
    static std::atomic<uint64_t> puts{0};
    armed = false;
    puts = 0;
    UDepot<PosixIO>::get_read_test_hook = [](std::span<const uint8_t>) {
        if (!armed.exchange(false)) return;
        // Hold the read until the writer has wrapped around the device
        // twice, so the record's segment is overwritten if it was freed, or
        // until the writer stalls: with the fix, every segment it could
        // reuse waits for this read's grace period.
        using Clock = std::chrono::steady_clock;
        constexpr uint64_t kWraps = 2 * kStoreSize / kVal;
        const uint64_t target = puts.load() + kWraps;
        uint64_t last = puts.load();
        auto progress = Clock::now();
        while (last < target) {
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
            uint64_t now = puts.load();
            if (now != last) {
                last = now;
                progress = Clock::now();
            } else if (Clock::now() - progress >
                       std::chrono::milliseconds(50)) {
                break;
            }
        }
    };
    struct ResetHook {
        ~ResetHook() { UDepot<PosixIO>::get_read_test_hook = nullptr; }
    } reset_hook;

    // Every key is rewritten each round, so whole segments go dead and are
    // recycled without GC relocating anything.
    std::atomic<bool> stop{false};
    std::atomic<int> put_errors{0};
    std::thread writer([&] {
        for (int r = 1; !stop.load(); ++r) {
            for (int k = 0; k < kKeys && !stop.load(); ++k) {
                if (store_.put("w" + std::to_string(k),
                               value_for(k, r, kVal)).run_sync() != 0)
                    put_errors.fetch_add(1);
                puts.fetch_add(1);
            }
        }
    });

    int bad = 0;
    for (int i = 0; i < kReads; ++i) {
        int k = (i * 7) % kKeys;
        armed = true;
        std::string got = get_or_empty(store_, "w" + std::to_string(k));
        // Any round's value of this key is fine; anything else is not.
        std::string prefix = "k" + std::to_string(k) + "_r";
        if (got.size() != kVal || got.rfind(prefix, 0) != 0) ++bad;
    }
    stop = true;
    writer.join();
    UDepot<PosixIO>::get_read_test_hook = nullptr;
    EXPECT_EQ(put_errors.load(), 0);
    EXPECT_EQ(bad, 0) << "of " << kReads << " reads held across a recycle";
}

// Recovery orders a key's records by segment timestamp, so a relocated copy
// must land in a segment newer than the one it came from: all older copies
// of the key are older than that. The relocation segment stays open while
// the data stream moves on, and in this workload (cold data interleaved
// with hot overwrites) GC moved records into an older one (PR #3 review,
// finding 6). Any key with an older copy on disk in a
// segment between the two would then come back stale after a crash.
TEST_F(StoreGcTest, RelocationNeverMovesIntoAnOlderSegment) {
    static std::atomic<int> relocations{0};
    static std::atomic<int> into_older{0};
    relocations = 0;
    into_older = 0;
    UDepot<PosixIO>::gc_relocation_order_test_hook =
        [](std::span<const uint8_t>, uint64_t victim_ts, uint64_t dst_ts) {
            relocations.fetch_add(1);
            if (dst_ts <= victim_ts) into_older.fetch_add(1);
        };
    struct ResetHook {
        ~ResetHook() {
            UDepot<PosixIO>::gc_relocation_order_test_hook = nullptr;
        }
    } reset_hook;

    ASSERT_EQ(store_.open(config_), 0);
    const std::string val(3000, 'v');
    constexpr int kCold = 5000;
    constexpr int kHot = 200;
    for (int i = 0; i < kCold; ++i) {
        ASSERT_EQ(store_.put("c" + std::to_string(i), val).run_sync(), 0);
        for (int h = 0; h < 3; ++h)
            ASSERT_EQ(store_.put("h" + std::to_string((i * 3 + h) % kHot), val)
                          .run_sync(), 0);
    }
    // Long enough that, without the fix, every run saw at least one (6 of
    // 6; a sixth of this caught a third of runs).
    for (int i = 0; i < 120000; ++i)
        ASSERT_EQ(store_.put("h" + std::to_string(i % kHot), val).run_sync(),
                  0);
    store_.close();
    UDepot<PosixIO>::gc_relocation_order_test_hook = nullptr;

    EXPECT_GT(relocations.load(), 0);
    EXPECT_EQ(into_older.load(), 0) << "of " << relocations.load()
                                    << " relocations";
}

// Whether the store file holds, in a segment older than ts, a record of key
// that crash recovery would replay: one whose checksum binds it to its
// segment's timestamp and the device seed. Reads the file directly, so it
// does not trust the store's own bookkeeping.
static bool older_copy_on_disk(const std::filesystem::path& path,
                               std::span<const uint8_t> key, uint64_t ts,
                               uint64_t seed) {
    constexpr size_t kGrain = 512;
    constexpr size_t kSegGrains = 2048;  // gc_config's segment_size
    constexpr size_t kSegBytes = kGrain * kSegGrains;
    std::ifstream in(path, std::ios::binary);
    std::vector<char> seg(kSegBytes);
    while (in.read(seg.data(), kSegBytes)) {
        const char* md = seg.data() + kSegBytes - kGrain;  // salsa_seg_md
        uint64_t seg_size, grain_size, seg_ts;
        std::memcpy(&seg_size, md, 8);
        std::memcpy(&grain_size, md + 8, 8);
        std::memcpy(&seg_ts, md + 16, 8);
        if (seg_size != kSegGrains || grain_size != kGrain || seg_ts == 0 ||
            seg_ts >= ts)
            continue;
        for (size_t g = 0; g + 1 < kSegGrains; ++g) {
            const char* rec = seg.data() + g * kGrain;
            udepot::KvHeader hdr;
            std::memcpy(&hdr, rec, sizeof(hdr));
            if (hdr.key_size != key.size() || udepot::is_tombstone(hdr) ||
                std::memcmp(rec + sizeof(hdr), key.data(), key.size()) != 0)
                continue;
            const size_t crc_at = sizeof(hdr) + hdr.key_size + hdr.val_size;
            if (g * kGrain + crc_at + 2 > kSegBytes - kGrain) continue;
            uint16_t crc;
            std::memcpy(&crc, rec + crc_at, sizeof(crc));
            if (crc == record_checksum(reinterpret_cast<const uint8_t*>(rec),
                                       seg_ts, seed))
                return true;
        }
    }
    return false;
}

// Regression (PR #3 review, finding 8): GC let a tombstone go once every
// older segment had been reclaimed, but a reclaimed segment keeps its
// metadata and old records on disk until it is reused, and crash recovery
// replays them: a deleted key could come back. In this workload (random
// puts and deletes under GC pressure) about a fifth of the tombstones GC
// dropped had an older copy of their key still on disk.
TEST_F(StoreGcTest, TombstoneIsDroppedOnlyWhenNoOlderCopyIsOnDisk) {
    static std::filesystem::path path;
    static std::atomic<int> drops{0};
    static std::atomic<int> unsafe{0};
    static std::atomic<uint64_t> seed{0};
    path = path_;
    drops = 0;
    unsafe = 0;
    UDepot<PosixIO>::gc_tombstone_drop_test_hook =
        [](std::span<const uint8_t> key, uint64_t victim_ts) {
            drops.fetch_add(1);
            if (older_copy_on_disk(path, key, victim_ts, seed.load()))
                unsafe.fetch_add(1);
        };
    struct ResetHook {
        ~ResetHook() {
            UDepot<PosixIO>::gc_tombstone_drop_test_hook = nullptr;
        }
    } reset_hook;

    ASSERT_EQ(store_.open(config_), 0);
    seed = store_.seed();
    std::mt19937 rng(1);
    const std::string val(2500, 'v');
    constexpr int kKeys = 6000;
    for (int i = 0; i < 60000; ++i) {
        std::string key = "k" + std::to_string(rng() % kKeys);
        if (rng() % 100 < 30) {
            int rc = store_.del(key).run_sync();
            ASSERT_TRUE(rc == 0 || rc == -ENOENT) << rc;
        } else {
            ASSERT_EQ(store_.put(key, val).run_sync(), 0);
        }
    }
    store_.close();
    UDepot<PosixIO>::gc_tombstone_drop_test_hook = nullptr;

    // Tombstones must still be reclaimed, or they would take space forever.
    EXPECT_GT(drops.load(), 0);
    EXPECT_EQ(unsafe.load(), 0) << "of " << drops.load() << " dropped";
}

// Regression (PR #3 review, finding 10): a segment's timestamp was read
// from the allocation counter after the increment, so when the user and
// the relocation stream staged segments at once, both could read the same
// value. Recovery orders a key's records by segment timestamp; with a tie
// it cannot tell which is newer. The test widens the window; this workload
// (puts and deletes under GC, so both streams stage segments) then gave
// duplicates in every run.
// Regression (PR #3 review, finding 13): a version was the record's pba,
// which comes back once its segment is reused, so a conditional put or del
// holding a version read earlier could act on a different value (ABA).
// Overwrites one key until its records land on places they used before.
TEST_F(StoreGcTest, VersionNeverRepeatsWhenItsPlaceIsReused) {
    ASSERT_EQ(store_.open(config_), 0);
    const std::string key = "versioned";
    const uint64_t hash = store_.hash_key(std::span<const uint8_t>(
        reinterpret_cast<const uint8_t*>(key.data()), key.size()));
    std::set<uint64_t> versions;
    std::map<uint64_t, uint64_t> version_at;  // pba -> its latest version
    int reused = 0;
    uint64_t previous = udepot::kAnyVersion;
    for (int round = 0; round < 40000 && reused < 50; ++round) {
        ASSERT_EQ(store_.put(key, value_for(0, round, 3000)).run_sync(), 0);
        uint8_t buf[4096];
        size_t n = 0;
        uint64_t version = 0;
        ASSERT_EQ(store_.get(key, buf, sizeof(buf), &n, &version).run_sync(),
                  0);
        uint32_t idx = store_.rcu().read_lock();
        const uint64_t pba = store_.directory().lookup(hash).pba();
        store_.rcu().read_unlock(idx);

        EXPECT_TRUE(versions.insert(version).second)
            << "version " << version << " repeated at round " << round;
        auto [it, fresh] = version_at.emplace(pba, version);
        if (!fresh) {
            ++reused;
            it->second = version;
        }
        // The version read before this round's put no longer applies.
        if (previous != udepot::kAnyVersion) {
            EXPECT_EQ(store_.put(key, "stale", udepot::PutMode::kUpsert,
                                 previous).run_sync(), -ESTALE);
        }
        previous = version;
    }
    ASSERT_GT(reused, 0) << "no record landed on a reused place; churn more";
}

TEST_F(StoreGcTest, SegmentTimestampsAreUnique) {
    static std::mutex mu;
    static std::vector<uint64_t> stamps;
    stamps.clear();
    UDepot<PosixIO>::seg_md_test_hook = [](uint64_t ts) {
        std::lock_guard<std::mutex> lock(mu);
        stamps.push_back(ts);
    };
    // Widen the window between the counter's increment and the stamp, so
    // the two streams' stagings overlap.
    UDepot<PosixIO>::seg_md_enter_test_hook = [] {
        std::this_thread::sleep_for(std::chrono::milliseconds(2));
    };
    struct ResetHook {
        ~ResetHook() {
            UDepot<PosixIO>::seg_md_test_hook = nullptr;
            UDepot<PosixIO>::seg_md_enter_test_hook = nullptr;
        }
    } reset_hook;

    ASSERT_EQ(store_.open(config_), 0);
    std::mt19937 rng(2);
    const std::string val(2500, 'v');
    constexpr int kKeys = 4000;
    for (int i = 0; i < 60000; ++i) {
        std::string key = "k" + std::to_string(rng() % kKeys);
        if (rng() % 100 < 50) {
            int rc = store_.del(key).run_sync();
            ASSERT_TRUE(rc == 0 || rc == -ENOENT) << rc;
        } else {
            ASSERT_EQ(store_.put(key, val).run_sync(), 0);
        }
    }
    store_.close();
    UDepot<PosixIO>::seg_md_test_hook = nullptr;
    UDepot<PosixIO>::seg_md_enter_test_hook = nullptr;

    std::lock_guard<std::mutex> lock(mu);
    std::vector<uint64_t> sorted = stamps;
    std::sort(sorted.begin(), sorted.end());
    int dups = 0;
    for (size_t i = 1; i < sorted.size(); ++i)
        if (sorted[i] == sorted[i - 1]) ++dups;
    EXPECT_GT(sorted.size(), 64u);
    EXPECT_EQ(dups, 0) << "of " << sorted.size() << " segments";
}

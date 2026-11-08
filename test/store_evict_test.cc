// Copyright (c) 2024-2026 Nikolas Ioannou
// SPDX-License-Identifier: BSD-3-Clause

// A memcache store (StoreConfig::memcache, uDepot's *MC runtimes): GC evicts
// the entries of the segments it cleans, oldest segment first, instead of
// relocating them, and a store that was not shut down cleanly opens empty.

#include "udepot/hash_table.h"
#include "udepot/io/posix.h"
#include "udepot/store.h"

#include <atomic>
#include <cerrno>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <cstdio>
#include <ctime>
#include <filesystem>
#include <memory>
#include <mutex>
#include <set>
#include <span>
#include <string>
#include <thread>
#include <vector>

#include <gtest/gtest.h>

using udepot::PosixIO;
using udepot::StoreConfig;
using udepot::uDepot;

namespace {

using Store = uDepot<PosixIO>;

// 140 segments of 128 KiB: live data is capped at the device less 10
// segments, GC runs from 20 free segments to 40, so the device has to be well
// over 40 segments for GC to rest between runs. (The large-device test uses
// bigger ones.)
constexpr uint64_t kSegGrains = 256;
constexpr uint64_t kSegs = 140;
constexpr uint32_t kGrain = 512;
constexpr size_t kStoreSize = kSegs * kSegGrains * kGrain + 4096;
constexpr size_t kVal = 3000;
// Grains of a record: header, key, value and suffix (the keys are short),
// and how many fit a segment's data grains.
constexpr uint64_t kRecGrains = (kVal + 64 + kGrain - 1) / kGrain;
constexpr uint64_t kRecsPerSeg = (kSegGrains - 1) / kRecGrains;
// GC starts at 20 free segments and stops at 40, and idles in between, so
// when it is idle all but 20 of the segments can hold records (each
// segment, on average, three new keys in four; the others are overwrites of
// hot keys, dead but for the last of each).
constexpr uint64_t kMaxUniqueSurvivors = (kSegs - 20) * kRecsPerSeg * 3 / 4;
constexpr int kHotKeys = 50;

std::string value_for(int key, int round, size_t len = kVal) {
    std::string v = "k" + std::to_string(key) + "_r" +
                    std::to_string(round) + "_";
    v.resize(len, static_cast<char>('a' + (key + round) % 26));
    return v;
}

// "" for not found, "<error>" for any other failure.
std::string get_or_empty(Store& store, const std::string& key) {
    std::string out(16384, '\0');
    size_t n = 0;
    int rc = store.get(key, reinterpret_cast<uint8_t*>(out.data()),
                       out.size(), &n).run_sync();
    if (rc != 0) return rc == -ENOENT ? std::string() : "<error>";
    out.resize(n);
    return out;
}

struct Entries {
    uint64_t live = 0;
    uint64_t deleted = 0;
    bool operator==(const Entries&) const = default;
};

Entries count_entries(Store& store) {
    Entries e;
    for (const auto& t : store.directory().snapshot().tables)
        for (uint64_t i = 0; i < t->total_slots(); ++i) {
            const auto slot = t->load_slot(i);
            if (slot.empty()) continue;
            (slot.deleted() ? e.deleted : e.live)++;
        }
    return e;
}

// GC runs on its own thread: wait until it has stopped changing the table.
Entries quiesce(Store& store) {
    Entries last = count_entries(store);
    int stable = 0;
    for (int i = 0; i < 200 && stable < 5; ++i) {
        std::this_thread::sleep_for(std::chrono::milliseconds(50));
        Entries now = count_entries(store);
        stable = now == last ? stable + 1 : 0;
        last = now;
    }
    EXPECT_GE(stable, 5) << "GC did not settle";
    return last;
}

// A put that finds no space waits for it, with no limit (the space waker
// retries); a GC that frees nothing would hang the test rather than fail it.
// Ends the process with a message instead.
class Watchdog {
public:
    explicit Watchdog(std::chrono::seconds limit)
        : thread_([this, limit] {
              std::unique_lock<std::mutex> lock(mu_);
              if (!cv_.wait_for(lock, limit, [this] { return done_; })) {
                  std::fprintf(stderr, "FAIL: store_evict_test hung for %lld s "
                               "(a put waiting for space that GC never "
                               "frees?)\n",
                               static_cast<long long>(limit.count()));
                  std::_Exit(1);
              }
          }) {}
    ~Watchdog() {
        {
            std::lock_guard<std::mutex> lock(mu_);
            done_ = true;
        }
        cv_.notify_one();
        thread_.join();
    }

private:
    std::mutex mu_;
    std::condition_variable cv_;
    bool done_ = false;
    std::thread thread_;
};

std::atomic<int> g_relocations{0};
void count_relocation(std::span<const uint8_t>) { ++g_relocations; }

class StoreEvictTest : public ::testing::Test {
protected:
    void SetUp() override {
        path_ = std::filesystem::temp_directory_path() /
                ("udepot_store_evict_test_" + std::to_string(getpid()));
        config_.path = path_.c_str();
        config_.size = kStoreSize;
        config_.grain_size = kGrain;
        config_.segment_size = kSegGrains;
        config_.force_destroy = true;
        config_.memcache = true;
        g_relocations = 0;
        Store::gc_relocation_test_hook.store(count_relocation);
        watchdog_ = std::make_unique<Watchdog>(std::chrono::seconds(90));
    }
    void TearDown() override {
        Store::gc_relocation_test_hook.store(nullptr);
        Store::index_gc_test_hook.store(nullptr);
        Store::index_footer_test_hook.store(nullptr);
        Store::index_format_test_hook.store(nullptr);
        store_.close();
        std::filesystem::remove(path_);
        std::filesystem::remove(snapshot_path());
        watchdog_.reset();  // armed across close() too
    }
    std::filesystem::path snapshot_path() const {
        return path_.string() + ".snapshot";
    }

    // open() with stderr captured into log_. try_open() leaves the result
    // to the caller (a test of a refused open); open() asserts it, so a
    // failed open ends the test instead of going on with an unopened store.
    int try_open(Store& store, const StoreConfig& config) {
        ::testing::internal::CaptureStderr();
        const int rc = store.open(config);
        log_ = ::testing::internal::GetCapturedStderr();
        return rc;
    }
    void open(Store& store, const StoreConfig& config) {
        ASSERT_EQ(try_open(store, config), 0) << log_;
    }
    void reopen() {
        store_.close();
        config_.force_destroy = false;
        open(store_, config_);
    }

    // A "crash": the device as it is now, store still open, opened by a
    // second store. Returns what that open printed.
    void open_crashed_copy(Store& recovered, bool memcache) {
        std::filesystem::copy_file(
            path_, snapshot_path(),
            std::filesystem::copy_options::overwrite_existing);
        snap_ = snapshot_path().string();
        StoreConfig cfg = config_;
        cfg.path = snap_.c_str();
        cfg.force_destroy = false;
        cfg.memcache = memcache;
        open(recovered, cfg);
    }

    // Puts far beyond the device: three new keys and one overwrite of a hot
    // key, in turn. Every put must succeed, whatever GC has to do.
    void fill(uint64_t total_puts) {
        hot_last_.assign(kHotKeys, -1);
        unique_ = 0;
        for (uint64_t i = 0; i < total_puts; ++i) {
            if (i % 4 == 3) {
                const int j = static_cast<int>((i / 4) % kHotKeys);
                hot_last_[j] = static_cast<int>(i);
                ASSERT_EQ(store_.put("hot" + std::to_string(j),
                                     value_for(j, static_cast<int>(i)))
                              .run_sync(), 0) << "put " << i;
            } else {
                ASSERT_EQ(store_.put("u" + std::to_string(unique_),
                                     value_for(unique_, 0)).run_sync(), 0)
                    << "put " << i;
                ++unique_;
            }
        }
    }

    // Which of the keys fill() wrote can be read, with the right value (a
    // wrong value fails the test).
    struct Survivors {
        std::vector<bool> unique;
        std::vector<bool> hot;
        uint64_t count() const {
            uint64_t n = 0;
            for (bool b : unique) n += b;
            for (bool b : hot) n += b;
            return n;
        }
    };
    Survivors survivors() {
        Survivors s;
        for (int k = 0; k < unique_; ++k) {
            const std::string got =
                get_or_empty(store_, "u" + std::to_string(k));
            if (!got.empty()) {
                EXPECT_EQ(got, value_for(k, 0)) << "unique key " << k;
            }
            s.unique.push_back(!got.empty());
        }
        for (int j = 0; j < kHotKeys; ++j) {
            const std::string got =
                get_or_empty(store_, "hot" + std::to_string(j));
            if (!got.empty()) {
                EXPECT_EQ(got, value_for(j, hot_last_[j])) << "hot key " << j;
            }
            s.hot.push_back(!got.empty());
        }
        return s;
    }

    std::filesystem::path path_;
    std::string snap_;
    std::string log_;
    StoreConfig config_;
    Store store_;
    std::vector<int> hot_last_;
    int unique_ = 0;
    std::unique_ptr<Watchdog> watchdog_;
};

}  // namespace

// Six times the device in puts, overwrites and new keys: none fails, GC
// evicts the oldest segments (what survives is a suffix of what was
// written, the newest data all there), relocates nothing, and keeps the
// device's free segments at the circular GC's high watermark.
TEST_F(StoreEvictTest, FillsFarBeyondTheDeviceEvictingOldestFirst) {
    ASSERT_NO_FATAL_FAILURE(open(store_, config_));
    fill(6 * kSegs * kRecsPerSeg);
    ASSERT_FALSE(HasFatalFailure());
    quiesce(store_);

    EXPECT_EQ(g_relocations.load(), 0) << "GC relocated a record";
    const Survivors s = survivors();
    // Oldest first: no key is readable that was written before one that is
    // not.
    ASSERT_GT(unique_, 0);
    EXPECT_FALSE(s.unique.front()) << "the oldest key survived";
    EXPECT_TRUE(s.unique.back()) << "the newest key was evicted";
    int first_alive = 0;
    while (first_alive < unique_ && !s.unique[first_alive]) ++first_alive;
    for (int k = first_alive; k < unique_; ++k)
        ASSERT_TRUE(s.unique[k]) << "key " << k << " was evicted, but key "
                                 << first_alive << ", older, was not";
    // The hot keys are rewritten all the time: always in the newest segments.
    for (int j = 0; j < kHotKeys; ++j)
        EXPECT_TRUE(s.hot[j]) << "hot key " << j;

    // GC starts at 20 free segments and stops at 40: the device holds the
    // rest, between half of it and everything it can. (A GC that started
    // at 2 and stopped at 4, as a KV store's, would leave more.)
    uint64_t alive = 0;
    for (bool b : s.unique) alive += b;
    EXPECT_LE(alive, kMaxUniqueSurvivors);
    EXPECT_GE(alive, kSegs / 2 * kRecsPerSeg * 3 / 4);
}

// The directory holds exactly the entries that read back: eviction removes
// the entry of every record it drops, and a clean close and reopen restores
// the same ones from the persisted index (which rebuilds salsa's valid
// counts from them, and refuses an index that does not fit).
TEST_F(StoreEvictTest, EvictionKeepsTheDirectoryExactAcrossACleanReopen) {
    ASSERT_NO_FATAL_FAILURE(open(store_, config_));
    fill(6 * kSegs * kRecsPerSeg);
    ASSERT_FALSE(HasFatalFailure());
    const Entries before = quiesce(store_);
    const Survivors s = survivors();
    EXPECT_EQ(before.deleted, 0u);
    EXPECT_EQ(before.live, s.count()) << "entries left for evicted records";

    ASSERT_NO_FATAL_FAILURE(reopen());
    const std::string log = log_;
    EXPECT_TRUE(store_.index_restored()) << log;
    EXPECT_EQ(log.find("not shut down cleanly"), std::string::npos) << log;
    EXPECT_EQ(count_entries(store_), before);
    const Survivors again = survivors();
    EXPECT_EQ(again.unique, s.unique);
    EXPECT_EQ(again.hot, s.hot);
    // And it still takes writes, evicting on.
    ASSERT_EQ(store_.put("after", "reopen").run_sync(), 0);
    EXPECT_EQ(get_or_empty(store_, "after"), "reopen");
}

// A key deleted stays deleted when GC evicts its tombstone and the
// segment of the value it deleted is reused; and the tombstones that GC
// drops leave an index that restores.
TEST_F(StoreEvictTest, DeletedKeysStayDeletedThroughEviction) {
    ASSERT_NO_FATAL_FAILURE(open(store_, config_));
    std::vector<std::string> deleted;
    uint64_t tombstones_seen = 0;
    const uint64_t total = 6 * kSegs * kRecsPerSeg;
    for (uint64_t i = 0; i < total; ++i) {
        const std::string k = "f" + std::to_string(i);
        ASSERT_EQ(store_.put(k, value_for(static_cast<int>(i), 0)).run_sync(),
                  0) << "put " << i;
        if (i % 8 == 0) {
            ASSERT_EQ(store_.del(k).run_sync(), 0) << "del " << i;
            deleted.push_back(k);
        }
        if (i == total - 1) tombstones_seen = count_entries(store_).deleted;
    }
    EXPECT_GT(tombstones_seen, 0u) << "no tombstone in the newest segments";
    const Entries e = quiesce(store_);
    EXPECT_EQ(g_relocations.load(), 0);
    for (const std::string& k : deleted)
        ASSERT_EQ(get_or_empty(store_, k), "") << k << " is back";
    EXPECT_TRUE(get_or_empty(store_, "f" + std::to_string(total - 1)) ==
                value_for(static_cast<int>(total - 1), 0));

    ASSERT_NO_FATAL_FAILURE(reopen());
    EXPECT_TRUE(store_.index_restored());
    EXPECT_EQ(count_entries(store_), e);
    for (const std::string& k : deleted)
        ASSERT_EQ(get_or_empty(store_, k), "") << k << " is back after reopen";
}

// The open says what the store is, with the parameters it runs on; a KV store
// says nothing of the kind. StoreConfig::overprovision is not used: a value
// that would stop a KV store from opening (no spare at all) changes nothing.
TEST_F(StoreEvictTest, OpenSaysItIsAMemcacheStore) {
    config_.overprovision = 0;
    ASSERT_NO_FATAL_FAILURE(open(store_, config_));
    const std::string log = log_;
    EXPECT_NE(log.find("udepot: memcache store: GC evicts the entries"),
              std::string::npos) << log;
    EXPECT_NE(log.find("(gc_type 1), started at 20 free segments and run "
                       "until 40. Live data is capped at the device (its "
                       "tail included) less 10 segments"),
              std::string::npos) << log;
    EXPECT_EQ(log.find("can never rest"), std::string::npos) << log;
    EXPECT_NE(log.find("On " + std::to_string(kSegs) +
                       " segments of 128 KiB"), std::string::npos) << log;
    // The segment is the one asked for: nothing shrank it.
    EXPECT_EQ(log.find("do not fit"), std::string::npos) << log;
    ASSERT_EQ(store_.put("k", "v").run_sync(), 0);
    EXPECT_EQ(get_or_empty(store_, "k"), "v");
    store_.close();

    StoreConfig kv = config_;
    kv.memcache = false;
    kv.overprovision = 200;
    kv.force_destroy = true;
    Store kv_store;
    ASSERT_NO_FATAL_FAILURE(open(kv_store, kv));
    const std::string kv_log = log_;
    EXPECT_EQ(kv_log.find("memcache"), std::string::npos) << kv_log;
    kv_store.close();
}

// Salsa's spare-segment check passes at any size under the memcache cap
// (the device less 10 segments), so a small device is no longer shrunk; but
// one of no more segments than GC's high watermark could never rest, and the
// open says so.
TEST_F(StoreEvictTest, OpenSaysWhenGcCanNeverRest) {
    config_.size = 32 * 1024 * 1024 + 4096;
    config_.segment_size = 2048;  // 1 MiB: 32 of them
    ASSERT_NO_FATAL_FAILURE(open(store_, config_));
    const std::string log = log_;
    EXPECT_EQ(log.find("do not fit"), std::string::npos) << log;
    EXPECT_NE(log.find("On 32 segments of 1024 KiB"), std::string::npos) << log;
    EXPECT_NE(log.find("udepot: memcache store: the device has fewer "
                       "segments than 43 (GC's high watermark of 40, the "
                       "directory's table segments and 2 more that are "
                       "never free), so GC can never rest"),
              std::string::npos) << log;
    ASSERT_EQ(store_.put("k", "v").run_sync(), 0);
    EXPECT_EQ(get_or_empty(store_, "k"), "v");
}

// A clean close keeps a memcache store's entries.
TEST_F(StoreEvictTest, CleanShutdownRestoresTheIndex) {
    ASSERT_NO_FATAL_FAILURE(open(store_, config_));
    for (int k = 0; k < 500; ++k)
        ASSERT_EQ(store_.put("c" + std::to_string(k), value_for(k, 1))
                      .run_sync(), 0);
    ASSERT_EQ(store_.del("c7").run_sync(), 0);
    ASSERT_NO_FATAL_FAILURE(reopen());
    const std::string log = log_;
    EXPECT_TRUE(store_.index_restored()) << log;
    EXPECT_EQ(log.find("not shut down cleanly"), std::string::npos) << log;
    for (int k = 0; k < 500; ++k)
        ASSERT_EQ(get_or_empty(store_, "c" + std::to_string(k)),
                  k == 7 ? std::string() : value_for(k, 1)) << k;
}

// After a crash the log is not replayed: the store opens empty, says so, and
// works. Taken after evictions, where a replay would bring back stale
// values: the hot keys' older copies sit in segments that are not reused yet.
TEST_F(StoreEvictTest, UncleanShutdownStartsEmpty) {
    ASSERT_NO_FATAL_FAILURE(open(store_, config_));
    fill(6 * kSegs * kRecsPerSeg);
    ASSERT_FALSE(HasFatalFailure());
    quiesce(store_);
    ASSERT_GT(survivors().count(), 0u);
    const uint64_t unique = unique_;

    Store recovered;
    ASSERT_NO_FATAL_FAILURE(open_crashed_copy(recovered, true));
    const std::string log = log_;
    EXPECT_NE(log.find("udepot: memcache store was not shut down cleanly "
                       "(no persisted index): starting empty."),
              std::string::npos) << log;
    EXPECT_FALSE(recovered.index_restored());
    EXPECT_NE(recovered.seed(), store_.seed()) << "old records stay valid";
    EXPECT_EQ(count_entries(recovered), (Entries{0, 0}));
    for (uint64_t k = 0; k < unique; k += 7)
        ASSERT_EQ(get_or_empty(recovered, "u" + std::to_string(k)), "") << k;
    for (int j = 0; j < kHotKeys; ++j)
        ASSERT_EQ(get_or_empty(recovered, "hot" + std::to_string(j)), "") << j;

    // Usable: new data under the old names reads back as the new data, and
    // survives a clean close.
    for (int j = 0; j < kHotKeys; ++j)
        ASSERT_EQ(recovered.put("hot" + std::to_string(j), "new" +
                                std::to_string(j)).run_sync(), 0);
    for (int j = 0; j < kHotKeys; ++j)
        ASSERT_EQ(get_or_empty(recovered, "hot" + std::to_string(j)),
                  "new" + std::to_string(j));
    recovered.close();
    StoreConfig cfg = config_;
    cfg.path = snap_.c_str();
    cfg.force_destroy = false;
    Store again;
    ASSERT_NO_FATAL_FAILURE(open(again, cfg));
    const std::string log2 = log_;
    EXPECT_TRUE(again.index_restored()) << log2;
    EXPECT_EQ(get_or_empty(again, "hot3"), "new3");
    again.close();
}

// The index is invalidated at open: a clean close, a reopen and a crash
// later must not restore that old index over what was written since.
TEST_F(StoreEvictTest, CrashAfterACleanReopenStartsEmpty) {
    ASSERT_NO_FATAL_FAILURE(open(store_, config_));
    ASSERT_EQ(store_.put("a", "1").run_sync(), 0);
    ASSERT_NO_FATAL_FAILURE(reopen());
    ASSERT_TRUE(store_.index_restored());
    ASSERT_EQ(store_.put("b", "2").run_sync(), 0);

    Store recovered;
    ASSERT_NO_FATAL_FAILURE(open_crashed_copy(recovered, true));
    const std::string log = log_;
    EXPECT_NE(log.find("not shut down cleanly"), std::string::npos) << log;
    EXPECT_FALSE(recovered.index_restored());
    EXPECT_EQ(get_or_empty(recovered, "a"), "");
    EXPECT_EQ(get_or_empty(recovered, "b"), "");
    recovered.close();
}

// The same crash on a KV (non-memcache) store replays the log, as before,
// and says nothing about memcache.
TEST_F(StoreEvictTest, UncleanShutdownOfAKvStoreStillRecoversFromTheLog) {
    config_.memcache = false;
    ASSERT_NO_FATAL_FAILURE(open(store_, config_));
    for (int k = 0; k < 300; ++k)
        ASSERT_EQ(store_.put("r" + std::to_string(k), value_for(k, 2))
                      .run_sync(), 0);
    ASSERT_EQ(store_.del("r5").run_sync(), 0);

    Store recovered;
    ASSERT_NO_FATAL_FAILURE(open_crashed_copy(recovered, false));
    const std::string log = log_;
    EXPECT_EQ(log.find("memcache"), std::string::npos) << log;
    EXPECT_FALSE(recovered.index_restored());
    for (int k = 0; k < 300; ++k)
        ASSERT_EQ(get_or_empty(recovered, "r" + std::to_string(k)),
                  k == 5 ? std::string() : value_for(k, 2)) << k;
    recovered.close();
}

// A store's geometry is fixed by its device metadata. Opened with parameters
// salsa refuses for that geometry (here a KV store's overprovision of 0,
// which leaves no spare segment), it must fail to open, saying why, and leave
// the device alone: it used to halve the segment, find every segment's
// metadata invalid, and start empty (memcache) or scan the log over the
// wrong geometry (KV).
TEST_F(StoreEvictTest, ExistingStoreSalsaRefusesFailsToOpenUntouched) {
    config_.size = 64 * kSegGrains * kGrain + 4096;
    config_.memcache = false;
    ASSERT_NO_FATAL_FAILURE(open(store_, config_));
    for (int k = 0; k < 200; ++k)
        ASSERT_EQ(store_.put("p" + std::to_string(k), value_for(k, 3))
                      .run_sync(), 0);
    ASSERT_EQ(store_.del("p9").run_sync(), 0);

    // A crashed copy of it too: recovered from the log, as a KV store.
    std::filesystem::copy_file(
        path_, snapshot_path(),
        std::filesystem::copy_options::overwrite_existing);
    snap_ = snapshot_path().string();
    StoreConfig crashed = config_;
    crashed.path = snap_.c_str();
    crashed.force_destroy = false;

    store_.close();
    config_.force_destroy = false;
    for (const StoreConfig& base : {config_, crashed}) {
        StoreConfig bad = base;
        bad.overprovision = 0;
        Store refused;
        ASSERT_EQ(try_open(refused, bad), -EINVAL) << log_;
        EXPECT_NE(log_.find("udepot: the existing store, 64 segments of "
                            "128 KiB, cannot be opened as a KV store "
                            "(overprovision 0 per mille): salsa refuses it "
                            "at this overprovision"),
                  std::string::npos) << log_;
        EXPECT_EQ(log_.find("not shut down cleanly"), std::string::npos)
            << log_;
    }

    // Untouched: opened as it was created, the data is there.
    ASSERT_NO_FATAL_FAILURE(open(store_, config_));
    EXPECT_TRUE(store_.index_restored()) << log_;
    for (int k = 0; k < 200; ++k)
        ASSERT_EQ(get_or_empty(store_, "p" + std::to_string(k)),
                  k == 9 ? std::string() : value_for(k, 3)) << k;
    Store recovered;
    ASSERT_NO_FATAL_FAILURE(open(recovered, crashed));
    EXPECT_FALSE(recovered.index_restored());
    for (int k = 0; k < 200; ++k)
        ASSERT_EQ(get_or_empty(recovered, "p" + std::to_string(k)),
                  k == 9 ? std::string() : value_for(k, 3)) << k;
    recovered.close();
}

// A memcache store's live data is capped at the device less half the low
// watermark's segments, so GC is always due before salsa refuses a put for
// space; and a refusal that does come (several writers fill the few segments
// between GC being due and the cap faster than it evicts) is a wait for GC,
// not an error. However large the device and however many threads write new
// keys, no put fails. With the cap a percentage, as legacy's 40 per mille,
// a device of more than ~500 segments reached it with more than 20 segments
// still free: GC was not due, and puts failed with ENOSPC for as long as
// that lasted.
TEST_F(StoreEvictTest, NoPutFailsForSpaceOnAnyDeviceSize) {
    uint64_t waits = 0;
    for (const uint64_t segs : {200u, 600u, 1000u}) {
        config_.size = segs * kSegGrains * kGrain + 4096;
        config_.force_destroy = true;
        ASSERT_NO_FATAL_FAILURE(open(store_, config_));
        constexpr int kThreads = 4;
        const std::string val(600, 'v');  // 2-grain records
        const uint64_t per_thread = 3 * segs * ((kSegGrains - 1) / 2) /
                                    kThreads;
        std::atomic<int> failures{0};
        std::atomic<int> first_rc{0};
        std::vector<std::thread> threads;
        for (int t = 0; t < kThreads; ++t)
            threads.emplace_back([&, t] {
                for (uint64_t i = 0; i < per_thread; ++i) {
                    const int rc = store_.put("t" + std::to_string(t) + "_" +
                                              std::to_string(i), val)
                                       .run_sync();
                    if (rc != 0) {
                        ++failures;
                        int expected = 0;
                        first_rc.compare_exchange_strong(expected, rc);
                    }
                }
            });
        for (auto& th : threads) th.join();
        EXPECT_EQ(failures.load(), 0)
            << segs << " segments: puts failed, first with " << first_rc.load();
        EXPECT_EQ(g_relocations.load(), 0);
        waits += store_.capacity_waits();
        store_.close();
    }
    // The test means nothing unless writers did meet the cap.
    EXPECT_GT(waits, 0u) << "no put was ever refused by the cap";
}

// The circular GC frees a segment only when its relocation upcall succeeds.
// A directory table's index segment, retired by a grow, has nothing to
// relocate: it must be let go, or every grow leaks the old tables' segments
// for the session. Live tables' segments are kept. Many small records, so
// that the directory grows several times, and several devices' worth of
// them, so that GC comes round to every index segment.
namespace {
Store* g_hook_store = nullptr;
std::atomic<int> g_index_freed{0};
std::atomic<int> g_index_kept{0};
std::atomic<int> g_live_table_freed{0};
// What GC decided for an index segment; a freed one must not be a live
// table's.
void count_index_gc(uint64_t seg, bool freed) {
    if (!freed) {
        ++g_index_kept;
        return;
    }
    ++g_index_freed;
    udepot::Rcu::ReadGuard guard(g_hook_store->rcu());
    for (const auto& t : g_hook_store->directory().snapshot().tables)
        if (t->region().grain() / kSegGrains == seg) ++g_live_table_freed;
}
}  // namespace

TEST_F(StoreEvictTest, RetiredIndexSegmentsAreFreedByTheCircularGc) {
    g_index_freed = 0;
    g_index_kept = 0;
    g_live_table_freed = 0;
    g_hook_store = &store_;
    Store::index_gc_test_hook.store(count_index_gc);
    ASSERT_NO_FATAL_FAILURE(open(store_, config_));
    // 2-grain records: a device holds ~17K of them, of which ~14K live,
    // which takes 4 tables of this segment size (about 5K entries each),
    // not so many that a grow for the next doubling could not find the
    // segments.
    const std::string val(600, 'v');
    const uint64_t total = 6 * kSegs * ((kSegGrains - 1) / 2);
    for (uint64_t i = 0; i < total; ++i)
        ASSERT_EQ(store_.put("s" + std::to_string(i), val).run_sync(), 0)
            << "put " << i;
    quiesce(store_);

    // Doubling directory: every table but the last generation was retired.
    const size_t tables = store_.directory().snapshot().tables.size();
    ASSERT_GE(tables, 4u) << "the directory did not grow";
    EXPECT_GE(g_index_freed.load(), static_cast<int>(tables - 1))
        << "retired index segments were not let go";
    EXPECT_EQ(g_relocations.load(), 0);
    EXPECT_EQ(g_live_table_freed.load(), 0)
        << "GC let go of a live table's segment";
    EXPECT_GT(g_index_kept.load(), 0) << "GC never met a live table";

    // The live tables' segments were kept: the index still restores.
    ASSERT_NO_FATAL_FAILURE(reopen());
    EXPECT_TRUE(store_.index_restored()) << log_;
    EXPECT_EQ(get_or_empty(store_, "s" + std::to_string(total - 1)), val);
}

// A persisted index the open cannot use is not the same as none: the
// message says which.
TEST_F(StoreEvictTest, OpenSaysWhyAPersistedIndexIsNotRestored) {
    ASSERT_NO_FATAL_FAILURE(open(store_, config_));
    ASSERT_EQ(store_.put("a", "1").run_sync(), 0);
    Store::index_format_test_hook.store([](uint32_t) { return uint32_t{0}; });
    store_.close();  // writes an index in another format
    Store::index_format_test_hook.store(nullptr);

    config_.force_destroy = false;
    ASSERT_NO_FATAL_FAILURE(open(store_, config_));
    EXPECT_FALSE(store_.index_restored());
    EXPECT_NE(log_.find("memcache store: its persisted index cannot be "
                        "restored (the persisted index is in another "
                        "format): starting empty"),
              std::string::npos) << log_;
    EXPECT_EQ(log_.find("not shut down cleanly"), std::string::npos) << log_;
    EXPECT_EQ(get_or_empty(store_, "a"), "");
}

// close() says what a failed index flush costs, by mode.
TEST_F(StoreEvictTest, CloseSaysWhatAnUnpersistedIndexCosts) {
    // (No other test covers this message.) The footer hook returning false
    // stops the flush before the first footer, as a crash would.
    for (bool memcache : {true, false}) {
        config_.memcache = memcache;
        config_.force_destroy = true;
        ASSERT_NO_FATAL_FAILURE(open(store_, config_));
        ASSERT_EQ(store_.put("a", "1").run_sync(), 0);
        Store::index_footer_test_hook.store([] { return false; });
        ::testing::internal::CaptureStderr();
        store_.close();
        const std::string log = ::testing::internal::GetCapturedStderr();
        Store::index_footer_test_hook.store(nullptr);
        if (memcache) {
            EXPECT_NE(log.find("index not persisted"), std::string::npos)
                << log;
            EXPECT_NE(log.find("the next open starts empty: this memcache "
                               "store's entries are lost"),
                      std::string::npos) << log;
            EXPECT_EQ(log.find("recovers from the log"), std::string::npos)
                << log;
        } else {
            EXPECT_NE(log.find("the next open recovers from the log"),
                      std::string::npos) << log;
            EXPECT_EQ(log.find("memcache"), std::string::npos) << log;
        }
    }
}

// What a memcache store's cap has to leave: a table and 4 data segments
// (kMcMinTableSegments, kMcMinDataSegments), which is 15 segments of 256
// grains with the 10 the cap takes off. An existing store below that is
// refused with the real cause; a new one is shrunk to segments that fit, as
// for any device too small.
TEST_F(StoreEvictTest, TinyDevicesAreRefusedOrShrunkAtTheMinimum) {
    // Existing stores, created as KV with a high overprovision so that they
    // open at these sizes at all.
    for (const uint64_t segs : {10u, 11u, 14u, 15u}) {
        StoreConfig kv = config_;
        kv.memcache = false;
        kv.overprovision = 900;
        kv.size = segs * kSegGrains * kGrain + 4096;
        kv.force_destroy = true;
        {
            Store s;
            ASSERT_NO_FATAL_FAILURE(open(s, kv)) << segs;
            ASSERT_EQ(s.put("a", "1").run_sync(), 0);
            s.close();
        }
        StoreConfig mc = kv;
        mc.memcache = true;
        mc.force_destroy = false;
        Store s;
        if (segs < 15) {
            ASSERT_EQ(try_open(s, mc), -EINVAL) << segs << ": " << log_;
            EXPECT_NE(log_.find("cannot be opened as a memcache store"),
                      std::string::npos) << log_;
            EXPECT_NE(log_.find("under the 1275 a memcache store needs for "
                                "a table and 4 data segments; that is at "
                                "least 15 segments of this size"),
                      std::string::npos) << log_;
            EXPECT_EQ(log_.find("too few spare segments"), std::string::npos)
                << log_;
        } else {
            ASSERT_EQ(try_open(s, mc), 0) << segs << ": " << log_;
            EXPECT_TRUE(s.index_restored());
            EXPECT_EQ(get_or_empty(s, "a"), "1");
            ASSERT_EQ(s.put("b", "2").run_sync(), 0);
            EXPECT_EQ(get_or_empty(s, "b"), "2");
            s.close();
        }
    }
    // New stores: segments halved until the cap leaves the room, said so.
    for (const uint64_t segs : {10u, 11u, 14u}) {
        StoreConfig mc = config_;
        mc.size = segs * kSegGrains * kGrain + 4096;
        Store s;
        ASSERT_EQ(try_open(s, mc), 0) << segs << ": " << log_;
        EXPECT_NE(log_.find("do not fit a"), std::string::npos) << log_;
        EXPECT_NE(log_.find("leaves less than a table and 4 data segments"),
                  std::string::npos) << log_;
        ASSERT_EQ(s.put("a", "1").run_sync(), 0);
        EXPECT_EQ(get_or_empty(s, "a"), "1");
        s.close();
    }
}

// A grow that salsa refuses for capacity (the cap) waits for GC, keeps the
// tables it has gathered, and completes; no put fails. The refusals come
// from the index-segment seam: the first requests of every grow are refused
// as the cap would, then let through.
namespace {
std::atomic<int> g_refusals_left{0};
std::atomic<int> g_index_allocs{0};
std::atomic<int> g_index_calls{0};
// Refuses every third request while refusals are left, so that a grow has
// gathered tables when one comes.
int refuse_index_segment_for_capacity() {
    if (g_index_calls.fetch_add(1) % 3 == 2) {
        int left = g_refusals_left.load();
        while (left > 0 &&
               !g_refusals_left.compare_exchange_weak(left, left - 1)) {}
        if (left > 0) return -ENOSPC;
    }
    ++g_index_allocs;  // goes on to salsa
    return 0;
}
}  // namespace

TEST_F(StoreEvictTest, DirectoryGrowWaitsForGcWhenTheCapRefusesIt) {
    config_.size = 100 * kSegGrains * kGrain + 4096;
    ASSERT_NO_FATAL_FAILURE(open(store_, config_));
    g_refusals_left = 40;  // more than the grows the fill needs: some stay
    g_index_allocs = 0;
    g_index_calls = 0;
    Store::index_segment_test_hook.store(refuse_index_segment_for_capacity);
    constexpr int kThreads = 4;
    std::atomic<int> failures{0};
    std::atomic<int> first_rc{0};
    std::vector<std::thread> threads;
    for (int t = 0; t < kThreads; ++t)
        threads.emplace_back([&, t] {
            for (uint64_t i = 0; i < 40000 / kThreads; ++i) {  // 1-grain
                const int rc = store_.put("g" + std::to_string(t) + "_" +
                                          std::to_string(i), "v").run_sync();
                if (rc != 0) {
                    ++failures;
                    int zero = 0;
                    first_rc.compare_exchange_strong(zero, rc);
                }
            }
        });
    for (auto& th : threads) th.join();
    Store::index_segment_test_hook.store(nullptr);
    EXPECT_EQ(failures.load(), 0) << "first failure " << first_rc.load();
    const size_t tables = store_.directory().snapshot().tables.size();
    EXPECT_GE(tables, 4u) << "the directory did not grow";
    EXPECT_LT(g_refusals_left.load(), 40) << "no refusal was met";
    EXPECT_GT(store_.capacity_waits(), 0u);
    // Each grow gathers its new tables once: the ones kept across a
    // refusal are reused, not allocated again (1 -> 2 -> ... -> tables).
    EXPECT_EQ(g_index_allocs.load(), static_cast<int>(2 * tables - 2))
        << "a refused grow dropped the tables it had gathered";
}

// A device of fewer segments than GC's high watermark, the first table and
// the two salsa keeps cannot let GC rest, and the open says so; one of
// exactly that many can. Neither burns a core: GC backs off when it can only
// meet segments it cannot clean.
TEST_F(StoreEvictTest, GcRestsFromTheFloorUpAndNeverSpins) {
    auto idle_cpu = [] {
        auto cpu = [] {
            timespec t;
            clock_gettime(CLOCK_PROCESS_CPUTIME_ID, &t);
            return t.tv_sec + t.tv_nsec * 1e-9;
        };
        std::this_thread::sleep_for(std::chrono::milliseconds(300));
        const double c0 = cpu();
        std::this_thread::sleep_for(std::chrono::seconds(1));
        return cpu() - c0;
    };
    for (const uint64_t segs : {42u, 43u}) {
        config_.size = segs * kSegGrains * kGrain + 4096;
        config_.force_destroy = true;
        ASSERT_NO_FATAL_FAILURE(open(store_, config_));
        const bool warned = log_.find("so GC can never rest") !=
                            std::string::npos;
        EXPECT_EQ(warned, segs < 43u) << segs << ": " << log_;
        for (uint64_t i = 0; i < 3 * segs * 127; ++i)
            ASSERT_EQ(store_.put("r" + std::to_string(i),
                                 std::string(600, 'v')).run_sync(), 0);
        // Process CPU time, so a loaded runner does not inflate it; a spin
        // is ~1 s of it over the second, a GC at rest or waiting ~0.
        const double cpu = idle_cpu();
        if (segs < 43u) {
            // Never at rest, but waiting (for a kick) once a lap of its queue
            // frees nothing: not a core.
            EXPECT_LT(cpu, 0.1) << segs << " segments: GC spins";
            EXPECT_GT(store_.gc_waits(), 0u)
                << segs << " segments: GC never waited";
        } else {
            EXPECT_LT(cpu, 0.1) << segs << " segments: GC does not rest";
        }
        store_.close();
    }
}

// After a clean close the segments go back to GC oldest first, not in index
// order, so eviction stays oldest first across a reopen. Segments are reused
// out of age order once GC has run, so the first session has to have
// evicted.
TEST_F(StoreEvictTest, EvictionStaysOldestFirstAcrossACleanReopen) {
    ASSERT_NO_FATAL_FAILURE(open(store_, config_));
    const uint64_t per_dev = kSegs * kRecsPerSeg;
    auto put_range = [&](uint64_t from, uint64_t to) {
        for (uint64_t i = from; i < to; ++i)
            ASSERT_EQ(store_.put("o" + std::to_string(i),
                                 value_for(static_cast<int>(i), 0))
                          .run_sync(), 0) << i;
    };
    put_range(0, 3 * per_dev);
    ASSERT_FALSE(HasFatalFailure());
    quiesce(store_);
    ASSERT_NO_FATAL_FAILURE(reopen());
    ASSERT_TRUE(store_.index_restored()) << log_;
    // Enough new data to evict part, not all, of what the first session
    // left.
    put_range(3 * per_dev, 3 * per_dev + per_dev / 3);
    ASSERT_FALSE(HasFatalFailure());
    quiesce(store_);
    uint64_t first_alive = 0;
    bool seen = false;
    uint64_t alive = 0;
    for (uint64_t i = 0; i < 3 * per_dev + per_dev / 3; ++i) {
        const bool a =
            !get_or_empty(store_, "o" + std::to_string(i)).empty();
        if (a) {
            if (!seen) first_alive = i;
            seen = true;
            ++alive;
        } else if (seen) {
            FAIL() << "key " << i << " was evicted, but key " << first_alive
                   << ", older, was not";
        }
    }
    EXPECT_GT(alive, 0u);
    EXPECT_GT(first_alive, 2 * per_dev) << "the first session was not evicted";
}

// A memcache store's live data can exceed a KV store's cap: opened as KV it
// is refused, and the open says why.
TEST_F(StoreEvictTest, CleanMemcacheStoreOpenedAsKvSaysWhyItFails) {
    ASSERT_NO_FATAL_FAILURE(open(store_, config_));
    for (uint64_t i = 0; i < 2 * kSegs * kRecsPerSeg; ++i)
        ASSERT_EQ(store_.put("m" + std::to_string(i), value_for(1, 1))
                      .run_sync(), 0);
    quiesce(store_);
    store_.close();
    StoreConfig kv = config_;
    kv.memcache = false;
    kv.overprovision = 500;  // a cap of half the device
    kv.force_destroy = false;
    Store s;
    ASSERT_EQ(try_open(s, kv), -EIO) << log_;
    EXPECT_NE(log_.find("the persisted index holds more live data than this "
                        "KV store may"), std::string::npos) << log_;
    // The failed open cleared the index, as it says: as memcache the store
    // now starts empty.
    EXPECT_NE(log_.find("it has cleared the index's footers"),
              std::string::npos) << log_;
    config_.force_destroy = false;
    Store again;
    ASSERT_NO_FATAL_FAILURE(open(again, config_));
    EXPECT_FALSE(again.index_restored());
    EXPECT_NE(log_.find("not shut down cleanly"), std::string::npos) << log_;
    EXPECT_EQ(get_or_empty(again, "m" + std::to_string(2 * kSegs *
                                                       kRecsPerSeg - 1)), "");
    again.close();
}

// GC waits (for a kick) only when a lap of its queue freed nothing, not on
// each live table's segment it meets: with several tables and writers
// keeping GC busy there are data segments to evict all the time, and it
// never waits. (Waiting at every table would cost a store of D tables ~D
// stalls per lap of its queue.)
TEST_F(StoreEvictTest, GcNeverWaitsWhileAFreeableSegmentIsQueued) {
    g_index_freed = 0;
    g_index_kept = 0;
    g_live_table_freed = 0;
    g_hook_store = &store_;
    Store::index_gc_test_hook.store(count_index_gc);
    ASSERT_NO_FATAL_FAILURE(open(store_, config_));
    const std::string val(600, 'v');  // 2-grain records: 4 tables
    for (uint64_t i = 0; i < 6 * kSegs * ((kSegGrains - 1) / 2); ++i)
        ASSERT_EQ(store_.put("b" + std::to_string(i), val).run_sync(), 0)
            << "put " << i;
    quiesce(store_);
    ASSERT_GE(store_.directory().snapshot().tables.size(), 4u);
    EXPECT_GT(g_index_kept.load(), 0) << "GC never met a live table";
    EXPECT_EQ(store_.gc_waits(), 0u) << "GC waited with data segments queued";
}

// An index segment whose metadata write failed is invalidated and released,
// and must be flagged retired too, or the circular GC meets it, gets ENOSYS
// for ever, and the segment is lost to the store. (The same goes for one
// whose mapping failed; that path sets the flag too, but no seam fails an
// mmap, so it is untested: a follow-up.)
// The seams: the index-segment hook marks the thread that is allocating one,
// and the internal-I/O hook fails the next write on it, which is that
// segment's metadata.
namespace {
thread_local bool t_allocating_index_segment = false;
std::atomic<bool> g_fail_next_index_md{false};
std::atomic<int64_t> g_failed_index_seg{-1};
std::mutex g_freed_segs_mu;
std::set<uint64_t> g_freed_segs;
int mark_index_allocation() {
    t_allocating_index_segment = true;
    return 0;
}
int fail_index_segment_md(bool write, off_t offset, size_t) {
    if (!write || !t_allocating_index_segment) return 0;
    t_allocating_index_segment = false;
    if (!g_fail_next_index_md.exchange(false)) return 0;
    g_failed_index_seg = offset / static_cast<off_t>(kSegGrains * kGrain);
    return -EIO;
}
void record_freed_index_segment(uint64_t seg, bool freed) {
    if (!freed) return;
    std::lock_guard<std::mutex> lock(g_freed_segs_mu);
    g_freed_segs.insert(seg);
}
}  // namespace

TEST_F(StoreEvictTest, IndexSegmentWhoseMetadataFailedIsFreedByGc) {
    config_.size = 100 * kSegGrains * kGrain + 4096;
    g_fail_next_index_md = true;
    g_failed_index_seg = -1;
    g_freed_segs.clear();
    ASSERT_NO_FATAL_FAILURE(open(store_, config_));
    Store::index_segment_test_hook.store(mark_index_allocation);
    Store::internal_io_test_hook.store(fail_index_segment_md);
    Store::index_gc_test_hook.store(record_freed_index_segment);
    // 1-grain records: the directory has to grow, whose first new table's
    // metadata write fails (the puts waiting on that grow get -EIO), and
    // then grows again. Six devices' worth, so that GC comes round.
    int bad = 0, eio = 0;
    for (uint64_t i = 0; i < 6 * 100 * (kSegGrains - 1); ++i) {
        const int rc = store_.put("x" + std::to_string(i), "v").run_sync();
        if (rc == -EIO) ++eio;
        else if (rc != 0) ++bad;
    }
    Store::internal_io_test_hook.store(nullptr);
    quiesce(store_);
    EXPECT_EQ(bad, 0);
    EXPECT_GE(eio, 1) << "the metadata write never failed";
    ASSERT_GE(g_failed_index_seg.load(), 0);
    EXPECT_GE(store_.directory().snapshot().tables.size(), 2u);
    std::lock_guard<std::mutex> lock(g_freed_segs_mu);
    EXPECT_TRUE(g_freed_segs.count(g_failed_index_seg.load()))
        << "GC never freed segment " << g_failed_index_seg.load()
        << ", whose metadata write failed";
}

// The floor under which GC cannot rest counts the tables the directory has,
// restored ones included. A KV store with small segments (tables of ~250
// entries) grown to two tables, reopened as memcache on a device of 43
// segments, is under the floor of 44; with one table 43 would be enough.
TEST_F(StoreEvictTest, GcFloorCountsTheRestoredTables) {
    config_.memcache = false;
    config_.grain_size = 128;  // small grains and segments: tables of
    config_.segment_size = 32;  // ~270 entries, which 33 segments can hold
    config_.size = 43 * 32 * 128 + 128;
    ASSERT_NO_FATAL_FAILURE(open(store_, config_));
    for (uint64_t i = 0; i < 400 &&
                         store_.directory().snapshot().tables.size() < 2; ++i)
        ASSERT_EQ(store_.put("f" + std::to_string(i), "v").run_sync(), 0) << i;
    ASSERT_EQ(store_.directory().snapshot().tables.size(), 2u)
        << "the directory did not grow to two tables";
    store_.close();
    config_.memcache = true;
    config_.force_destroy = false;
    ASSERT_NO_FATAL_FAILURE(open(store_, config_));
    EXPECT_TRUE(store_.index_restored()) << log_;
    EXPECT_EQ(store_.directory().snapshot().tables.size(), 2u);
    EXPECT_NE(log_.find("fewer segments than 44 (GC's high watermark of 40, "
                        "the directory's table segments and 2 more"),
              std::string::npos) << log_;
}

// The grow guard's ENOSPC and its message: a grow the cap refuses a segment
// to, that could never fit at its peak, fails (no real workload gets there,
// so the guard's cap is lowered through the seam).
TEST_F(StoreEvictTest, GrowThatCouldNeverFitFailsWithAMessage) {
    config_.size = 100 * kSegGrains * kGrain + 4096;
    ASSERT_NO_FATAL_FAILURE(open(store_, config_));
    // 6 net segments of cap: the grow of one table needs 3 + 4 = 7.
    store_.set_memcache_guard_cap_for_test(6 * (kSegGrains - 1));
    Store::index_segment_test_hook.store([] { return -ENOSPC; });
    int enospc = 0, other = 0;
    ::testing::internal::CaptureStderr();
    for (uint64_t i = 0; i < 20000 && enospc == 0; ++i) {  // 1-grain
        const int rc = store_.put("q" + std::to_string(i), "v").run_sync();
        if (rc == -ENOSPC) ++enospc;
        else if (rc != 0) ++other;
    }
    const std::string log = ::testing::internal::GetCapturedStderr();
    Store::index_segment_test_hook.store(nullptr);
    EXPECT_EQ(other, 0);
    EXPECT_EQ(enospc, 1) << "the grow did not fail";
    EXPECT_NE(log.find("udepot: memcache store: the directory may not be "
                       "able to grow past 1 table: growing needs 3 tables' "
                       "segments at once and 4 data segments"),
              std::string::npos) << log;
}

// Retiring a table's segment kicks GC: it may be waiting, after a lap of
// segments it could not free, and this one it now can. On a device too small
// for GC to rest it waits at once; retire a table by hand and the segment is
// freed promptly.
TEST_F(StoreEvictTest, RetiringATableSegmentKicksGc) {
    g_hook_store = &store_;
    g_freed_segs.clear();
    config_.size = 42 * kSegGrains * kGrain + 4096;
    ASSERT_NO_FATAL_FAILURE(open(store_, config_));
    Store::index_gc_test_hook.store(record_freed_index_segment);
    for (uint64_t i = 0; i < 3 * 42 * 127; ++i)
        ASSERT_EQ(store_.put("k" + std::to_string(i), std::string(600, 'v'))
                      .run_sync(), 0) << i;
    // GC provably asleep: the eviction has run its course (the entries stop
    // changing), and it has waited, and does not wait again (its counter
    // is still) for long enough that it is not just between two waits.
    quiesce(store_);
    uint64_t waits = 0;
    for (int stable = 0, i = 0; stable < 10 && i < 2000; ++i) {
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
        const uint64_t now = store_.gc_waits();
        stable = (now == waits && now > 0) ? stable + 1 : 0;
        waits = now;
    }
    ASSERT_GT(waits, 0u) << "GC did not settle to waiting";
    const uint64_t seg =
        store_.directory().snapshot().tables[0]->region().grain() / kSegGrains;
    store_.retire_table_for_test(0);
    bool freed = false;
    for (int i = 0; i < 2000 && !freed; ++i) {
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
        std::lock_guard<std::mutex> lock(g_freed_segs_mu);
        freed = g_freed_segs.count(seg) != 0;
    }
    EXPECT_TRUE(freed) << "GC did not free the retired table's segment";
}

// A segment GC cannot read (here: every read of its own fails) takes part in
// the lap like one it cannot free: after a lap with nothing freed it waits
// for a kick instead of spinning on them. (Before, any error but ENOSYS made
// it try the next at once, for ever.)
namespace {
std::atomic<bool> g_fail_gc_reads{false};
std::atomic<int> g_failed_gc_reads{0};
int fail_gc_reads(bool write, off_t, size_t) {
    if (write || !g_fail_gc_reads.load()) return 0;
    ++g_failed_gc_reads;
    return -EIO;
}
}  // namespace

TEST_F(StoreEvictTest, GcWaitsInsteadOfSpinningOnSegmentsItCannotRead) {
    ASSERT_NO_FATAL_FAILURE(open(store_, config_));
    // The directory grows to two tables first, before any data: a table's
    // segment is sealed, so that GC meets it, only when the next one is
    // allocated. The first table's is retired and freed; the second's is
    // live, ahead of every data segment in GC's queue: a lap starts on a
    // segment GC cannot free, not on one that fails to read.
    ASSERT_EQ(store_.directory().grow(udepot::Directory::kAnyGeneration), 0);
    ASSERT_EQ(store_.directory().snapshot().tables.size(), 2u);
    ::testing::internal::CaptureStderr();
    g_fail_gc_reads = true;
    Store::internal_io_test_hook.store(fail_gc_reads);
    // Until GC is due (20 free segments) and has its first go at a segment
    // (its first failed read): stop writing then, so that no segment is
    // sealed, kicking GC, while it makes its first lap. (The cap is 10
    // segments further on, so no put waits: a waiting put spins in
    // run_sync(), which is not what is measured.)
    g_failed_gc_reads = 0;
    const uint64_t records = 140 * ((kSegGrains - 1) / 2);
    for (uint64_t i = 0; i < records && g_failed_gc_reads.load() == 0; ++i)
        ASSERT_EQ(store_.put("e" + std::to_string(i), std::string(600, 'v'))
                      .run_sync(), 0) << i;
    ASSERT_GT(g_failed_gc_reads.load(), 0) << "GC never tried to read";
    uint64_t waits = 0;
    for (int stable = 0, i = 0; stable < 10 && i < 3000; ++i) {
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
        const uint64_t now = store_.gc_waits();
        stable = (now == waits && now > 0) ? stable + 1 : 0;
        waits = now;
    }
    EXPECT_GT(waits, 0u) << "GC never waited";
    // The failure was said, in that first lap, which the live table's
    // segment started; and not again while GC sits waiting.
    const std::string said = ::testing::internal::GetCapturedStderr();
    EXPECT_NE(said.find("ctlr_relocate_segment  failed with 5"),
              std::string::npos) << "GC never reported the failing read";
    ::testing::internal::CaptureStderr();
    timespec t0, t1;
    clock_gettime(CLOCK_PROCESS_CPUTIME_ID, &t0);
    std::this_thread::sleep_for(std::chrono::seconds(1));
    clock_gettime(CLOCK_PROCESS_CPUTIME_ID, &t1);
    EXPECT_EQ(::testing::internal::GetCapturedStderr().find("failed with"),
              std::string::npos) << "GC kept reporting while it waited";
    const double cpu =
        (t1.tv_sec - t0.tv_sec) + (t1.tv_nsec - t0.tv_nsec) * 1e-9;
    EXPECT_LT(cpu, 0.1) << "GC spins on segments it cannot read";
    g_fail_gc_reads = false;
    Store::internal_io_test_hook.store(nullptr);
}

// A put that waits for GC to make room completes once segments are sealed:
// each sealing kicks GC, which may be waiting after a lap that freed
// nothing. Bounded: if the kick is missing the test fails after a few
// seconds (and kicks GC by hand so that it can end), instead of hanging.
TEST_F(StoreEvictTest, PutsOnADeviceTooSmallForGcToRestCompleteWithinABound) {
    config_.size = 42 * kSegGrains * kGrain + 4096;
    ASSERT_NO_FATAL_FAILURE(open(store_, config_));
    std::atomic<bool> done{false};
    std::thread writer([&] {
        for (uint64_t i = 0; i < 3 * 42 * 127; ++i)
            (void)store_.put("w" + std::to_string(i), std::string(600, 'v'))
                .run_sync();
        done = true;
    });
    bool in_time = false;
    for (int i = 0; i < 1500 && !(in_time = done.load()); ++i)  // 15 s
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    EXPECT_TRUE(in_time) << "puts were left waiting for a GC that was not "
                            "kicked when segments were sealed";
    for (int i = 0; i < 6000 && !done; ++i) {  // let a failed run end
        store_.kick_gc_for_test();
        std::this_thread::sleep_for(std::chrono::milliseconds(5));
    }
    writer.join();
}

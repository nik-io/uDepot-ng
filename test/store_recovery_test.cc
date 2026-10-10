// Copyright (c) 2024-2026 Nikolas Ioannou
// SPDX-License-Identifier: BSD-3-Clause

#include "udepot/store.h"
#include "udepot/io/posix.h"

#include <cstdint>
#include <cstring>
#include <filesystem>
#include <string>
#include <vector>

#include <unistd.h>

#include <gtest/gtest.h>

using udepot::PosixIO;
using udepot::StoreConfig;
using udepot::uDepot;

static constexpr size_t kStoreSize = 4 * 1024 * 1024;

// How a session ends before the store is reopened. kClean closes it, and
// the next open restores the index close() flushed. kCrash keeps the device
// as it was before close(), so the next open finds no index and scans the
// log.
enum class Shutdown { kClean, kCrash };

class StoreRecoveryTest : public ::testing::TestWithParam<Shutdown> {
protected:
    void SetUp() override {
        path_ = std::filesystem::temp_directory_path() /
                ("udepot_recovery_test_" + std::to_string(getpid()));
        config_.path = path_.c_str();
        config_.size = kStoreSize;
        config_.grain_size = 512;
        config_.initial_tables = 2;
        config_.index_bits = 10;
        config_.force_destroy = true;
    }

    void TearDown() override {
        uDepot<PosixIO>::index_footer_test_hook = nullptr;
        std::filesystem::remove(path_);
        std::filesystem::remove(crash_path());
    }

    StoreConfig config() const { return config_; }
    StoreConfig reopen_config() const {
        StoreConfig cfg = config_;
        cfg.force_destroy = false;
        return cfg;
    }
    std::filesystem::path crash_path() const {
        return path_.string() + ".crash";
    }

    void end_session(uDepot<PosixIO>& store) {
        if (GetParam() == Shutdown::kClean) {
            store.close();
            return;
        }
        crash(store);
    }

    // The device as it is now, whatever close() writes after.
    void crash(uDepot<PosixIO>& store) {
        const auto overwrite = std::filesystem::copy_options::overwrite_existing;
        std::filesystem::copy_file(path_, crash_path(), overwrite);
        store.close();
        std::filesystem::copy_file(crash_path(), path_, overwrite);
        std::filesystem::remove(crash_path());
    }

    std::filesystem::path path_;
    StoreConfig config_;
};

static std::string make_key(int i) {
    char buf[32];
    snprintf(buf, sizeof(buf), "rk_%06d", i);
    return buf;
}

static std::string make_val(int i) {
    char buf[64];
    snprintf(buf, sizeof(buf), "rv_%06d_data", i);
    return buf;
}

TEST_P(StoreRecoveryTest, DataSurvivesReopen) {
    constexpr int kKeys = 100;

    {
        uDepot<PosixIO> store;
        ASSERT_EQ(store.open(config()), 0);
        for (int i = 0; i < kKeys; ++i) {
            ASSERT_EQ(store.put(make_key(i), make_val(i)).run_sync(), 0)
                << "put i=" << i;
        }
        end_session(store);
    }

    {
        StoreConfig cfg = reopen_config();
        uDepot<PosixIO> store;
        ASSERT_EQ(store.open(cfg), 0);

        for (int i = 0; i < kKeys; ++i) {
            uint8_t val[128];
            size_t val_size = 0;
            int rc = store.get(make_key(i), val, sizeof(val),
                               &val_size).run_sync();
            ASSERT_EQ(rc, 0) << "key missing after reopen: " << make_key(i);
            EXPECT_EQ(
                std::string_view(reinterpret_cast<char*>(val), val_size),
                make_val(i));
        }
        store.close();
    }
}

TEST_P(StoreRecoveryTest, DeletedKeysStayDeletedAfterRecovery) {
    constexpr int kKeys = 40;

    {
        uDepot<PosixIO> store;
        ASSERT_EQ(store.open(config()), 0);
        for (int i = 0; i < kKeys; ++i)
            ASSERT_EQ(store.put(make_key(i), make_val(i)).run_sync(), 0);

        for (int i = 0; i < kKeys; i += 2)
            ASSERT_EQ(store.del(make_key(i)).run_sync(), 0);

        end_session(store);
    }

    {
        StoreConfig cfg = reopen_config();
        uDepot<PosixIO> store;
        ASSERT_EQ(store.open(cfg), 0);

        for (int i = 0; i < kKeys; ++i) {
            uint8_t val[128];
            size_t val_size = 0;
            int rc = store.get(make_key(i), val, sizeof(val),
                               &val_size).run_sync();
            if (i % 2 == 0) {
                EXPECT_NE(rc, 0)
                    << "deleted key still present: " << make_key(i);
            } else {
                ASSERT_EQ(rc, 0)
                    << "surviving key missing: " << make_key(i);
                EXPECT_EQ(
                    std::string_view(reinterpret_cast<char*>(val), val_size),
                    make_val(i));
            }
        }
        store.close();
    }
}

TEST_P(StoreRecoveryTest, UpsertedKeysHaveNewestValueAfterRecovery) {
    constexpr int kKeys = 30;

    {
        uDepot<PosixIO> store;
        ASSERT_EQ(store.open(config()), 0);

        for (int i = 0; i < kKeys; ++i)
            ASSERT_EQ(store.put(make_key(i), make_val(i)).run_sync(), 0);

        for (int i = 0; i < kKeys; ++i) {
            std::string new_val = make_val(i + 1000);
            ASSERT_EQ(store.put(make_key(i), new_val).run_sync(), 0);
        }

        end_session(store);
    }

    {
        StoreConfig cfg = reopen_config();
        uDepot<PosixIO> store;
        ASSERT_EQ(store.open(cfg), 0);

        for (int i = 0; i < kKeys; ++i) {
            uint8_t val[128];
            size_t val_size = 0;
            int rc = store.get(make_key(i), val, sizeof(val),
                               &val_size).run_sync();
            ASSERT_EQ(rc, 0) << "key missing: " << make_key(i);
            std::string expected = make_val(i + 1000);
            EXPECT_EQ(
                std::string_view(reinterpret_cast<char*>(val), val_size),
                expected)
                << "old value survived for: " << make_key(i);
        }
        store.close();
    }
}

TEST_P(StoreRecoveryTest, RecoveryOnEmptyStoreSucceeds) {
    {
        uDepot<PosixIO> store;
        ASSERT_EQ(store.open(config()), 0);
        end_session(store);
    }

    {
        StoreConfig cfg = reopen_config();
        uDepot<PosixIO> store;
        ASSERT_EQ(store.open(cfg), 0);

        uint8_t val[128];
        size_t val_size = 0;
        EXPECT_NE(store.get("nonexistent", val, sizeof(val),
                             &val_size).run_sync(), 0);
        store.close();
    }
}

TEST_P(StoreRecoveryTest, ForceDestroyIgnoresExistingData) {
    constexpr int kKeys = 20;

    {
        uDepot<PosixIO> store;
        ASSERT_EQ(store.open(config()), 0);
        for (int i = 0; i < kKeys; ++i)
            ASSERT_EQ(store.put(make_key(i), make_val(i)).run_sync(), 0);
        store.close();
    }

    {
        uDepot<PosixIO> store;
        ASSERT_EQ(store.open(config()), 0);

        for (int i = 0; i < kKeys; ++i) {
            uint8_t val[128];
            size_t val_size = 0;
            EXPECT_NE(store.get(make_key(i), val, sizeof(val),
                                 &val_size).run_sync(), 0)
                << "key should be gone after force_destroy: " << make_key(i);
        }
        store.close();
    }
}

TEST_P(StoreRecoveryTest, RecoveryWithManyKeys) {
    constexpr int kKeys = 500;

    {
        uDepot<PosixIO> store;
        ASSERT_EQ(store.open(config()), 0);
        for (int i = 0; i < kKeys; ++i)
            ASSERT_EQ(store.put(make_key(i), make_val(i)).run_sync(), 0);
        end_session(store);
    }

    {
        StoreConfig cfg = reopen_config();
        uDepot<PosixIO> store;
        ASSERT_EQ(store.open(cfg), 0);

        int found = 0;
        for (int i = 0; i < kKeys; ++i) {
            uint8_t val[128];
            size_t val_size = 0;
            int rc = store.get(make_key(i), val, sizeof(val),
                               &val_size).run_sync();
            if (rc == 0) {
                EXPECT_EQ(
                    std::string_view(reinterpret_cast<char*>(val), val_size),
                    make_val(i));
                ++found;
            }
        }
        EXPECT_EQ(found, kKeys);
        store.close();
    }
}

TEST_P(StoreRecoveryTest, NewWritesAfterRecoveryWork) {
    constexpr int kKeys = 30;
    constexpr int kNewKeys = 20;

    {
        uDepot<PosixIO> store;
        ASSERT_EQ(store.open(config()), 0);
        for (int i = 0; i < kKeys; ++i)
            ASSERT_EQ(store.put(make_key(i), make_val(i)).run_sync(), 0);
        end_session(store);
    }

    {
        StoreConfig cfg = reopen_config();
        uDepot<PosixIO> store;
        ASSERT_EQ(store.open(cfg), 0);

        for (int i = kKeys; i < kKeys + kNewKeys; ++i)
            ASSERT_EQ(store.put(make_key(i), make_val(i)).run_sync(), 0);

        for (int i = 0; i < kKeys + kNewKeys; ++i) {
            uint8_t val[128];
            size_t val_size = 0;
            int rc = store.get(make_key(i), val, sizeof(val),
                               &val_size).run_sync();
            ASSERT_EQ(rc, 0) << "key missing: " << make_key(i);
            EXPECT_EQ(
                std::string_view(reinterpret_cast<char*>(val), val_size),
                make_val(i));
        }
        store.close();
    }
}

static std::string get_value(uDepot<PosixIO>& store, const std::string& key) {
    uint8_t val[256];
    size_t n = 0;
    int rc = store.get(key, val, sizeof(val), &n).run_sync();
    if (rc != 0) return "<" + std::to_string(rc) + ">";
    return std::string(reinterpret_cast<char*>(val), n);
}

// Paper §4.5: a put writes its record before it checks its condition, so a
// rejected conditional put leaves a record in the log. After a clean
// shutdown the persisted index decides, and the rejected writes stay
// rejected (PR #3 review, finding 1). After a crash the log scan decides,
// and they may come back, which the paper accepts.
TEST_P(StoreRecoveryTest, RejectedConditionalPutsAfterReopen) {
    {
        uDepot<PosixIO> store;
        ASSERT_EQ(store.open(config()), 0);
        ASSERT_EQ(store.put("exists", "first").run_sync(), 0);
        ASSERT_EQ(store.put("exists", "rejected", udepot::PutMode::kCreate)
                      .run_sync(), -EEXIST);
        ASSERT_EQ(store.put("missing", "rejected", udepot::PutMode::kReplace)
                      .run_sync(), -ENOENT);
        ASSERT_EQ(store.put("cas", "first").run_sync(), 0);
        uint64_t version = 0;
        uint8_t buf[16];
        size_t n = 0;
        ASSERT_EQ(store.get("cas", buf, sizeof(buf), &n, &version).run_sync(),
                  0);
        ASSERT_EQ(store.put("cas", "rejected", udepot::PutMode::kUpsert,
                            version + 1).run_sync(), -ESTALE);
        end_session(store);
    }

    uDepot<PosixIO> store;
    ASSERT_EQ(store.open(reopen_config()), 0);
    if (GetParam() == Shutdown::kClean) {
        EXPECT_EQ(get_value(store, "exists"), "first");
        EXPECT_EQ(get_value(store, "missing"), "<" + std::to_string(-ENOENT) + ">");
        EXPECT_EQ(get_value(store, "cas"), "first");
    } else {
        for (const char* key : {"exists", "cas"}) {
            std::string v = get_value(store, key);
            EXPECT_TRUE(v == "first" || v == "rejected") << key << ": " << v;
        }
    }
    store.close();
}

// Deleted keys keep their tombstones' grains valid across the restore:
// re-putting them and churning the device past its size invalidates those
// grains, which salsa's (Debug) accounting checks.
TEST_P(StoreRecoveryTest, TombstonesSurviveReopenAndChurn) {
    constexpr int kKeys = 300;
    {
        uDepot<PosixIO> store;
        ASSERT_EQ(store.open(config()), 0);
        for (int i = 0; i < kKeys; ++i)
            ASSERT_EQ(store.put(make_key(i), make_val(i)).run_sync(), 0);
        for (int i = 0; i < kKeys; i += 2)
            ASSERT_EQ(store.del(make_key(i)).run_sync(), 0);
        end_session(store);
    }
    {
        uDepot<PosixIO> store;
        ASSERT_EQ(store.open(reopen_config()), 0);
        for (int i = 0; i < kKeys; ++i)
            ASSERT_EQ(get_value(store, make_key(i)),
                      i % 2 ? make_val(i)
                            : "<" + std::to_string(-ENOENT) + ">")
                << make_key(i);
        // Several times the device: GC reclaims everything written before.
        for (int round = 1; round <= 30; ++round)
            for (int i = 0; i < kKeys; ++i)
                ASSERT_EQ(store.put(make_key(i), make_val(i + round * kKeys))
                              .run_sync(), 0);
        for (int i = 0; i < kKeys; i += 3)
            ASSERT_EQ(store.del(make_key(i)).run_sync(), 0);
        end_session(store);
    }
    uDepot<PosixIO> store;
    ASSERT_EQ(store.open(reopen_config()), 0);
    for (int i = 0; i < kKeys; ++i)
        EXPECT_EQ(get_value(store, make_key(i)),
                  i % 3 ? make_val(i + 30 * kKeys)
                        : "<" + std::to_string(-ENOENT) + ">")
            << make_key(i);
    store.close();
}

// The directory comes back at the size it had grown to.
TEST_P(StoreRecoveryTest, GrownDirectorySurvivesReopen) {
    config_.index_bits = 6;
    constexpr int kKeys = 1500;
    uint32_t tables = 0;
    {
        uDepot<PosixIO> store;
        ASSERT_EQ(store.open(config()), 0);
        for (int i = 0; i < kKeys; ++i)
            ASSERT_EQ(store.put(make_key(i), make_val(i)).run_sync(), 0);
        tables = store.directory().num_tables();
        ASSERT_GT(tables, 2u);
        end_session(store);
    }
    uDepot<PosixIO> store;
    ASSERT_EQ(store.open(reopen_config()), 0);
    if (GetParam() == Shutdown::kClean) {
        EXPECT_EQ(store.directory().num_tables(), tables);
    }
    for (int i = 0; i < kKeys; ++i)
        ASSERT_EQ(get_value(store, make_key(i)), make_val(i)) << make_key(i);
    store.close();
}

// A table larger than a segment spans several index segments.
TEST_P(StoreRecoveryTest, TableLargerThanASegmentSurvivesReopen) {
    config_.size = 32 * 1024 * 1024 + 4096;
    config_.segment_size = 2048;  // 1 MiB: a 2^18-bucket table needs three
    config_.index_bits = 18;
    {
        uDepot<PosixIO> store;
        ASSERT_EQ(store.open(config()), 0);
        for (int i = 0; i < 200; ++i)
            ASSERT_EQ(store.put(make_key(i), make_val(i)).run_sync(), 0);
        ASSERT_EQ(store.put(make_key(0), "rejected", udepot::PutMode::kCreate)
                      .run_sync(), -EEXIST);
        end_session(store);
    }
    uDepot<PosixIO> store;
    ASSERT_EQ(store.open(reopen_config()), 0);
    for (int i = (GetParam() == Shutdown::kClean ? 0 : 1); i < 200; ++i)
        ASSERT_EQ(get_value(store, make_key(i)), make_val(i)) << make_key(i);
    store.close();
}

// The device metadata lives in the tail past the last whole segment. A
// device that is an exact multiple of the segment size has no tail: the
// metadata sat inside the last segment, so filling the device overwrote it
// and the next open found an empty store (or close() overwrote records).
// As uDepot, open now picks a segment size that leaves a tail.
TEST_P(StoreRecoveryTest, DeviceMetadataSurvivesAFullDevice) {
    config_.segment_size = 256;  // 128 KiB: divides the 4 MiB store exactly
    constexpr int kKeys = 200;
    {
        uDepot<PosixIO> store;
        ASSERT_EQ(store.open(config()), 0);
        for (int round = 0; round < 40; ++round)
            for (int i = 0; i < kKeys; ++i)
                ASSERT_EQ(store.put(make_key(i), make_val(i + round * kKeys))
                              .run_sync(), 0);
        end_session(store);
    }
    uDepot<PosixIO> store;
    ASSERT_EQ(store.open(reopen_config()), 0);
    for (int i = 0; i < kKeys; ++i)
        ASSERT_EQ(get_value(store, make_key(i)), make_val(i + 39 * kKeys))
            << make_key(i);
    store.close();
}

INSTANTIATE_TEST_SUITE_P(
    Shutdowns, StoreRecoveryTest,
    ::testing::Values(Shutdown::kClean, Shutdown::kCrash),
    [](const ::testing::TestParamInfo<Shutdown>& info) {
        return info.param == Shutdown::kClean ? "Clean" : "Crash";
    });

// Tests of the index itself, which only a clean shutdown writes.
class StoreIndexTest : public StoreRecoveryTest {};

// A store created over an earlier one (force_destroy) restarts its segment
// timestamps from the same values, and its segments reuse the same places
// on the device. A record's checksum was bound to neither its segment's
// timestamp nor the device seed, so a crash brought back records the old
// store had left past what the new one had written: keys it never put. As
// uDepot's, the checksum now covers both, and each store gets a fresh seed
// (the monotonic clock's seconds repeated for stores created within one).
TEST_P(StoreIndexTest, RecreatedStoreDoesNotRecoverTheOldStoresRecords) {
    config_.size = 32 * 1024 * 1024 + 4096;
    config_.segment_size = 2048;  // 1 MiB
    const std::string big(3000, 'o');
    {
        uDepot<PosixIO> store;
        ASSERT_EQ(store.open(config()), 0);
        for (int i = 0; i < 5000; ++i)
            ASSERT_EQ(store.put("old" + std::to_string(i), big).run_sync(), 0);
        store.close();
    }
    {
        uDepot<PosixIO> store;
        ASSERT_EQ(store.open(config()), 0);  // force_destroy: a new store
        for (int i = 0; i < 200; ++i)
            ASSERT_EQ(store.put(make_key(i), make_val(i)).run_sync(), 0);
        crash(store);
    }
    uDepot<PosixIO> store;
    ASSERT_EQ(store.open(reopen_config()), 0);
    int resurrected = 0;
    for (int i = 0; i < 5000; ++i)
        if (get_value(store, "old" + std::to_string(i))[0] != '<')
            ++resurrected;
    EXPECT_EQ(resurrected, 0) << "keys of the old store came back";
    for (int i = 0; i < 200; ++i)
        EXPECT_EQ(get_value(store, make_key(i)), make_val(i)) << make_key(i);
    store.close();
}

// The open that restores an index clears it: after writes and a crash, the
// log decides, not the older index. The old index's segments must outlive
// the second session for this to show, so the first session churns the
// device: its index then goes to segments the next session does not reuse
// first (salsa rebuilds its free list in segment order).
TEST_P(StoreIndexTest, RestoredIndexIsNotRestoredAgainAfterACrash) {
    config_.size = 32 * 1024 * 1024 + 4096;
    config_.segment_size = 2048;  // 1 MiB
    config_.initial_tables = 1;
    constexpr int kKeys = 2000;
    const std::string big(1500, 'v');
    {
        uDepot<PosixIO> store;
        ASSERT_EQ(store.open(config()), 0);
        for (int round = 0; round < 15; ++round)
            for (int i = 0; i < kKeys; ++i)
                ASSERT_EQ(store.put(make_key(i), big + std::to_string(round))
                              .run_sync(), 0);
        ASSERT_EQ(store.put("k", "before").run_sync(), 0);
        store.close();
    }
    {
        uDepot<PosixIO> store;
        ASSERT_EQ(store.open(reopen_config()), 0);
        ASSERT_EQ(get_value(store, "k"), "before");
        ASSERT_EQ(store.put("k", "after").run_sync(), 0);
        ASSERT_EQ(store.put("new", "after").run_sync(), 0);
        crash(store);
    }
    uDepot<PosixIO> store;
    ASSERT_EQ(store.open(reopen_config()), 0);
    EXPECT_EQ(get_value(store, "k"), "after");
    EXPECT_EQ(get_value(store, "new"), "after");
    store.close();
}

// A flush cut short leaves an incomplete index; the next open must not use
// it, and recovers from the log instead.
TEST_P(StoreIndexTest, IncompleteIndexFallsBackToTheLog) {
    static int footers;
    footers = 0;
    uDepot<PosixIO>::index_footer_test_hook = [] { return footers++ < 1; };
    {
        uDepot<PosixIO> store;
        ASSERT_EQ(store.open(config()), 0);  // two tables: two footers
        for (int i = 0; i < 100; ++i)
            ASSERT_EQ(store.put(make_key(i), make_val(i)).run_sync(), 0);
        ASSERT_EQ(store.put(make_key(0), "rejected", udepot::PutMode::kCreate)
                      .run_sync(), -EEXIST);
        store.close();
    }
    uDepot<PosixIO>::index_footer_test_hook = nullptr;
    ASSERT_EQ(footers, 2);

    uDepot<PosixIO> store;
    ASSERT_EQ(store.open(reopen_config()), 0);
    for (int i = 1; i < 100; ++i)
        ASSERT_EQ(get_value(store, make_key(i)), make_val(i)) << make_key(i);
    // Only the log holds the rejected record: seeing it proves the log
    // was scanned rather than the half-written index restored.
    EXPECT_EQ(get_value(store, make_key(0)), "rejected");
    store.close();
}

INSTANTIATE_TEST_SUITE_P(Index, StoreIndexTest,
                         ::testing::Values(Shutdown::kClean));

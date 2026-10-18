// Copyright (c) 2024-2026 Nikolas Ioannou
// SPDX-License-Identifier: BSD-3-Clause

#include "udepot/store.h"
#include "udepot/io/posix.h"
#include "udepot/table_region.h"

#include <algorithm>
#include <atomic>
#include <cstdint>
#include <cstring>
#include <filesystem>
#include <set>
#include <string>
#include <thread>
#include <vector>

#include <sys/mman.h>
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
        config_.force_destroy = true;
    }

    void TearDown() override {
        uDepot<PosixIO>::index_footer_test_hook = nullptr;
        uDepot<PosixIO>::index_format_test_hook = nullptr;
        uDepot<PosixIO>::index_segment_test_hook = nullptr;
        uDepot<PosixIO>::internal_io_test_hook = nullptr;
        uDepot<PosixIO>::gc_tombstone_drop_test_hook = nullptr;
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
    config_.size = 16 * 1024 * 1024 + 4096;
    config_.segment_size = 16;  // 8 KiB: tables of 512 buckets
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
    EXPECT_EQ(store.index_restored(), GetParam() == Shutdown::kClean);
    if (GetParam() == Shutdown::kClean) {
        EXPECT_EQ(store.directory().num_tables(), tables);
    }
    for (int i = 0; i < kKeys; ++i)
        ASSERT_EQ(get_value(store, make_key(i)), make_val(i)) << make_key(i);
    // Whichever way it came back, the directory has its segments again
    // (crash recovery builds it in memory first), so a clean close
    // persists it.
    tables = store.directory().num_tables();
    store.close();
    uDepot<PosixIO> again;
    ASSERT_EQ(again.open(reopen_config()), 0);
    EXPECT_TRUE(again.index_restored());
    EXPECT_EQ(again.directory().num_tables(), tables);
    for (int i = 0; i < kKeys; ++i)
        ASSERT_EQ(get_value(again, make_key(i)), make_val(i)) << make_key(i);
    again.close();
}

// As in uDepot, a table fills its segment: the segment size sets the
// table's width, whatever the device, and a table never spans segments.
TEST_P(StoreRecoveryTest, SegmentSizeSetsTheTableSize) {
    for (uint64_t seg : {16u, 64u, 2048u}) {
        config_.size = 32 * 1024 * 1024 + 4096;
        config_.segment_size = seg;
        uDepot<PosixIO> store;
        ASSERT_EQ(store.open(config()), 0);
        const auto& table = *store.directory().snapshot().tables[0];
        const size_t net = (seg - 1) * config_.grain_size;  // 1 md grain
        EXPECT_EQ(table.index_bits(), udepot::HashTable::index_bits_for(net));
        EXPECT_EQ(table.region().net_bytes(), net);
        EXPECT_EQ(table.region().map_bytes(), seg * config_.grain_size);
        for (int i = 0; i < 50; ++i)
            ASSERT_EQ(store.put(make_key(i), make_val(i)).run_sync(), 0);
        end_session(store);
        uDepot<PosixIO> again;
        ASSERT_EQ(again.open(reopen_config()), 0);
        for (int i = 0; i < 50; ++i)
            ASSERT_EQ(get_value(again, make_key(i)), make_val(i));
        again.close();
    }
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
    config_.size = 16 * 1024 * 1024 + 4096;
    config_.segment_size = 16;  // 8 KiB: tables of 512 buckets
    {
        uDepot<PosixIO> store;
        ASSERT_EQ(store.open(config()), 0);
        for (int i = 0; i < 1000; ++i)
            ASSERT_EQ(store.put(make_key(i), make_val(i)).run_sync(), 0);
        ASSERT_GE(store.directory().num_tables(), 2u);  // two footers at least
        ASSERT_EQ(store.put(make_key(0), "rejected", udepot::PutMode::kCreate)
                      .run_sync(), -EEXIST);
        store.close();
    }
    uDepot<PosixIO>::index_footer_test_hook = nullptr;
    ASSERT_EQ(footers, 2);

    uDepot<PosixIO> store;
    ASSERT_EQ(store.open(reopen_config()), 0);
    EXPECT_FALSE(store.index_restored());
    for (int i = 1; i < 1000; ++i)
        ASSERT_EQ(get_value(store, make_key(i)), make_val(i)) << make_key(i);
    // Only the log holds the rejected record: seeing it proves the log
    // was scanned rather than the half-written index restored.
    EXPECT_EQ(get_value(store, make_key(0)), "rejected");
    store.close();
}

// An index persisted in another format is not restored: before PR #9 a
// tag was the hash's top 8 bits, and read as uDepot's (the 8 above the
// bucket) such an index would miss keys it holds. The log scan rebuilds
// it, and the next close writes the current format.
TEST_P(StoreIndexTest, AnIndexOfAnotherFormatIsRebuiltFromTheLog) {
    config_.size = 16 * 1024 * 1024 + 4096;
    config_.segment_size = 16;  // 8 KiB: tables of 512 buckets
    uDepot<PosixIO>::index_format_test_hook = [](uint32_t) {
        return uint32_t{0};
    };
    {
        uDepot<PosixIO> store;
        ASSERT_EQ(store.open(config()), 0);
        for (int i = 0; i < 1000; ++i)
            ASSERT_EQ(store.put(make_key(i), make_val(i)).run_sync(), 0);
        store.close();
    }
    uDepot<PosixIO>::index_format_test_hook = nullptr;
    {
        uDepot<PosixIO> store;
        ASSERT_EQ(store.open(reopen_config()), 0);
        EXPECT_FALSE(store.index_restored());
        for (int i = 0; i < 1000; ++i)
            ASSERT_EQ(get_value(store, make_key(i)), make_val(i)) << make_key(i);
        store.close();
    }
    uDepot<PosixIO> store;
    ASSERT_EQ(store.open(reopen_config()), 0);
    EXPECT_TRUE(store.index_restored());
    for (int i = 0; i < 1000; ++i)
        ASSERT_EQ(get_value(store, make_key(i)), make_val(i)) << make_key(i);
    store.close();
}

// As uDepot's directory, each table keeps one index segment for its life:
// close() writes it back there, and the next open restores it from there.
TEST_P(StoreIndexTest, TablesKeepTheirSegmentsAcrossReopen) {
    config_.size = 16 * 1024 * 1024 + 4096;
    config_.segment_size = 16;  // 8 KiB: tables of 512 buckets
    auto grains = [](uDepot<PosixIO>& store) {
        std::set<uint64_t> g;
        for (auto& t : store.directory().snapshot().tables)
            g.insert(t->region().grain());
        return g;
    };
    std::set<uint64_t> before;
    {
        uDepot<PosixIO> store;
        ASSERT_EQ(store.open(config()), 0);
        for (int i = 0; i < 1500; ++i)
            ASSERT_EQ(store.put(make_key(i), make_val(i)).run_sync(), 0);
        before = grains(store);
        ASSERT_GE(before.size(), 4u);
        ASSERT_EQ(before.size(), store.directory().num_tables())
            << "two tables share a segment";
        store.close();
    }
    uDepot<PosixIO> store;
    ASSERT_EQ(store.open(reopen_config()), 0);
    EXPECT_TRUE(store.index_restored());
    EXPECT_EQ(grains(store), before);
    for (int i = 0; i < 1500; ++i)
        ASSERT_EQ(get_value(store, make_key(i)), make_val(i)) << make_key(i);
    // And they are still the store's for the session: nothing else may
    // take their segments, writes and a grow go on, and the next close
    // writes them back to the same place.
    for (int i = 1500; i < 3000; ++i)
        ASSERT_EQ(store.put(make_key(i), make_val(i)).run_sync(), 0);
    std::set<uint64_t> grown = grains(store);
    store.close();
    uDepot<PosixIO> again;
    ASSERT_EQ(again.open(reopen_config()), 0);
    EXPECT_TRUE(again.index_restored());
    EXPECT_EQ(grains(again), grown);
    for (int i = 0; i < 3000; ++i)
        ASSERT_EQ(get_value(again, make_key(i)), make_val(i)) << make_key(i);
    again.close();
}

// A restored table's segment stays the table's: with the device churned
// several times over, every free segment is reused for data, and one the
// table had lost would have its records overwritten by the table at close,
// or the table by them.
TEST_P(StoreIndexTest, RestoredTablesKeepTheirSegmentsUnderChurn) {
    config_.size = 4 * 1024 * 1024 + 4096;
    config_.segment_size = 16;  // 8 KiB: tables of 512 buckets
    constexpr int kKeys = 1500;
    {
        uDepot<PosixIO> store;
        ASSERT_EQ(store.open(config()), 0);
        for (int i = 0; i < kKeys; ++i)
            ASSERT_EQ(store.put(make_key(i), make_val(i)).run_sync(), 0);
        store.close();
    }
    constexpr int kRounds = 16;  // about three times the device
    {
        uDepot<PosixIO> store;
        ASSERT_EQ(store.open(reopen_config()), 0);
        ASSERT_TRUE(store.index_restored());
        for (int round = 1; round <= kRounds; ++round)
            for (int i = 0; i < kKeys; ++i)
                ASSERT_EQ(store.put(make_key(i), make_val(i + round * kKeys))
                              .run_sync(), 0);
        store.close();
    }
    uDepot<PosixIO> store;
    ASSERT_EQ(store.open(reopen_config()), 0);
    EXPECT_TRUE(store.index_restored());
    for (int i = 0; i < kKeys; ++i)
        ASSERT_EQ(get_value(store, make_key(i)), make_val(i + kRounds * kKeys))
            << make_key(i);
    store.close();
}

// As uDepot's KV_conf::sanitize_segment_size, a store's segment size is
// whole pages.
TEST_P(StoreIndexTest, SegmentSizeIsWholePages) {
    config_.size = 64 * 1024 * 1024 + 4096;
    config_.segment_size = 4096 + 2;  // 2 MiB + 2 grains
    uDepot<PosixIO> store;
    ASSERT_EQ(store.open(config()), 0);
    const auto& region = store.directory().snapshot().tables[0]->region();
    EXPECT_EQ(region.map_bytes(), size_t{4096} * config_.grain_size);
    EXPECT_EQ(region.map_bytes() % udepot::TableRegion::kHugePage, 0u);
    store.close();
}

// A fitted segment's table holds exactly the buckets it was fitted for:
// the next power of two does not fit, so the table fills more than half
// of it, and all but the room it needs (header, footer, last neighborhood,
// salsa metadata, rounded up to a page) when the table is large. A segment
// of a power of two of bytes held half the table it was meant for.
// The bindings size a store by its tables: 0 is the store's default
// segment, a power of two of at least 4 KiB its fitted one, anything else
// -EINVAL.
TEST(FittedSegmentSize, SegmentSizeForTable) {
    uint64_t seg = 1;
    EXPECT_EQ(udepot::segment_size_for_table(0, 4096, &seg), 0);
    EXPECT_EQ(seg, 0u);
    EXPECT_EQ(udepot::segment_size_for_table(size_t{2} << 20, 4096, &seg), 0);
    EXPECT_EQ(seg, udepot::fitted_segment_size(size_t{2} << 20, 4096));
    EXPECT_EQ(udepot::segment_size_for_table(4096, 512, &seg), 0);
    EXPECT_EQ(seg, udepot::fitted_segment_size(4096, 512));
    seg = 7;
    EXPECT_EQ(udepot::segment_size_for_table(2048, 512, &seg), -EINVAL);
    EXPECT_EQ(udepot::segment_size_for_table(6 << 20, 512, &seg), -EINVAL);
    EXPECT_EQ(seg, 7u);  // untouched on failure
}

TEST(FittedSegmentSize, TheTableFillsItsSegment) {
    for (uint32_t grain : {512u, 4096u}) {
        const size_t md = std::max<size_t>(512, grain);  // salsa md grain
        for (int k = 12; k <= 29; ++k) {
            SCOPED_TRACE("grain " + std::to_string(grain) + ", table 2^" +
                         std::to_string(k) + " bytes");
            const uint64_t table = uint64_t{1} << k;
            const uint64_t seg = udepot::fitted_segment_size(table, grain);
            const uint64_t seg_bytes = seg * grain;
            EXPECT_EQ(seg_bytes % 4096, 0u);  // whole pages
            const uint32_t bits = udepot::HashTable::index_bits_for(
                static_cast<size_t>(seg_bytes - md));
            EXPECT_EQ(uint64_t{8} << bits, table);  // 8-byte slots
            EXPECT_LE(seg_bytes - table, 2 * std::max<uint64_t>(4096, grain));
        }
        // A power of two of bytes holds half that table.
        EXPECT_EQ(udepot::HashTable::index_bits_for((size_t{1} << 25) - md),
                  21u);
        EXPECT_EQ(udepot::HashTable::index_bits_for(
                      udepot::fitted_segment_size(size_t{1} << 25, grain) *
                          grain -
                      md),
                  22u);
    }
}

// The default segment, halved until it fits the device, is a fitted one:
// its table fills it. Also on devices that are an exact multiple of a
// fitted size, which leave no tail for the device metadata at that size:
// the search for a size that does must stay among fitted ones (it once
// settled on a power of two of bytes, half of whose table went unused).
// A segment whose tables would waste more than 1/8 of a huge-page
// mapping maps them on 4 KiB pages, and open() says so, naming what that
// costs: slower grows first. The bindings' (a 2 MiB table, 4 KiB grains)
// is one; a 16 MiB table's segment keeps huge pages and says nothing.
TEST_P(StoreIndexTest, OpenSaysWhenTablesMissHugePages) {
    config_.grain_size = 4096;
    auto open_log = [&](size_t table_bytes) {
        const uint64_t seg =
            udepot::fitted_segment_size(table_bytes, config_.grain_size);
        config_.segment_size = seg;
        config_.size = 32 * seg * config_.grain_size + 4096;
        uDepot<PosixIO> store;
        ::testing::internal::CaptureStderr();
        const int rc = store.open(config());
        std::string log = ::testing::internal::GetCapturedStderr();
        EXPECT_EQ(rc, 0);
        if (rc == 0) {
            const auto& region =
                store.directory().snapshot().tables[0]->region();
            if (!udepot::TableRegion::wants_huge_pages(seg *
                                                       config_.grain_size)) {
                EXPECT_FALSE(region.huge());
                EXPECT_EQ(region.map_bytes(), seg * config_.grain_size);
            }
            store.close();
        }
        std::filesystem::remove(path_);
        return log;
    };
    const std::string small = open_log(size_t{2} << 20);
    EXPECT_NE(small.find("index tables map on 4 KiB pages"), std::string::npos)
        << small;
    EXPECT_NE(small.find("directory grows to be slower"), std::string::npos)
        << small;
    const std::string large = open_log(size_t{16} << 20);
    EXPECT_EQ(large.find("index tables map on 4 KiB pages"),
              std::string::npos)
        << large;
}

// A device too small for the segment asked for, or the default, makes it
// smaller, and every table with it; open() says so, naming both. A device
// with room for it says nothing.
TEST_P(StoreIndexTest, OpenSaysWhenTheDeviceShrinksTheSegment) {
    config_.grain_size = 4096;
    auto open_log = [&](uint64_t segment_size, size_t device) {
        config_.segment_size = segment_size;
        config_.size = device;
        uDepot<PosixIO> store;
        ::testing::internal::CaptureStderr();
        const int rc = store.open(config());
        std::string log = ::testing::internal::GetCapturedStderr();
        EXPECT_EQ(rc, 0);
        if (rc == 0) {
            EXPECT_EQ(store.put("k", "v").run_sync(), 0);
            store.close();
        }
        std::filesystem::remove(path_);
        return log;
    };
    const uint64_t seg =
        udepot::fitted_segment_size(size_t{2} << 20, config_.grain_size);
    const std::string small = open_log(seg, 4 << 20);
    EXPECT_NE(small.find("too small for the requested segments of 2056 KiB"),
              std::string::npos) << small;
    const std::string fits =
        open_log(seg, 32 * seg * config_.grain_size + 4096);
    EXPECT_EQ(fits.find("too small"), std::string::npos) << fits;
    const std::string dflt = open_log(0, 4 << 20);
    EXPECT_NE(dflt.find("too small for the default segments"),
              std::string::npos) << dflt;
}

// No silent fallback: a table that wants huge pages and cannot have them
// (none free) maps on 4 KiB pages, and the open says so, once, whether it
// creates the table or restores it; the next open says it again. With
// huge pages free, nothing is said.
TEST_P(StoreIndexTest, OpenSaysWhenNoHugePagesAreFree) {
    config_.grain_size = 4096;
    const uint64_t seg =
        udepot::fitted_segment_size(size_t{16} << 20, config_.grain_size);
    const size_t seg_bytes = seg * config_.grain_size;
    ASSERT_TRUE(udepot::TableRegion::wants_huge_pages(seg_bytes));
    config_.segment_size = seg;
    config_.size = 32 * seg_bytes + 4096;

    auto count = [](const std::string& log) {
        int n = 0;
        for (size_t at = 0;
             (at = log.find("no 2 MiB huge pages", at)) != std::string::npos;
             ++at)
            ++n;
        return n;
    };
    for (int round = 0; round < 2; ++round) {
        uDepot<PosixIO> store;
        ::testing::internal::CaptureStderr();
        const int rc = store.open(round == 0 ? config() : reopen_config());
        const std::string log = ::testing::internal::GetCapturedStderr();
        ASSERT_EQ(rc, 0);
        // Whether it got huge pages, rather than a probe beforehand that a
        // test running alongside could invalidate.
        const auto& region = store.directory().snapshot().tables[0]->region();
        EXPECT_EQ(count(log), region.huge() ? 0 : 1) << log;
        if (!region.huge()) {
            EXPECT_NE(region.huge_error(), 0);
            EXPECT_NE(log.find("directory grows to be slower"),
                      std::string::npos) << log;
        }
        ASSERT_EQ(store.put("k", "v").run_sync(), 0);
        store.close();
    }
}

TEST_P(StoreIndexTest, TheDefaultSegmentsTableFillsIt) {
    std::vector<size_t> sizes = {256 * 1024 * 1024 + 4096};
    for (int k : {21, 22, 23})
        for (size_t n : {16u, 24u, 32u, 40u, 48u, 64u})
            sizes.push_back(n * udepot::fitted_segment_size(size_t{1} << k,
                                                            config_.grain_size) *
                            config_.grain_size);
    for (size_t size : sizes) {
        SCOPED_TRACE("device " + std::to_string(size) + " B");
        config_.size = size;
        config_.segment_size = 0;
        uDepot<PosixIO> store;
        ASSERT_EQ(store.open(config()), 0);
        const auto& table = *store.directory().snapshot().tables[0];
        const size_t used = 2 * udepot::TableRegion::kMdBytes +
                            table.total_slots() * sizeof(uint64_t);
        EXPECT_GT(used, table.region().net_bytes() * 9 / 10)
            << "a table of 2^" << table.index_bits() << " buckets in a net "
            << table.region().net_bytes() << " B segment";
        EXPECT_EQ(table.region().net_bytes() + config_.grain_size,
                  udepot::fitted_segment_size(size_t{8} << table.index_bits(),
                                              config_.grain_size) *
                      config_.grain_size);
        for (int i = 0; i < 100; ++i)
            ASSERT_EQ(store.put(make_key(i), make_val(i)).run_sync(), 0);
        store.close();
        uDepot<PosixIO> again;
        ASSERT_EQ(again.open(reopen_config()), 0);
        EXPECT_TRUE(again.index_restored());
        for (int i = 0; i < 100; ++i)
            ASSERT_EQ(get_value(again, make_key(i)), make_val(i));
        again.close();
        std::filesystem::remove(path_);
    }
}

// The index holds its segments for the whole session, so a store full of
// live data can still write it back at close. It used to take its
// segments only then, and a full store fell back to the log scan.
TEST_P(StoreIndexTest, AFullStoreStillPersistsItsIndex) {
    config_.segment_size = 64;  // 32 KiB
    const std::string val(200, 'f');  // get_value reads up to 256
    int written = 0;
    {
        uDepot<PosixIO> store;
        ASSERT_EQ(store.open(config()), 0);
        for (;; ++written) {
            int rc = store.put(make_key(written), val).run_sync();
            if (rc == -ENOSPC) break;
            ASSERT_EQ(rc, 0);
        }
        ASSERT_GT(written, 100);
        store.close();
    }
    uDepot<PosixIO> store;
    ASSERT_EQ(store.open(reopen_config()), 0);
    EXPECT_TRUE(store.index_restored());
    for (int i = 0; i < written; ++i)
        ASSERT_EQ(get_value(store, make_key(i)), val) << make_key(i);
    store.close();
}

// A grow that finds no index segment for its tables fails the put that
// needed it with -ENOSPC, as uDepot's put does, instead of retrying
// forever; the next put that finds its table full tries again.
TEST_P(StoreIndexTest, AGrowWithoutASegmentFailsThePut) {
    config_.size = 16 * 1024 * 1024 + 4096;
    config_.segment_size = 16;  // 8 KiB: tables of 512 buckets
    uDepot<PosixIO> store;
    ASSERT_EQ(store.open(config()), 0);
    uDepot<PosixIO>::index_segment_test_hook = [] { return -ENOSPC; };
    int i = 0;
    int rc = 0;
    for (; i < 5000; ++i) {
        rc = store.put(make_key(i), make_val(i)).run_sync();
        if (rc != 0) break;
    }
    uDepot<PosixIO>::index_segment_test_hook = nullptr;
    EXPECT_EQ(rc, -ENOSPC);
    EXPECT_EQ(store.directory().num_tables(), 1u);
    // With segments to be had again, the same put grows the directory.
    ASSERT_EQ(store.put(make_key(i), make_val(i)).run_sync(), 0);
    EXPECT_EQ(store.directory().num_tables(), 2u);
    for (int j = 0; j <= i; ++j)
        ASSERT_EQ(get_value(store, make_key(j)), make_val(j)) << make_key(j);
    store.close();
}

// A grow that finds no index segment free yet (-EAGAIN) is not a failure:
// as uDepot's grow() loops on EAGAIN, the waker tries again, letting every
// other waiter go on meanwhile, and the put that needed it succeeds.
TEST_P(StoreIndexTest, AGrowThatMustWaitForASegmentGoesOn) {
    config_.size = 16 * 1024 * 1024 + 4096;
    config_.segment_size = 16;  // 8 KiB: tables of 512 buckets
    static std::atomic<int> calls;
    calls = 0;
    uDepot<PosixIO> store;
    ASSERT_EQ(store.open(config()), 0);
    uDepot<PosixIO>::index_segment_test_hook = [] {
        return calls.fetch_add(1) < 50 ? -EAGAIN : 0;
    };
    int i = 0;
    for (; store.directory().num_tables() < 2; ++i) {
        ASSERT_LT(i, 5000);
        ASSERT_EQ(store.put(make_key(i), make_val(i)).run_sync(), 0) << i;
    }
    uDepot<PosixIO>::index_segment_test_hook = nullptr;
    EXPECT_GT(calls.load(), 50);
    for (int j = 0; j < i; ++j)
        ASSERT_EQ(get_value(store, make_key(j)), make_val(j)) << make_key(j);
    store.close();
}

// Every writer waiting on a grow that failed gets the failure, however
// many there are; none waits forever, and once segments can be had again
// the directory grows.
TEST_P(StoreIndexTest, WritersWaitingOnAFailedGrowAllGetENOSPC) {
    config_.size = 16 * 1024 * 1024 + 4096;
    config_.segment_size = 16;  // 8 KiB: tables of 512 buckets
    uDepot<PosixIO> store;
    ASSERT_EQ(store.open(config()), 0);
    uDepot<PosixIO>::index_segment_test_hook = [] { return -ENOSPC; };
    constexpr int kThreads = 4;
    std::vector<int> last_rc(kThreads, 0), written(kThreads, 0);
    std::vector<std::thread> threads;
    for (int t = 0; t < kThreads; ++t)
        threads.emplace_back([&, t] {
            for (int i = 0; i < 5000; ++i) {
                const std::string key = make_key(t * 100000 + i);
                last_rc[t] = store.put(key, make_val(i)).run_sync();
                if (last_rc[t] != 0) return;
                written[t] = i + 1;
            }
        });
    for (auto& th : threads) th.join();
    uDepot<PosixIO>::index_segment_test_hook = nullptr;
    for (int t = 0; t < kThreads; ++t)
        EXPECT_EQ(last_rc[t], -ENOSPC) << "writer " << t;
    EXPECT_EQ(store.directory().num_tables(), 1u);
    for (int t = 0; t < kThreads; ++t) {
        ASSERT_EQ(store.put(make_key(t * 100000 + written[t]),
                            make_val(written[t])).run_sync(), 0);
        for (int i = 0; i <= written[t]; ++i)
            ASSERT_EQ(get_value(store, make_key(t * 100000 + i)), make_val(i));
    }
    EXPECT_GE(store.directory().num_tables(), 2u);
    store.close();
}

// A grow frees the old tables' segments: churned enough, the device puts
// data where they were. (They used to be invalidated nowhere a test saw.)
TEST_P(StoreIndexTest, RetiredTablesFreeTheirSegments) {
    config_.size = 4 * 1024 * 1024 + 4096;
    config_.segment_size = 16;  // 8 KiB: tables of 512 buckets
    uDepot<PosixIO> store;
    ASSERT_EQ(store.open(config()), 0);
    const uint64_t seg = config_.segment_size;
    std::set<uint64_t> retired;  // segment indexes
    std::set<uint64_t> current;
    auto table_segs = [&] {
        std::set<uint64_t> s;
        for (auto& t : store.directory().snapshot().tables)
            s.insert(t->region().grain() / seg);
        return s;
    };
    current = table_segs();
    int i = 0;
    for (; store.directory().num_tables() < 4; ++i) {
        ASSERT_LT(i, 5000);
        ASSERT_EQ(store.put(make_key(i), make_val(i)).run_sync(), 0);
        std::set<uint64_t> now = table_segs();
        for (uint64_t s : current)
            if (!now.count(s)) retired.insert(s);
        current = now;
    }
    ASSERT_EQ(retired.size(), 3u);  // 1 -> 2 -> 4 tables
    // Churn a few keys until one's record lands in a retired segment.
    bool reused = false;
    for (int round = 0; round < 40000 && !reused; ++round) {
        const std::string key = "churn" + std::to_string(round % 64);
        ASSERT_EQ(store.put(key, make_val(round)).run_sync(), 0);
        const uint64_t hash = store.hash_key(uDepot<PosixIO>::as_bytes(key));
        uint32_t idx = store.rcu().read_lock();
        const uint64_t pba = store.directory().lookup(hash).pba();
        store.rcu().read_unlock(idx);
        reused = retired.count(pba / seg) != 0;
    }
    EXPECT_TRUE(reused) << "no retired table segment was ever reused";
    store.close();
}

// After a crash, recovery may need more tables than the session had (the
// log brings back records an index had rejected). If the device has no
// segment for them all, open fails with -ENOSPC, every time, and leaves
// the store for an operator to give it space; it does not open without a
// persistable index.
TEST_P(StoreIndexTest, CrashRecoveryWithNoRoomForItsIndexDoesNotOpen) {
    config_.size = 4 * 1024 * 1024 + 4096;
    config_.segment_size = 64;  // 32 KiB
    {
        uDepot<PosixIO> store;
        ASSERT_EQ(store.open(config()), 0);
        for (int i = 0; i < 3000; ++i)
            ASSERT_EQ(store.put(make_key(i), make_val(i)).run_sync(), 0);
        for (int i = 3000; i < 6500; ++i)
            ASSERT_EQ(store.put(make_key(i), make_val(i),
                                udepot::PutMode::kReplace).run_sync(),
                      -ENOENT);
        crash(store);
    }
    for (int attempt = 0; attempt < 2; ++attempt) {
        uDepot<PosixIO> store;
        EXPECT_EQ(store.open(reopen_config()), -ENOSPC) << attempt;
    }
}

// After a crash, the store restores to salsa as valid only the grains the
// log still holds live, so it takes as much new data as a fresh store less
// that. uDepot's crash path restored every segment in full before
// invalidating the dead grains: past the logical capacity salsa dropped the excess, and on a
// churned device with little live data the used count underflowed, so
// every write failed with -ENOSPC.
//
// Live includes the tombstones of deleted keys, and a key put again after
// its delete no longer holds its tombstone: both are counted right, or the
// fill is off by thousands of grains.
TEST_P(StoreIndexTest, CrashRecoveryRestoresOnlyLiveGrains) {
    config_.size = 4 * 1024 * 1024 + 4096;
    config_.segment_size = 64;  // 32 KiB
    auto fill = [](uDepot<PosixIO>& store, const std::string& prefix) {
        int n = 0;
        for (;; ++n) {
            int rc = store.put(prefix + std::to_string(n), "v").run_sync();
            if (rc == -ENOSPC) return n;
            EXPECT_EQ(rc, 0);
            if (rc != 0) return -1;
        }
    };
    int fresh = 0;
    {
        uDepot<PosixIO> store;
        ASSERT_EQ(store.open(config()), 0);
        fresh = fill(store, "fresh");
        store.close();
    }
    ASSERT_GT(fresh, 5000);
    constexpr int kLive = 100;
    constexpr int kRounds = 200;
    constexpr int kDeleted = 4000;  // put, then deleted
    constexpr int kReput = 2000;    // of those, put again
    auto del_key = [](int d) { return "del" + std::to_string(d); };
    // Tombstones of keys still deleted at the crash that GC dropped (lazy
    // reclaim): they are not live after it.
    static std::atomic<int> dropped;
    dropped = 0;
    {
        uDepot<PosixIO> store;
        ASSERT_EQ(store.open(config()), 0);  // a new store
        // About 2.5 times the device: every segment carries valid
        // metadata at the crash, and restored in full they exceed the
        // logical capacity.
        for (int round = 0; round < kRounds; ++round)
            for (int i = 0; i < kLive; ++i)
                ASSERT_EQ(store.put(make_key(i),
                                    make_val(i + round * kLive)).run_sync(),
                          0);
        for (int d = 0; d < kDeleted; ++d)
            ASSERT_EQ(store.put(del_key(d), "x").run_sync(), 0);
        uDepot<PosixIO>::gc_tombstone_drop_test_hook =
            [](std::span<const uint8_t> key, uint64_t) {
                const std::string k(key.begin(), key.end());
                if (k.starts_with("del") && std::stoi(k.substr(3)) >= kReput)
                    ++dropped;
            };
        for (int d = 0; d < kDeleted; ++d)
            ASSERT_EQ(store.del(del_key(d)).run_sync(), 0);
        for (int d = 0; d < kReput; ++d)
            ASSERT_EQ(store.put(del_key(d), "y").run_sync(), 0);
        crash(store);
        uDepot<PosixIO>::gc_tombstone_drop_test_hook = nullptr;
    }
    uDepot<PosixIO> store;
    ASSERT_EQ(store.open(reopen_config()), 0);
    ASSERT_FALSE(store.index_restored());
    for (int i = 0; i < kLive; ++i)
        ASSERT_EQ(get_value(store, make_key(i)),
                  make_val(i + (kRounds - 1) * kLive));
    for (int d = 0; d < kDeleted; ++d)
        ASSERT_EQ(get_value(store, del_key(d)), d < kReput ? std::string("y") : "<" + std::to_string(-ENOENT) + ">")
            << del_key(d);
    // Every record and tombstone here takes one grain, as a fill's put.
    const int live = kLive + kReput + (kDeleted - kReput - dropped.load());
    const int after = fill(store, "after");
    // GC's timing moves a fill by a few hundred either way.
    EXPECT_NEAR(after, fresh - live, fresh / 10)
        << "live " << live << ", fresh " << fresh;
    store.close();
}

// Rejected writes come back after a crash (paper §4.5). When the log then
// holds more live data than the store may, the device is full: the open
// fails with -ENOSPC, every time. (It used to trip an assert in salsa, or
// without asserts open with a broken used count.)
TEST_P(StoreIndexTest, CrashRecoveryBeyondCapacityDoesNotOpen) {
    config_.size = 4 * 1024 * 1024 + 4096;
    config_.segment_size = 64;  // 32 KiB
    {
        uDepot<PosixIO> store;
        ASSERT_EQ(store.open(config()), 0);
        for (int i = 0; i < 3000; ++i)
            ASSERT_EQ(store.put(make_key(i), make_val(i)).run_sync(), 0);
        for (int i = 3000; i < 7000; ++i)
            ASSERT_EQ(store.put(make_key(i), make_val(i),
                                udepot::PutMode::kReplace).run_sync(),
                      -ENOENT);
        crash(store);
    }
    for (int attempt = 0; attempt < 2; ++attempt) {
        uDepot<PosixIO> store;
        testing::internal::CaptureStderr();
        const int rc = store.open(reopen_config());
        const std::string err = testing::internal::GetCapturedStderr();
        EXPECT_EQ(rc, -ENOSPC) << attempt;
        EXPECT_NE(err.find("grains of live data, more than the store's "
                           "capacity"),
                  std::string::npos)
            << "failed for another reason: " << err;
    }
}

// Just past the capacity, by no more than salsa lets relocation overshoot
// it (restore_grain_range provisions as relocation does): the open fails for
// the capacity. It used to restore the data, then fail for want of
// segments for the index, which named the wrong cause.
TEST_P(StoreIndexTest, CrashRecoveryJustBeyondCapacityNamesTheCapacity) {
    config_.size = 4 * 1024 * 1024 + 4096;
    config_.segment_size = 64;  // 32 KiB
    // Salsa's capacity, as Scm::init computes it; every record here takes
    // one grain.
    const int capacity = static_cast<int>(
        config_.size / config_.grain_size * (1000 - config_.overprovision) /
        1000);
    constexpr int kLive = 3000;
    const int rejected = capacity - kLive + 16;
    {
        uDepot<PosixIO> store;
        ASSERT_EQ(store.open(config()), 0);
        for (int i = 0; i < kLive; ++i)
            ASSERT_EQ(store.put(make_key(i), make_val(i)).run_sync(), 0);
        for (int i = kLive; i < kLive + rejected; ++i)
            ASSERT_EQ(store.put(make_key(i), make_val(i),
                                udepot::PutMode::kReplace).run_sync(),
                      -ENOENT);
        crash(store);
    }
    uDepot<PosixIO> store;
    testing::internal::CaptureStderr();
    const int rc = store.open(reopen_config());
    const std::string err = testing::internal::GetCapturedStderr();
    EXPECT_EQ(rc, -ENOSPC);
    unsigned long live = 0, cap = 0;
    const size_t at = err.find("crash recovery found");
    ASSERT_NE(at, std::string::npos) << "failed for another reason: " << err;
    ASSERT_EQ(std::sscanf(err.c_str() + at,
                          "crash recovery found %lu grains of live data, "
                          "more than the store's capacity of %lu",
                          &live, &cap),
              2)
        << err;
    // The case this covers: within what relocation may overshoot.
    EXPECT_GT(live, cap);
    EXPECT_LE(live, cap + 2 * config_.segment_size) << err;
}

// A read error while open looks for the index fails the open. It used to
// pass over the segment and scan the log, leaving that segment's footer
// valid: after more writes and a crash, the next open restored the old
// index, and every write since was lost.
TEST_P(StoreIndexTest, AReadErrorWhileRestoringTheIndexFailsTheOpen) {
    config_.size = 16 * 1024 * 1024 + 4096;
    config_.segment_size = 16;  // 8 KiB: tables of 512 buckets
    constexpr int kKeys = 100;
    static off_t header;
    {
        uDepot<PosixIO> store;
        ASSERT_EQ(store.open(config()), 0);
        for (int i = 0; i < kKeys; ++i)
            ASSERT_EQ(store.put(make_key(i), make_val(i)).run_sync(), 0);
        header = static_cast<off_t>(
                     store.directory().snapshot().tables[0]->region().grain()) *
                 config_.grain_size;
        store.close();
    }
    uDepot<PosixIO>::internal_io_test_hook = [](bool write, off_t off,
                                                size_t) {
        return !write && off == header ? -EIO : 0;
    };
    {
        uDepot<PosixIO> store;
        EXPECT_EQ(store.open(reopen_config()), -EIO);
    }
    uDepot<PosixIO>::internal_io_test_hook = nullptr;
    {
        uDepot<PosixIO> store;
        ASSERT_EQ(store.open(reopen_config()), 0);
        EXPECT_TRUE(store.index_restored());
        for (int i = 0; i < kKeys; ++i)
            ASSERT_EQ(get_value(store, make_key(i)), make_val(i));
        ASSERT_EQ(store.put(make_key(0), "after").run_sync(), 0);
        ASSERT_EQ(store.put("new", "after").run_sync(), 0);
        crash(store);
    }
    uDepot<PosixIO> store;
    ASSERT_EQ(store.open(reopen_config()), 0);
    EXPECT_FALSE(store.index_restored());
    EXPECT_EQ(get_value(store, make_key(0)), "after");
    EXPECT_EQ(get_value(store, "new"), "after");
    for (int i = 1; i < kKeys; ++i)
        ASSERT_EQ(get_value(store, make_key(i)), make_val(i));
    store.close();
}

// A segment whose metadata cannot be read fails crash recovery's open. It
// used to be skipped: its records were lost, and the segment free to be
// written over.
TEST_P(StoreIndexTest, AReadErrorInCrashRecoveryFailsTheOpen) {
    config_.size = 4 * 1024 * 1024 + 4096;
    config_.segment_size = 64;  // 32 KiB
    constexpr int kKeys = 500;
    {
        uDepot<PosixIO> store;
        ASSERT_EQ(store.open(config()), 0);
        for (int i = 0; i < kKeys; ++i)
            ASSERT_EQ(store.put(make_key(i), make_val(i)).run_sync(), 0);
        crash(store);
    }
    // Only crash recovery reads a segment's records: fail the metadata
    // read that follows the first such read.
    static uint64_t seg_bytes;
    static std::atomic<int> state;  // 0: before, 1: armed, 2: failed one
    seg_bytes = config_.segment_size * config_.grain_size;
    state = 0;
    uDepot<PosixIO>::internal_io_test_hook = [](bool write, off_t off,
                                                size_t count) {
        if (write) return 0;
        const uint64_t end = static_cast<uint64_t>(off) + count;
        if (off % seg_bytes == 0 && count > 4096 && state == 0) state = 1;
        else if (end % seg_bytes == 0 && count < seg_bytes && state == 1) {
            state = 2;
            return -EIO;
        }
        return 0;
    };
    {
        uDepot<PosixIO> store;
        EXPECT_EQ(store.open(reopen_config()), -EIO);
    }
    uDepot<PosixIO>::internal_io_test_hook = nullptr;
    ASSERT_EQ(state.load(), 2);
    uDepot<PosixIO> store;
    ASSERT_EQ(store.open(reopen_config()), 0);
    for (int i = 0; i < kKeys; ++i)
        ASSERT_EQ(get_value(store, make_key(i)), make_val(i));
    store.close();
}

// A new table's segment whose metadata write fails is not used: the grow
// fails the write that asked for it with -EIO, as a put that cannot write
// its segment's metadata does. Its metadata used to be ignored, so the
// segment kept its old data metadata and records, and GC took them for
// gone.
TEST_P(StoreIndexTest, ATableSegmentWhoseMetadataWriteFailsIsNotUsed) {
    config_.size = 16 * 1024 * 1024 + 4096;
    config_.segment_size = 16;  // 8 KiB: tables of 512 buckets
    uDepot<PosixIO> store;
    ASSERT_EQ(store.open(config()), 0);
    // Segment metadata written off this thread: the space waker's, for a
    // grow's tables. (This thread's puts write their segments'.)
    static std::thread::id test_thread;
    static uint64_t seg_bytes;
    test_thread = std::this_thread::get_id();
    seg_bytes = config_.segment_size * config_.grain_size;
    uDepot<PosixIO>::internal_io_test_hook = [](bool write, off_t off,
                                                size_t count) {
        const bool seg_md = (static_cast<uint64_t>(off) + count) %
                                    seg_bytes == 0 &&
                            count < seg_bytes;
        return write && seg_md && std::this_thread::get_id() != test_thread
                   ? -EIO
                   : 0;
    };
    int i = 0;
    int rc = 0;
    for (; i < 5000; ++i) {
        rc = store.put(make_key(i), make_val(i)).run_sync();
        if (rc != 0) break;
    }
    uDepot<PosixIO>::internal_io_test_hook = nullptr;
    EXPECT_EQ(rc, -EIO);
    EXPECT_EQ(store.directory().num_tables(), 1u);
    ASSERT_EQ(store.put(make_key(i), make_val(i)).run_sync(), 0);
    EXPECT_GE(store.directory().num_tables(), 2u);
    for (int j = 0; j <= i; ++j)
        ASSERT_EQ(get_value(store, make_key(j)), make_val(j)) << make_key(j);
    store.close();

    uDepot<PosixIO> reopened;
    ASSERT_EQ(reopened.open(reopen_config()), 0);
    EXPECT_TRUE(reopened.index_restored());
    for (int j = 0; j <= i; ++j)
        ASSERT_EQ(get_value(reopened, make_key(j)), make_val(j));
    reopened.close();
}

INSTANTIATE_TEST_SUITE_P(Index, StoreIndexTest,
                         ::testing::Values(Shutdown::kClean));

// Copyright (c) 2024-2026 Nikolas Ioannou
// SPDX-License-Identifier: BSD-3-Clause

#include "udepot/store.h"
#include "udepot/io/posix.h"

#include <cstdint>
#include <cstring>
#include <filesystem>
#include <string>
#include <vector>

#include <gtest/gtest.h>

using udepot::PosixIO;
using udepot::StoreConfig;
using udepot::UDepot;

static constexpr size_t kStoreSize = 4 * 1024 * 1024;

class StoreRecoveryTest : public ::testing::Test {
protected:
    void SetUp() override {
        path_ = std::filesystem::temp_directory_path() /
                "udepot_recovery_test";
        config_.path = path_.c_str();
        config_.size = kStoreSize;
        config_.grain_size = 512;
        config_.initial_tables = 2;
        config_.index_bits = 10;
        config_.force_destroy = true;
    }

    void TearDown() override {
        std::filesystem::remove(path_);
    }

    StoreConfig config() const { return config_; }

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

TEST_F(StoreRecoveryTest, DataSurvivesCleanShutdownAndReopen) {
    constexpr int kKeys = 100;

    {
        UDepot<PosixIO> store;
        ASSERT_EQ(store.open(config()), 0);
        for (int i = 0; i < kKeys; ++i) {
            ASSERT_EQ(store.put(make_key(i), make_val(i)).run_sync(), 0)
                << "put i=" << i;
        }
        store.close();
    }

    {
        StoreConfig cfg = config();
        cfg.force_destroy = false;
        UDepot<PosixIO> store;
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

TEST_F(StoreRecoveryTest, DataSurvivesSimulatedCrash) {
    constexpr int kKeys = 50;

    {
        auto* store = new UDepot<PosixIO>();
        ASSERT_EQ(store->open(config()), 0);
        for (int i = 0; i < kKeys; ++i) {
            ASSERT_EQ(store->put(make_key(i), make_val(i)).run_sync(), 0);
        }
        // Simulate crash: destroy without close().
        delete store;
    }

    {
        StoreConfig cfg = config();
        cfg.force_destroy = false;
        UDepot<PosixIO> store;
        ASSERT_EQ(store.open(cfg), 0);

        for (int i = 0; i < kKeys; ++i) {
            uint8_t val[128];
            size_t val_size = 0;
            int rc = store.get(make_key(i), val, sizeof(val),
                               &val_size).run_sync();
            ASSERT_EQ(rc, 0)
                << "key missing after crash recovery: " << make_key(i);
            EXPECT_EQ(
                std::string_view(reinterpret_cast<char*>(val), val_size),
                make_val(i));
        }
        store.close();
    }
}

TEST_F(StoreRecoveryTest, DeletedKeysStayDeletedAfterRecovery) {
    constexpr int kKeys = 40;

    {
        UDepot<PosixIO> store;
        ASSERT_EQ(store.open(config()), 0);
        for (int i = 0; i < kKeys; ++i)
            ASSERT_EQ(store.put(make_key(i), make_val(i)).run_sync(), 0);

        for (int i = 0; i < kKeys; i += 2)
            ASSERT_EQ(store.del(make_key(i)).run_sync(), 0);

        store.close();
    }

    {
        StoreConfig cfg = config();
        cfg.force_destroy = false;
        UDepot<PosixIO> store;
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

TEST_F(StoreRecoveryTest, UpsertedKeysHaveNewestValueAfterRecovery) {
    constexpr int kKeys = 30;

    {
        UDepot<PosixIO> store;
        ASSERT_EQ(store.open(config()), 0);

        for (int i = 0; i < kKeys; ++i)
            ASSERT_EQ(store.put(make_key(i), make_val(i)).run_sync(), 0);

        for (int i = 0; i < kKeys; ++i) {
            std::string new_val = make_val(i + 1000);
            ASSERT_EQ(store.put(make_key(i), new_val).run_sync(), 0);
        }

        store.close();
    }

    {
        StoreConfig cfg = config();
        cfg.force_destroy = false;
        UDepot<PosixIO> store;
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

TEST_F(StoreRecoveryTest, RecoveryOnEmptyStoreSucceeds) {
    {
        UDepot<PosixIO> store;
        ASSERT_EQ(store.open(config()), 0);
        store.close();
    }

    {
        StoreConfig cfg = config();
        cfg.force_destroy = false;
        UDepot<PosixIO> store;
        ASSERT_EQ(store.open(cfg), 0);

        uint8_t val[128];
        size_t val_size = 0;
        EXPECT_NE(store.get("nonexistent", val, sizeof(val),
                             &val_size).run_sync(), 0);
        store.close();
    }
}

TEST_F(StoreRecoveryTest, ForceDestroyIgnoresExistingData) {
    constexpr int kKeys = 20;

    {
        UDepot<PosixIO> store;
        ASSERT_EQ(store.open(config()), 0);
        for (int i = 0; i < kKeys; ++i)
            ASSERT_EQ(store.put(make_key(i), make_val(i)).run_sync(), 0);
        store.close();
    }

    {
        UDepot<PosixIO> store;
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

TEST_F(StoreRecoveryTest, RecoveryWithManyKeys) {
    constexpr int kKeys = 500;

    {
        UDepot<PosixIO> store;
        ASSERT_EQ(store.open(config()), 0);
        for (int i = 0; i < kKeys; ++i)
            ASSERT_EQ(store.put(make_key(i), make_val(i)).run_sync(), 0);
        store.close();
    }

    {
        StoreConfig cfg = config();
        cfg.force_destroy = false;
        UDepot<PosixIO> store;
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

TEST_F(StoreRecoveryTest, NewWritesAfterRecoveryWork) {
    constexpr int kKeys = 30;
    constexpr int kNewKeys = 20;

    {
        UDepot<PosixIO> store;
        ASSERT_EQ(store.open(config()), 0);
        for (int i = 0; i < kKeys; ++i)
            ASSERT_EQ(store.put(make_key(i), make_val(i)).run_sync(), 0);
        store.close();
    }

    {
        StoreConfig cfg = config();
        cfg.force_destroy = false;
        UDepot<PosixIO> store;
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

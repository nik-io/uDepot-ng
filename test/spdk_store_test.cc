// Copyright (c) 2024-2026 Nikolas Ioannou
// SPDX-License-Identifier: BSD-3-Clause

#include "udepot/store.h"
#include "udepot/io/spdk.h"

#include <cstdint>
#include <cstring>
#include <string>
#include <string_view>
#include <vector>

#include <gtest/gtest.h>

using udepot::SpdkIO;
using udepot::StoreConfig;
using udepot::UDepot;

class SpdkStoreTest : public ::testing::Test {
protected:
    static void SetUpTestSuite() {
        int rc = SpdkIO::global_init();
        ASSERT_EQ(rc, 0) << "SpdkIO::global_init() failed — is an NVMe "
                            "namespace or NVMeoF target available?";
    }

    static void TearDownTestSuite() {
        SpdkIO::global_shutdown();
    }

    void SetUp() override {
        config_.path = "SPDK";
        config_.size = 0;
        config_.grain_size = 4096;
        config_.initial_tables = 2;
        config_.index_bits = 10;

        ASSERT_EQ(store_.open(config_), 0);
    }

    void TearDown() override {
        store_.close();
    }

    StoreConfig config_;
    UDepot<SpdkIO> store_;
};

TEST_F(SpdkStoreTest, PutThenGetReturnsValue) {
    int rc = store_.put("hello", "world").run_sync();
    ASSERT_EQ(rc, 0);

    uint8_t val[64];
    size_t val_size = 0;
    rc = store_.get("hello", val, sizeof(val), &val_size).run_sync();
    ASSERT_EQ(rc, 0);
    EXPECT_EQ(val_size, 5u);
    EXPECT_EQ(std::string_view(reinterpret_cast<char*>(val), val_size),
              "world");
}

TEST_F(SpdkStoreTest, GetNonexistentKeyReturnsNotFound) {
    uint8_t val[64];
    size_t val_size = 0;
    int rc = store_.get("nosuchkey", val, sizeof(val), &val_size).run_sync();
    EXPECT_NE(rc, 0);
}

TEST_F(SpdkStoreTest, PutMultipleKeysGetEach) {
    ASSERT_EQ(store_.put("k1", "val1").run_sync(), 0);
    ASSERT_EQ(store_.put("k2", "val2").run_sync(), 0);
    ASSERT_EQ(store_.put("k3", "val3").run_sync(), 0);

    uint8_t val[64];
    size_t val_size = 0;

    ASSERT_EQ(store_.get("k1", val, sizeof(val), &val_size).run_sync(), 0);
    EXPECT_EQ(std::string_view(reinterpret_cast<char*>(val), val_size),
              "val1");

    ASSERT_EQ(store_.get("k2", val, sizeof(val), &val_size).run_sync(), 0);
    EXPECT_EQ(std::string_view(reinterpret_cast<char*>(val), val_size),
              "val2");

    ASSERT_EQ(store_.get("k3", val, sizeof(val), &val_size).run_sync(), 0);
    EXPECT_EQ(std::string_view(reinterpret_cast<char*>(val), val_size),
              "val3");
}

TEST_F(SpdkStoreTest, LargeValueRoundTrips) {
    std::vector<uint8_t> large_val(4096, 0xAB);
    auto key = std::span<const uint8_t>(
        reinterpret_cast<const uint8_t*>("bigkey"), 6);
    auto val = std::span<const uint8_t>(large_val.data(), large_val.size());

    ASSERT_EQ(store_.put(key, val).run_sync(), 0);

    std::vector<uint8_t> result(4096);
    size_t val_size = 0;
    ASSERT_EQ(store_.get(key, result.data(), result.size(),
                         &val_size).run_sync(), 0);
    EXPECT_EQ(val_size, 4096u);
    EXPECT_EQ(result, large_val);
}

TEST_F(SpdkStoreTest, DeleteRemovesKey) {
    ASSERT_EQ(store_.put("to_delete", "val").run_sync(), 0);
    ASSERT_EQ(store_.del("to_delete").run_sync(), 0);

    uint8_t val[64];
    size_t val_size = 0;
    EXPECT_NE(store_.get("to_delete", val, sizeof(val),
                         &val_size).run_sync(), 0);
}

TEST_F(SpdkStoreTest, DeleteNonexistentKeyReturnsNotFound) {
    int rc = store_.del("nosuchkey").run_sync();
    EXPECT_NE(rc, 0);
}

TEST_F(SpdkStoreTest, ExistsReturnsSizeForPresentKey) {
    ASSERT_EQ(store_.put("present", "12345").run_sync(), 0);

    size_t val_size = 0;
    int rc = store_.exists("present", &val_size).run_sync();
    ASSERT_EQ(rc, 0);
    EXPECT_EQ(val_size, 5u);
}

TEST_F(SpdkStoreTest, ExistsReturnsNotFoundForMissingKey) {
    size_t val_size = 0;
    int rc = store_.exists("missing", &val_size).run_sync();
    EXPECT_NE(rc, 0);
}

TEST_F(SpdkStoreTest, ManyKeysRoundTrip) {
    constexpr int kCount = 200;
    for (int i = 0; i < kCount; ++i) {
        std::string key = "key_" + std::to_string(i);
        std::string val = "val_" + std::to_string(i);
        ASSERT_EQ(store_.put(key, val).run_sync(), 0) << "put i=" << i;
    }

    for (int i = 0; i < kCount; ++i) {
        std::string key = "key_" + std::to_string(i);
        std::string expected = "val_" + std::to_string(i);
        uint8_t val[64];
        size_t val_size = 0;
        ASSERT_EQ(store_.get(key, val, sizeof(val), &val_size).run_sync(), 0)
            << "get i=" << i;
        EXPECT_EQ(std::string_view(reinterpret_cast<char*>(val), val_size),
                  expected)
            << "i=" << i;
    }
}

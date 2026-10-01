// Copyright (c) 2024-2026 Nikolas Ioannou
// SPDX-License-Identifier: BSD-3-Clause

#include "udepot/store.h"
#include "udepot/io/spdk.h"

#include <chrono>
#include <cstdint>
#include <cstring>
#include <string>
#include <string_view>
#include <thread>
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

namespace {

std::string value_for(int key, int round, size_t len) {
    std::string v(len, '\0');
    for (size_t i = 0; i < len; ++i)
        v[i] = static_cast<char>('a' + (key * 31 + round * 7 + i) % 26);
    return v;
}

std::string get_or_empty(UDepot<SpdkIO>& store, const std::string& key,
                         size_t max) {
    std::string out(max, '\0');
    size_t n = 0;
    int rc = store.get(key, reinterpret_cast<uint8_t*>(out.data()), max, &n)
                 .run_sync();
    if (rc != 0) return {};
    out.resize(n);
    return out;
}

}  // namespace

// Several threads, each with its own queue pair, overwrite the device a few
// times over: segments must be reclaimed by GC (whose own I/O runs on its
// thread's queue pair) while writers wait for space, and everything,
// relocated records included, must survive a reopen.
TEST_F(SpdkStoreTest, ConcurrentOverwritesBeyondDeviceSize) {
    constexpr int kThreads = 4;
    constexpr int kKeysPerThread = 50;
    constexpr size_t kVal = 60000;
    const uint64_t dev = store_.io().get_size();
    const int rounds = static_cast<int>(
        3 * dev / (kThreads * kKeysPerThread * kVal)) + 1;

    auto t0 = std::chrono::steady_clock::now();
    std::vector<std::thread> threads;
    std::vector<int> failures(kThreads, 0);
    for (int t = 0; t < kThreads; ++t) {
        threads.emplace_back([&, t] {
            for (int r = 0; r < rounds; ++r)
                for (int k = t * kKeysPerThread; k < (t + 1) * kKeysPerThread;
                     ++k)
                    if (store_.put("ow" + std::to_string(k),
                                   value_for(k, r, kVal)).run_sync() != 0)
                        ++failures[t];
        });
    }
    for (auto& th : threads) th.join();
    auto t1 = std::chrono::steady_clock::now();
    for (int t = 0; t < kThreads; ++t) EXPECT_EQ(failures[t], 0) << t;

    auto check = [&] {
        for (int k = 0; k < kThreads * kKeysPerThread; ++k)
            ASSERT_EQ(get_or_empty(store_, "ow" + std::to_string(k), kVal),
                      value_for(k, rounds - 1, kVal)) << k;
    };
    check();
    auto t2 = std::chrono::steady_clock::now();
    store_.close();
    ASSERT_EQ(store_.open(config_), 0);
    auto t3 = std::chrono::steady_clock::now();
    check();
    using ms = std::chrono::milliseconds;
    printf("puts=%d put=%lldms get=%lldms reopen=%lldms\n",
           rounds * kThreads * kKeysPerThread,
           (long long)std::chrono::duration_cast<ms>(t1 - t0).count(),
           (long long)std::chrono::duration_cast<ms>(t2 - t1).count(),
           (long long)std::chrono::duration_cast<ms>(t3 - t2).count());
}

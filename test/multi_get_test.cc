// Copyright (c) 2024-2026 Nikolas Ioannou
// SPDX-License-Identifier: BSD-3-Clause

// udepot::multi_get (src/udepot/multi_get.h), the batch behind pyudepot's
// uDepotMultiGetBuf. It is a header the store library compiles, so the
// bindings being out of the TSAN build does not leave it unraced here.

#include "udepot/multi_get.h"

#include <algorithm>
#include <climits>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <string>
#include <vector>

#include <sys/sysinfo.h>
#include <unistd.h>

#include <gtest/gtest.h>

#include "udepot/io/aio.h"

using udepot::AioIO;
using udepot::GetBuffer;
using udepot::StoreConfig;
using udepot::uDepot;
using Store = uDepot<AioIO>;

namespace {

constexpr unsigned kWindow = 8;
constexpr int kUnset = INT_MIN;
constexpr size_t kKeySize = 13;
constexpr size_t kValSize = 100;

std::string key_of(int i) {
    char b[32];
    snprintf(b, sizeof(b), "mget_key_%04d", i);
    return b;
}

std::string val_of(int i) {
    std::string v(kValSize, static_cast<char>('a' + i % 26));
    snprintf(v.data(), 16, "v%07d", i);
    v[8] = 'x';  // snprintf's NUL
    return v;
}

// get_read_test_hook has no context; these are the running test's.
const int* g_results = nullptr;
uint32_t g_n = 0;
unsigned g_started = 0;
unsigned g_max_in_flight = 0;

// Called by each get just before it submits its read. The batch writes
// results[i] as it reaps get i, so started minus reaped is the number of
// reads submitted and not yet reaped, this one included.
void count_in_flight(std::span<const uint8_t>) {
    ++g_started;
    unsigned reaped = 0;
    for (uint32_t i = 0; i < g_n; ++i) reaped += g_results[i] != kUnset;
    g_max_in_flight = std::max(g_max_in_flight, g_started - reaped);
}

class MultiGetTest : public ::testing::Test {
protected:
    void SetUp() override {
        path_ = std::filesystem::temp_directory_path() /
                ("udepot_multi_get_test_" + std::to_string(getpid()));
        config_.path = path_.c_str();
        config_.size = 64 * 1024 * 1024;
        config_.grain_size = 512;
        config_.force_destroy = true;
        config_.queue_depth = kWindow;
        ASSERT_EQ(store_.open(config_), 0);
        ASSERT_EQ(store_.io().queue_depth(), kWindow);
    }

    void TearDown() override {
        Store::get_read_test_hook = nullptr;
        store_.close();
        std::filesystem::remove(path_);
    }

    // Keys 0..n-1; every third is never stored.
    static bool missing(int i) { return i % 3 == 2; }

    void put_keys(int n) {
        for (int i = 0; i < n; ++i) {
            if (missing(i)) continue;
            ASSERT_EQ(store_.put(key_of(i), val_of(i)).run_sync(), 0);
        }
    }

    struct Batch {
        std::vector<std::string> keys;
        std::vector<const uint8_t*> key_ptrs;
        std::vector<uint32_t> key_sizes;
        std::vector<GetBuffer> bufs;
        std::vector<GetBuffer*> buf_ptrs;
        std::vector<int> results;
    };

    Batch make_batch(int n) {
        Batch b;
        b.bufs.resize(n);
        b.results.assign(n, kUnset);
        for (int i = 0; i < n; ++i) {
            b.keys.push_back(key_of(i));
            b.bufs[i] = store_.alloc_get_buffer(kKeySize, kValSize);
        }
        for (int i = 0; i < n; ++i) {
            b.key_ptrs.push_back(
                reinterpret_cast<const uint8_t*>(b.keys[i].data()));
            b.key_sizes.push_back(static_cast<uint32_t>(b.keys[i].size()));
            b.buf_ptrs.push_back(&b.bufs[i]);
        }
        return b;
    }

    int run(Batch& b, unsigned window) {
        return udepot::multi_get(store_, static_cast<uint32_t>(b.keys.size()),
                                 b.key_ptrs.data(), b.key_sizes.data(),
                                 b.buf_ptrs.data(), b.results.data(), window);
    }

    std::filesystem::path path_;
    StoreConfig config_;
    Store store_;
};

}  // namespace

TEST_F(MultiGetTest, EveryKeyGetsItsOwnResultAndValue) {
    constexpr int kMax = 200;
    put_keys(kMax);
    for (int n : {0, 1, 2, static_cast<int>(kWindow) - 1,
                  static_cast<int>(kWindow), static_cast<int>(kWindow) + 1,
                  4 * static_cast<int>(kWindow), kMax}) {
        SCOPED_TRACE("n=" + std::to_string(n));
        Batch b = make_batch(n);
        ASSERT_EQ(run(b, kWindow), 0);
        for (int i = 0; i < n; ++i) {
            if (missing(i)) {
                EXPECT_EQ(b.results[i], -ENOENT) << i;
                continue;
            }
            ASSERT_EQ(b.results[i], 0) << i;
            auto v = b.bufs[i].value();
            EXPECT_EQ(std::string(reinterpret_cast<const char*>(v.data()),
                                  v.size()),
                      val_of(i)) << i;
        }
    }
}

// The point of the call: gets overlap on one thread. Nothing here depends
// on timing: each get's hook counts the reads submitted and not yet reaped,
// and the batch can only reap on the thread that submitted, so a window of W
// shows W submitted before the first is reaped.
TEST_F(MultiGetTest, KeepsTheWindowFullOnOneThread) {
    for (int i = 0; i < 64; ++i)  // all present for this test
        ASSERT_EQ(store_.put(key_of(i), val_of(i)).run_sync(), 0);
    for (unsigned window : {1u, 3u, kWindow}) {
        SCOPED_TRACE("window=" + std::to_string(window));
        Batch b = make_batch(64);
        g_results = b.results.data();
        g_n = 64;
        g_started = g_max_in_flight = 0;
        Store::get_read_test_hook = count_in_flight;
        ASSERT_EQ(run(b, window), 0);
        Store::get_read_test_hook = nullptr;
        EXPECT_EQ(g_started, 64u);
        EXPECT_EQ(g_max_in_flight, window);
        for (int r : b.results) EXPECT_EQ(r, 0);
    }
}

// The window is what keeps a batch inside the queue: past it the backend
// refuses (-EAGAIN), and that is reported per key, not hidden. The kernel
// sizes an AIO context for at least 8 I/Os per possible CPU whatever depth
// was asked for (see queue_depth_tests.h), so "past it" is that many.
TEST_F(MultiGetTest, AWindowPastTheQueueFailsWithEagain) {
    const int n = std::max(4096, 16 * get_nprocs_conf() + 1024);
    for (int i = 0; i < n; ++i)
        ASSERT_EQ(store_.put(key_of(i), val_of(i)).run_sync(), 0);
    Batch ok = make_batch(n);
    ASSERT_EQ(run(ok, store_.io().queue_depth()), 0);
    for (int r : ok.results) EXPECT_EQ(r, 0);

    Batch over = make_batch(n);
    ASSERT_EQ(run(over, n), 0);
    int eagain = 0;
    for (int r : over.results) {
        EXPECT_TRUE(r == 0 || r == -EAGAIN) << r;
        eagain += r == -EAGAIN;
    }
    EXPECT_GT(eagain, 0) << "no get found the queue full";
}

TEST_F(MultiGetTest, ZeroCopiesNoValue) {
    put_keys(30);
    Batch b = make_batch(30);
    const uint64_t before = Store::thread_value_copies();
    ASSERT_EQ(run(b, kWindow), 0);
    EXPECT_EQ(Store::thread_value_copies(), before);
}

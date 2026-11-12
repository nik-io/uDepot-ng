// Copyright (c) 2024-2026 Nikolas Ioannou
// SPDX-License-Identifier: BSD-3-Clause

// uDepotMultiGetBuf, the C ABI under pyudepot's multi_get_into, called as
// the binding calls it. pyudepot.cc is compiled into this test (not loaded
// from libpyudepot.so) so that the store's test hooks are the same objects.

#include "pyudepot.h"

#include <algorithm>
#include <climits>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <string>
#include <thread>
#include <vector>

#include <sys/sysinfo.h>
#include <unistd.h>

#include <gtest/gtest.h>

#include "udepot/io/aio.h"
#include "udepot/store.h"

namespace {

using Store = udepot::uDepot<udepot::AioIO>;

constexpr int kUnset = INT_MIN;
constexpr uint32_t kKeySize = 13;
constexpr uint64_t kValSize = 100;
constexpr uint64_t kTableBytes = uint64_t{2} << 20;

std::string key_of(int i) {
    char b[32];
    snprintf(b, sizeof(b), "mget_key_%04d", i);
    return b;
}

struct TempStore {
    std::string path;
    void* kv = nullptr;
    TempStore(const char* tag, uint64_t size, uint32_t queue_depth) {
        path = (std::filesystem::temp_directory_path() /
                (std::string("udepot_abi_") + tag + "_" +
                 std::to_string(getpid()))).string();
        kv = uDepotOpenQueue(path.c_str(), size, 1, kTableBytes, queue_depth);
    }
    ~TempStore() {
        if (kv) uDepotClose(kv);
        std::filesystem::remove(path);
    }
    TempStore(const TempStore&) = delete;
    TempStore& operator=(const TempStore&) = delete;
};

void put_keys(void* kv, int n) {
    std::vector<uint8_t> val(kValSize, 7);
    for (int i = 0; i < n; ++i) {
        const std::string k = key_of(i);
        ASSERT_EQ(uDepotPut(kv, reinterpret_cast<const uint8_t*>(k.data()),
                            kKeySize, val.data(), val.size()), 0) << i;
    }
}

// One batch of keys 0..n-1, with its buffers and results.
struct Batch {
    std::vector<std::string> keys;
    std::vector<const uint8_t*> key_ptrs;
    std::vector<uint32_t> key_sizes;
    std::vector<void*> bufs;
    std::vector<int> results;
    Batch(void* kv, int n) : results(n, kUnset) {
        keys.reserve(n);
        for (int i = 0; i < n; ++i) {
            keys.push_back(key_of(i));
            bufs.push_back(uDepotAllocGetBuffer(kv, kKeySize, kValSize));
        }
        for (int i = 0; i < n; ++i) {
            key_ptrs.push_back(
                reinterpret_cast<const uint8_t*>(keys[i].data()));
            key_sizes.push_back(kKeySize);
        }
    }
    ~Batch() {
        for (void* b : bufs) uDepotFreeGetBuffer(b);
    }
    int run(void* kv) {
        return uDepotMultiGetBuf(kv, static_cast<uint32_t>(keys.size()),
                                 key_ptrs.data(), key_sizes.data(),
                                 bufs.data(), results.data());
    }
};

const int* g_results = nullptr;
uint32_t g_n = 0;
unsigned g_started = 0;
unsigned g_max_in_flight = 0;

// As multi_get_test.cc: reads submitted and not yet reaped.
void count_in_flight(std::span<const uint8_t>) {
    ++g_started;
    unsigned reaped = 0;
    for (uint32_t i = 0; i < g_n; ++i) reaped += g_results[i] != kUnset;
    g_max_in_flight = std::max(g_max_in_flight, g_started - reaped);
}

}  // namespace

// The binding keeps the store's queue depth in flight, not one.
TEST(PyudepotMultiGet, TheAbiKeepsTheQueueDepthInFlight) {
    constexpr uint32_t kQd = 8;
    TempStore s("inflight", 64 << 20, kQd);
    ASSERT_NE(s.kv, nullptr);
    put_keys(s.kv, 64);
    Batch b(s.kv, 64);
    g_results = b.results.data();
    g_n = 64;
    g_started = g_max_in_flight = 0;
    Store::get_read_test_hook = count_in_flight;
    const int rc = b.run(s.kv);
    Store::get_read_test_hook = nullptr;
    ASSERT_EQ(rc, 0);
    EXPECT_EQ(g_started, 64u);
    EXPECT_EQ(g_max_in_flight, kQd);
    for (int r : b.results) EXPECT_EQ(r, 0);
}

// A thread's AIO context belongs to the thread: opening another store
// resizes it (and sets the depth later threads' contexts are made at). The
// window follows the thread, so a store opened with a deep queue and used
// after a shallow one was opened on the same thread gets no -EAGAIN. The
// batch is past the kernel's floor (8 I/Os per possible CPU) so that a
// window of the store's own depth would show.
TEST(PyudepotMultiGet, TheWindowFollowsTheThreadNotTheStore) {
    const int n = std::max(4096, 16 * get_nprocs_conf() + 1024);
    TempStore deep("deep", 128 << 20, 0);  // the default depth, 1024
    ASSERT_NE(deep.kv, nullptr);
    put_keys(deep.kv, n);
    TempStore shallow("shallow", 8 << 20, 8);  // resizes this thread's context
    ASSERT_NE(shallow.kv, nullptr);
    ASSERT_EQ(udepot::AioIO::thread_queue_room(), 8u);

    Batch b(deep.kv, n);
    ASSERT_EQ(b.run(deep.kv), 0);
    int bad = 0;
    for (int r : b.results) bad += r != 0;
    EXPECT_EQ(bad, 0) << "gets failed (-EAGAIN) past the thread's context";

    // A thread that has not done I/O yet gets its context at the depth of the
    // store opened last, and says so before it has one.
    std::thread([&] {
        EXPECT_EQ(udepot::AioIO::thread_queue_room(), 8u);
        Batch c(deep.kv, 64);
        EXPECT_EQ(c.run(deep.kv), 0);
        for (int r : c.results) EXPECT_EQ(r, 0);
        EXPECT_EQ(udepot::AioIO::thread_queue_room(), 8u);
    }).join();
}

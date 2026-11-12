// Copyright (c) 2024-2026 Nikolas Ioannou
// SPDX-License-Identifier: BSD-3-Clause

// uDepotMultiGetBuf, the C ABI under pyudepot's multi_get_into, called as
// the binding calls it. pyudepot.cc is compiled into this test (not loaded
// from libpyudepot.so) so that the store's test hooks are the same objects.

#include "pyudepot.h"

#include <algorithm>
#include <climits>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <string>
#include <thread>
#include <vector>

#include <fcntl.h>
#include <sys/ioctl.h>
#include <linux/fs.h>
#include <sys/stat.h>
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

// uDepotOpenGrain: the grain is the caller's, and an invalid one fails the
// open with a message that names it.
namespace {

uint32_t grain_of(void* kv) { return static_cast<Store*>(kv)->grain_size(); }

void* open_grain(const std::string& path, uint32_t grain, int force) {
    return uDepotOpenGrain(path.c_str(), 16 << 20, force, kTableBytes, 0,
                           grain);
}

struct TempPath {
    std::string path;
    explicit TempPath(const char* tag)
        : path((std::filesystem::temp_directory_path() /
                (std::string("udepot_abi_grain_") + tag + "_" +
                 std::to_string(getpid()))).string()) {}
    ~TempPath() { std::filesystem::remove(path); }
};

}  // namespace

TEST(PyudepotGrain, TheGrainReachesTheStore) {
    TempPath p("reach");
    for (uint32_t g : {512u, 4096u, 32768u}) {
        void* kv = open_grain(p.path, g, 1);
        ASSERT_NE(kv, nullptr) << g;
        EXPECT_EQ(grain_of(kv), g);
        const std::string k = key_of(1);
        const uint8_t val[3] = {1, 2, 3};
        uint8_t out[3] = {};
        ASSERT_EQ(uDepotPut(kv, reinterpret_cast<const uint8_t*>(k.data()),
                            kKeySize, val, sizeof(val)), 0) << g;
        ASSERT_EQ(uDepotGet(kv, reinterpret_cast<const uint8_t*>(k.data()),
                            kKeySize, out, sizeof(out)), 0) << g;
        EXPECT_EQ(std::memcmp(val, out, sizeof(val)), 0) << g;
        uDepotClose(kv);
    }
}

TEST(PyudepotGrain, TheOlderEntryPointsKeepTheDefaultGrain) {
    TempPath p("default");
    for (void* kv : {uDepotOpen(p.path.c_str(), 16 << 20, 1),
                     uDepotOpenTables(p.path.c_str(), 16 << 20, 1, kTableBytes),
                     uDepotOpenQueue(p.path.c_str(), 16 << 20, 1, kTableBytes,
                                     0)}) {
        ASSERT_NE(kv, nullptr);
        EXPECT_EQ(grain_of(kv), 4096u);
        uDepotClose(kv);
    }
}

TEST(PyudepotGrain, AnInvalidGrainFailsTheOpenAndNamesItself) {
    TempPath p("invalid");
    for (uint32_t g : {0u, 1u, 256u, 511u, 513u, 3000u, 65535u,
                       2u << 20, 4u << 20, 0x80000000u}) {
        testing::internal::CaptureStderr();
        void* kv = open_grain(p.path, g, 1);
        const std::string err = testing::internal::GetCapturedStderr();
        EXPECT_EQ(kv, nullptr) << g;
        if (kv) uDepotClose(kv);
        EXPECT_NE(err.find("grain size " + std::to_string(g) + " is not valid"),
                  std::string::npos) << g << ": " << err;
        EXPECT_NE(err.find("a power of two from 512 to 1048576 bytes"),
                  std::string::npos) << g << ": " << err;
        EXPECT_FALSE(std::filesystem::exists(p.path))
            << g << ": a refused grain must not touch the file";
    }
}

// The largest grain is accepted: its open gets as far as the device (16 MiB
// holds too few 1 MiB grains), which has its own message, not the grain's.
TEST(PyudepotGrain, TheLargestGrainPassesTheCheck) {
    TempPath p("largest");
    testing::internal::CaptureStderr();
    void* kv = open_grain(p.path, 1u << 20, 1);
    const std::string err = testing::internal::GetCapturedStderr();
    if (kv) uDepotClose(kv);
    EXPECT_EQ(err.find("is not valid"), std::string::npos) << err;
}

// What the store does on a grain other than the one it was created with:
// the open fails (-EEXIST, "no store found": the device metadata sits at a
// grain-dependent offset, so the store is not even found), nothing is
// formatted over it, and it opens again with its own grain. force_destroy
// makes a new store of the new grain.
TEST(PyudepotGrain, ReopeningWithAnotherGrainFailsAndHarmsNothing) {
    TempPath p("reopen");
    const std::string k = key_of(1);
    const auto* key = reinterpret_cast<const uint8_t*>(k.data());
    const uint8_t val[3] = {4, 5, 6};
    for (uint32_t created : {512u, 4096u, 32768u}) {
        std::filesystem::remove(p.path);  // a file that held no other store
        void* kv = open_grain(p.path, created, 1);
        ASSERT_NE(kv, nullptr) << created;
        ASSERT_EQ(uDepotPut(kv, key, kKeySize, val, sizeof(val)), 0);
        uDepotClose(kv);
        for (uint32_t other : {512u, 4096u, 32768u, 8192u}) {
            if (other == created) continue;
            testing::internal::CaptureStderr();
            void* bad = open_grain(p.path, other, 0);
            const std::string err = testing::internal::GetCapturedStderr();
            if (bad) uDepotClose(bad);
            EXPECT_EQ(bad, nullptr) << created << " reopened as " << other;
            EXPECT_NE(err.find("no store of"), std::string::npos)
                << created << " as " << other << ": " << err;
        }
        kv = open_grain(p.path, created, 0);
        ASSERT_NE(kv, nullptr) << created << ": the failed opens harmed it";
        uint8_t out[3] = {};
        ASSERT_EQ(uDepotGet(kv, key, kKeySize, out, sizeof(out)), 0) << created;
        EXPECT_EQ(std::memcmp(val, out, sizeof(val)), 0) << created;
        uDepotClose(kv);
    }
    // force_destroy: a new store of the new grain (over a store of another
    // grain, a reopen under the old one is not safe afterwards: see
    // "Grain size" in docs/architecture.md; nothing here relies on it).
    void* kv = open_grain(p.path, 512, 1);
    ASSERT_NE(kv, nullptr);
    EXPECT_EQ(grain_of(kv), 512u);
    uint8_t out[3] = {};
    EXPECT_NE(uDepotGet(kv, key, kKeySize, out, sizeof(out)), 0)
        << "a new store holds nothing";
    uDepotClose(kv);
}

// The O_DIRECT alignment of the backing: a grain below it is refused with a
// message naming both numbers. Only the decision is tested here; a real 4Kn
// device needs root and a loop device.
namespace pyudepot_internal {
uint32_t direct_io_alignment(const char* path, const char** what);
bool grain_fits_alignment(uint32_t grain, uint32_t align, const char* path,
                          const char* what);
}  // namespace pyudepot_internal

TEST(PyudepotGrain, AGrainBelowTheBackingsAlignmentIsRefusedNamingBoth) {
    testing::internal::CaptureStderr();
    const bool ok = pyudepot_internal::grain_fits_alignment(
        512, 4096, "/dev/x", "the block device's logical sector size");
    const std::string err = testing::internal::GetCapturedStderr();
    EXPECT_FALSE(ok);
    EXPECT_NE(err.find("grain size 512 is smaller than the 4096 bytes"),
              std::string::npos) << err;
    EXPECT_NE(err.find("/dev/x"), std::string::npos) << err;
    EXPECT_NE(err.find("logical sector size"), std::string::npos) << err;
    for (auto [grain, align] : {std::pair{512u, 512u}, {4096u, 4096u},
                                {8192u, 4096u}, {512u, 0u}, {4096u, 0u}}) {
        testing::internal::CaptureStderr();
        EXPECT_TRUE(pyudepot_internal::grain_fits_alignment(grain, align,
                                                            "p", "w"))
            << grain << " in " << align;
        EXPECT_EQ(testing::internal::GetCapturedStderr(), "");
    }
}

TEST(PyudepotGrain, TheAlignmentOfAFileIsWhatStatxSays) {
    TempPath p("align");
    const char* what = "";
    EXPECT_EQ(pyudepot_internal::direct_io_alignment(p.path.c_str(), &what),
              0u) << "a file that does not exist has none";
    void* kv = open_grain(p.path, 4096, 1);
    ASSERT_NE(kv, nullptr);
    uDepotClose(kv);
    struct statx stx;
    ASSERT_EQ(::statx(AT_FDCWD, p.path.c_str(), 0, STATX_DIOALIGN, &stx), 0);
    const uint32_t expect = (stx.stx_mask & STATX_DIOALIGN)
                                ? stx.stx_dio_offset_align : 0;
    EXPECT_EQ(pyudepot_internal::direct_io_alignment(p.path.c_str(), &what),
              expect);
    if (expect) {
        EXPECT_NE(std::string(what).find("statx"), std::string::npos);
    }
    // A grain the file takes opens (the aligned ones: a 512 grain is the
    // smallest, so it is refused only where the file says more than 512).
    void* again = open_grain(p.path, std::max<uint32_t>(4096, expect), 0);
    EXPECT_NE(again, nullptr);
    if (again) uDepotClose(again);
}

// A 4Kn block device: a loop device with a 4096-byte logical sector size. A
// grain of 512 is below what O_DIRECT takes there, and the binding says so
// before the open (without the check the open fails with only "Input/output
// error"). Needs root and losetup, so it skips without them; CI runs it
// under sudo and fails the job if it skipped, as for block_device_test
// (.github/workflows/ci.yml).
TEST(PyudepotGrainLoop, AGrainBelowTheSectorSizeIsRefusedOn4KnAndOneAtItOpens) {
    constexpr uint64_t kBytes = (uint64_t{64} << 20) + 4096;
    if (::geteuid() != 0) GTEST_SKIP() << "not root: losetup needs it";
    const std::string img = (std::filesystem::temp_directory_path() /
                             ("udepot_abi_4kn_" + std::to_string(getpid()) +
                              ".img")).string();
    {
        const int fd = ::open(img.c_str(), O_RDWR | O_CREAT | O_TRUNC, 0600);
        ASSERT_GE(fd, 0);
        ASSERT_EQ(::ftruncate(fd, static_cast<off_t>(kBytes)), 0);
        ::close(fd);
    }
    FILE* pipe = ::popen(
        ("losetup --sector-size 4096 --find --show " + img + " 2>&1").c_str(),
        "r");
    ASSERT_NE(pipe, nullptr);
    char line[256] = {};
    const char* got = std::fgets(line, sizeof(line), pipe);
    const int lrc = ::pclose(pipe);
    std::string dev = got ? line : "";
    while (!dev.empty() && (dev.back() == '\n' || dev.back() == '\r'))
        dev.pop_back();
    if (lrc != 0 || dev.rfind("/dev/", 0) != 0) {
        std::filesystem::remove(img);
        GTEST_SKIP() << "losetup --sector-size failed: " << dev;
    }
    struct Detach {
        std::string dev, img;
        ~Detach() {
            if (std::system(("losetup -d " + dev + " >/dev/null 2>&1")
                                .c_str()) != 0)
                std::fprintf(stderr, "could not detach %s\n", dev.c_str());
            std::filesystem::remove(img);
        }
    } detach{dev, img};

    int sector = 0;
    {
        const int fd = ::open(dev.c_str(), O_RDONLY);
        ASSERT_GE(fd, 0);
        ASSERT_EQ(::ioctl(fd, BLKSSZGET, &sector), 0);
        ::close(fd);
    }
    ASSERT_EQ(sector, 4096) << dev << " is not 4Kn";

    testing::internal::CaptureStderr();
    void* bad = uDepotOpenGrain(dev.c_str(), kBytes, 1, uint64_t{1} << 20, 0,
                                512);
    const std::string err = testing::internal::GetCapturedStderr();
    if (bad) uDepotClose(bad);
    EXPECT_EQ(bad, nullptr);
    EXPECT_NE(err.find("grain size 512 is smaller than the 4096 bytes"),
              std::string::npos) << err;
    EXPECT_NE(err.find(dev), std::string::npos) << err;

    void* kv = uDepotOpenGrain(dev.c_str(), kBytes, 1, uint64_t{1} << 20, 0,
                               4096);
    ASSERT_NE(kv, nullptr);
    const std::string k = key_of(7);
    const auto* key = reinterpret_cast<const uint8_t*>(k.data());
    const uint8_t val[3] = {7, 8, 9};
    uint8_t out[3] = {};
    ASSERT_EQ(uDepotPut(kv, key, kKeySize, val, sizeof(val)), 0);
    ASSERT_EQ(uDepotGet(kv, key, kKeySize, out, sizeof(out)), 0);
    EXPECT_EQ(std::memcmp(val, out, sizeof(val)), 0);
    uDepotClose(kv);
}

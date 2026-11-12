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

#include "sto-ctlr/sto-capacity-mgr-parameters.h"
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

// uDepotOpenSegment: the segment is the caller's, in bytes. What reaches the
// store is read back from the store (segment, longest record, table width),
// not echoed from the argument.
namespace pyudepot_internal {
uint64_t min_segment_grains(uint32_t grain);
uint64_t segment_unit_bytes(uint32_t grain);
}  // namespace pyudepot_internal

namespace {

void* open_segment(const std::string& path, uint64_t size, uint32_t grain,
                   uint64_t segment_bytes, int force = 1,
                   uint64_t table_bytes = 0) {
    return uDepotOpenSegment(path.c_str(), size, force, table_bytes, 0, grain,
                             segment_bytes);
}

// The longest value under a key of key_size: the segment less its one
// metadata grain (seg_md_grains_, store.cc: align_up(sizeof(salsa_seg_md),
// grain) / grain, one for these grains), capped at 2047 grains
// (HashEntry::kKvSizeMask), less the record's 14-byte header, key and 2-byte
// CRC.
uint64_t expected_max_value(uint64_t segment_bytes, uint32_t grain,
                            uint32_t key_size) {
    const uint64_t grains =
        std::min<uint64_t>(2047, segment_bytes / grain - 1);
    return grains * grain - 16 - key_size;
}

// A device of `n` segments and a tail of two grains: the device metadata
// sits past the last whole segment, and a device that is an exact multiple of
// the segment has none, so the store halves the segment (and says so).
uint64_t device_of(uint64_t n, uint64_t segment_bytes, uint32_t grain) {
    return n * segment_bytes + 2 * grain;
}

uint32_t table_bits_of(void* kv) {
    return static_cast<Store*>(kv)->directory().snapshot().tables[0]
        ->index_bits();
}

std::string captured_open(const std::string& path, uint64_t size,
                          uint32_t grain, uint64_t segment_bytes, void** kv,
                          int force = 1, uint64_t table_bytes = 0) {
    testing::internal::CaptureStderr();
    *kv = open_segment(path, size, grain, segment_bytes, force, table_bytes);
    return testing::internal::GetCapturedStderr();
}

}  // namespace

TEST(PyudepotSegment, TheSegmentReachesTheStoreAndSetsTheLargestValue) {
    TempPath p("seg_reach");
    // 64 segments: fewer, and salsa's spare segments and GC watermarks halve
    // the segment (measured: a 32 MiB device asked for 1 MiB segments got 936 KiB).
    struct Case { uint32_t grain; uint64_t segment; uint64_t size; };
    for (Case c : {Case{4096, 256 * 4096, device_of(64, 256 * 4096, 4096)},
                   Case{512, 8 * 512 * 100, device_of(64, 4096 * 100, 512)},
                   Case{32768, 64 * 32768, device_of(64, 64 * 32768, 32768)},
                   // Not a power of two of grains: the table is the power of
                   // two below (2^19 buckets, 4 MiB: 1099 data grains of
                   // 4096 are 4501504 bytes, of which 8 B * (2^19 + 32) +
                   // 1 KiB are slots and headers).
                   Case{4096, 1100 * 4096, device_of(68, 1100 * 4096, 4096)}}) {
        SCOPED_TRACE(std::to_string(c.grain) + " x " +
                     std::to_string(c.segment));
        void* kv = open_segment(p.path, c.size, c.grain, c.segment);
        ASSERT_NE(kv, nullptr);
        ASSERT_EQ(uDepotSegmentBytes(kv), c.segment)
            << "the store shrank it: the device is too small for the test";
        for (uint32_t ks : {1u, 13u, 100u}) {
            const uint64_t n = expected_max_value(c.segment, c.grain, ks);
            EXPECT_EQ(uDepotMaxValueBytes(kv, ks), n) << ks;
        }
        // A value of exactly that fits, one more byte does not.
        const uint64_t n = expected_max_value(c.segment, c.grain, kKeySize);
        std::vector<uint8_t> val(n + 1), out(n);
        for (size_t i = 0; i < val.size(); ++i) val[i] = uint8_t(i * 7 + 1);
        const std::string k = key_of(1), k2 = key_of(2);
        const auto* key = reinterpret_cast<const uint8_t*>(k.data());
        const auto* key2 = reinterpret_cast<const uint8_t*>(k2.data());
        ASSERT_EQ(uDepotPut(kv, key, kKeySize, val.data(), n), 0);
        ASSERT_EQ(uDepotGet(kv, key, kKeySize, out.data(), n), 0);
        EXPECT_EQ(std::memcmp(val.data(), out.data(), n), 0);
        EXPECT_NE(uDepotPut(kv, key2, kKeySize, val.data(), n + 1), 0);
        uDepotClose(kv);
    }
}

// The segment sets the table: the largest power of two that fits it.
TEST(PyudepotSegment, ThePowerOfTwoTableBelowANonPowerOfTwoSegment) {
    TempPath p("seg_table");
    // 1100 grains: net 4501504 bytes, slots (4501504 - 1024) / 8 = 562560;
    // 2^19 = 524288 buckets + 32 fit, 2^20 do not.
    void* kv = open_segment(p.path, device_of(68, 1100 * 4096, 4096), 4096,
                            1100 * 4096);
    ASSERT_NE(kv, nullptr);
    EXPECT_EQ(table_bits_of(kv), 19u);
    uDepotClose(kv);
    // A segment of exactly a fitted table (2^19 buckets) has the same table.
    const uint64_t fitted = udepot::fitted_segment_size(uint64_t{8} << 19,
                                                        4096);
    kv = open_segment(p.path, device_of(68, fitted * 4096, 4096), 4096,
                      fitted * 4096);
    ASSERT_NE(kv, nullptr);
    EXPECT_EQ(table_bits_of(kv), 19u);
    uDepotClose(kv);
    // The same segment through the older entry point's table size.
    kv = uDepotOpenGrain(p.path.c_str(), device_of(68, fitted * 4096, 4096), 1,
                         uint64_t{8} << 19, 0, 4096);
    ASSERT_NE(kv, nullptr);
    EXPECT_EQ(uDepotSegmentBytes(kv), fitted * 4096);
    uDepotClose(kv);
}

TEST(PyudepotSegment, ZeroIsTheTableDerivedSegmentAndTheOlderEntryPointsAreUnchanged) {
    TempPath p("seg_zero");
    const uint64_t dev = device_of(64, 2105344, 4096);
    void* kv = uDepotOpenGrain(p.path.c_str(), dev, 1, kTableBytes, 0, 4096);
    ASSERT_NE(kv, nullptr);
    const uint64_t via_grain = uDepotSegmentBytes(kv);
    uDepotClose(kv);
    kv = open_segment(p.path, dev, 4096, 0, 1, kTableBytes);
    ASSERT_NE(kv, nullptr);
    EXPECT_EQ(uDepotSegmentBytes(kv), via_grain);
    EXPECT_EQ(via_grain, udepot::fitted_segment_size(kTableBytes, 4096) * 4096);
    uDepotClose(kv);
}

TEST(PyudepotSegment, AnInvalidSegmentFailsTheOpenAndSaysWhy) {
    TempPath p("seg_invalid");
    struct Case { uint32_t grain; uint64_t segment; uint64_t table;
                  const char* msg; };
    for (Case c : {
             // not a multiple of the grain
             Case{32768, 40000, 0, "is not a multiple of 32768 bytes"},
             Case{4096, 4096 * 300 + 512, 0, "is not a multiple of 4096 bytes"},
             // whole grains, not whole pages (the store would round down)
             Case{512, 512 * 33, 0, "is not a multiple of 4096 bytes"},
             // too small
             Case{4096, 4096, 0, "is too small for grains of 4096 bytes"},
             Case{4096, 2 * 4096, 0, "at least 12288 bytes"},
             Case{32768, 2 * 32768, 0, "at least 98304 bytes"},
             // both a segment and a table
             Case{4096, 256 * 4096, kTableBytes,
                  "both a segment size (1048576) and a table size"},
         }) {
        SCOPED_TRACE(std::string(c.msg));
        void* kv = nullptr;
        const std::string err = captured_open(p.path, 64 << 20, c.grain,
                                              c.segment, &kv, 1, c.table);
        EXPECT_EQ(kv, nullptr);
        if (kv) uDepotClose(kv);
        EXPECT_NE(err.find(c.msg), std::string::npos) << err;
        EXPECT_FALSE(std::filesystem::exists(p.path))
            << "a refused segment must not touch the file";
    }
}

// The store opened bare (no binding checks) with a segment of seg_grains on a
// device of dev bytes: its return code.
static int bare_open(const std::string& path, uint64_t dev, uint32_t grain,
                     uint64_t seg_grains) {
    udepot::StoreConfig config;
    config.path = path.c_str();
    config.size = dev;
    config.grain_size = grain;
    config.force_destroy = true;
    config.segment_size = seg_grains;
    Store store;
    testing::internal::CaptureStderr();
    const int rc = store.open(config);
    testing::internal::GetCapturedStderr();
    if (rc == 0) store.close();
    return rc;
}

// The least segment is the store's: one unit under it, the store refuses (so
// the binding's check is not stricter than needed), at it the store opens
// (nor looser). (The refusal below comes from the store, but not only from
// the table check: with the table-room checks of create_directory and
// restore_index removed, the store still refuses 2 grains of 4096, in salsa
// after the device is set up. The two floors coincide at 3 grains, so this
// test pins the binding's minimum to the store's, whichever of them binds.)
// Each store gets a device built for ITS segment, with a tail
// for the device metadata (device_of): on an exact multiple of the segment
// the store abandons for want of a tail before it looks at the table, and the
// refusal would say nothing about the minimum.
TEST(PyudepotSegment, TheMinimumIsTheStoresMinimum) {
    TempPath p("seg_min");
    for (uint32_t grain : {4096u, 32768u}) {
        SCOPED_TRACE(grain);
        const uint64_t unit = pyudepot_internal::segment_unit_bytes(grain);
        const uint64_t min_bytes =
            (pyudepot_internal::min_segment_grains(grain) * grain + unit - 1) /
            unit * unit;
        ASSERT_GT(min_bytes, unit);
        void* kv = open_segment(p.path, device_of(2000, min_bytes, grain), grain,
                                min_bytes);
        ASSERT_NE(kv, nullptr) << "the minimum, " << min_bytes;
        EXPECT_EQ(uDepotSegmentBytes(kv), min_bytes);
        const std::string k = key_of(1);
        const uint8_t val[3] = {1, 2, 3};
        uint8_t out[3] = {};
        const auto* key = reinterpret_cast<const uint8_t*>(k.data());
        EXPECT_EQ(uDepotPut(kv, key, kKeySize, val, 3), 0);
        EXPECT_EQ(uDepotGet(kv, key, kKeySize, out, 3), 0);
        EXPECT_EQ(std::memcmp(val, out, 3), 0);
        uDepotClose(kv);
        // The bare store takes the minimum on a device built for it...
        EXPECT_EQ(bare_open(p.path, device_of(2000, min_bytes, grain), grain,
                            min_bytes / grain), 0);
        // ...and refuses one unit under it, on a device built for that.
        const uint64_t below = min_bytes - unit;
        EXPECT_NE(bare_open(p.path, device_of(2000, below, grain), grain,
                            below / grain), 0)
            << "the store takes a segment under the minimum";
        // The binding refuses it too, before the store.
        void* none = nullptr;
        const std::string err = captured_open(
            p.path, device_of(2000, below, grain), grain, below, &none);
        EXPECT_EQ(none, nullptr);
        EXPECT_NE(err.find("is too small"), std::string::npos) << err;
    }
}

// At 512-byte grains the unit (a 4096-byte page, 8 grains) is above the least
// the table needs (4 grains: min_segment_grains), so no multiple of the unit is
// under the minimum and the table check cannot be reached from below. What
// bounds it there is `sanitize`: the store rounds a segment of 7 grains down to
// 1, which it refuses, and the binding refuses 7 grains as not a multiple of
// 4096 rather than let it be rounded.
TEST(PyudepotSegment, At512ByteGrainsThePageIsTheMinimum) {
    TempPath p("seg_min512");
    EXPECT_EQ(pyudepot_internal::min_segment_grains(512), 4u);
    EXPECT_EQ(bare_open(p.path, device_of(2000, 4096, 512), 512, 8), 0);
    EXPECT_NE(bare_open(p.path, device_of(2000, 3584, 512), 512, 7), 0);
    void* kv = nullptr;
    const std::string err = captured_open(
        p.path, device_of(2000, 3584, 512), 512, 3584, &kv);
    EXPECT_EQ(kv, nullptr);
    EXPECT_NE(err.find("is not a multiple of 4096 bytes"), std::string::npos)
        << err;
    kv = open_segment(p.path, device_of(2000, 4096, 512), 512, 4096);
    ASSERT_NE(kv, nullptr);
    EXPECT_EQ(uDepotSegmentBytes(kv), 4096u);
    uDepotClose(kv);
}

// Salsa takes at most SALSA_MAX_SEGMENT_SIZE grains (INT_MAX - 1); the binding
// refuses more naming the limit, and one segment of exactly that many passes
// the check (the open then goes on to the device, which halves it, said).
TEST(PyudepotSegment, ASegmentAboveWhatSalsaTakesIsRefusedAndTheLimitPasses) {
    TempPath p("seg_max");
    const uint64_t max_grains = SALSA_MAX_SEGMENT_SIZE;
    ASSERT_EQ(max_grains, 2147483646u);
    struct Case { uint32_t grain; uint64_t at_limit; };
    // At 512 the segment is a multiple of the 4096-byte page: the largest one
    // is the limit rounded down.
    for (Case c : {Case{4096, max_grains * 4096},
                   Case{512, max_grains * 512 / 4096 * 4096},
                   Case{32768, max_grains * 32768}}) {
        SCOPED_TRACE(c.grain);
        const uint64_t unit = pyudepot_internal::segment_unit_bytes(c.grain);
        void* kv = nullptr;
        std::string err = captured_open(p.path, 16 << 20, c.grain, c.at_limit,
                                        &kv);
        EXPECT_EQ(err.find("is too large"), std::string::npos) << err;
        if (kv) {
            EXPECT_LT(uDepotSegmentBytes(kv), c.at_limit);
            uDepotClose(kv);
        }
        std::filesystem::remove(p.path);
        err = captured_open(p.path, 16 << 20, c.grain, c.at_limit + unit,
                            &kv);
        EXPECT_EQ(kv, nullptr);
        EXPECT_NE(err.find("segment size " +
                           std::to_string(c.at_limit + unit) +
                           " is too large for grains of " +
                           std::to_string(c.grain) + " bytes: salsa takes at "
                           "most 2147483646 grains (SALSA_MAX_SEGMENT_SIZE)"),
                  std::string::npos) << err;
        EXPECT_FALSE(std::filesystem::exists(p.path))
            << "a refused segment must not touch the file";
    }
}

// A store keeps the segment it was created with. On a reopen segment_bytes
// is checked, then ignored, as table_bytes is.
TEST(PyudepotSegment, ReopeningKeepsTheCreatedSegmentWhateverIsAsked) {
    TempPath p("seg_reopen");
    const uint64_t created = 256 * 4096;
    const std::string k = key_of(1);
    const auto* key = reinterpret_cast<const uint8_t*>(k.data());
    const uint8_t val[3] = {4, 5, 6};
    const uint64_t dev = device_of(64, 512 * 4096, 4096);
    void* kv = open_segment(p.path, dev, 4096, created);
    ASSERT_NE(kv, nullptr);
    ASSERT_EQ(uDepotPut(kv, key, kKeySize, val, sizeof(val)), 0);
    uDepotClose(kv);
    for (uint64_t asked : {created, uint64_t{512 * 4096}, uint64_t{64 * 4096},
                           uint64_t{0}}) {
        SCOPED_TRACE(asked);
        kv = open_segment(p.path, dev, 4096, asked, 0,
                          asked == 0 ? kTableBytes : 0);
        ASSERT_NE(kv, nullptr);
        EXPECT_EQ(uDepotSegmentBytes(kv), created);
        uint8_t out[3] = {};
        ASSERT_EQ(uDepotGet(kv, key, kKeySize, out, sizeof(out)), 0);
        EXPECT_EQ(std::memcmp(val, out, sizeof(val)), 0);
        uDepotClose(kv);
    }
    // The argument is still checked on a reopen: an invalid one fails it.
    void* bad = nullptr;
    const std::string err =
        captured_open(p.path, dev, 4096, 4096 * 300 + 512, &bad, 0);
    EXPECT_EQ(bad, nullptr);
    if (bad) uDepotClose(bad);
    EXPECT_NE(err.find("is not a multiple of 4096 bytes"), std::string::npos)
        << err;
    kv = open_segment(p.path, dev, 4096, created, 0);
    ASSERT_NE(kv, nullptr) << "the failed open harmed the store";
    uDepotClose(kv);
}

// A device too small for the segment: the store halves it and says so.
TEST(PyudepotSegment, ASegmentTheDeviceCannotHoldIsHalvedAndSaid) {
    TempPath p("seg_big");
    void* kv = nullptr;
    const uint64_t asked = 1100 * 4096;
    const std::string err =
        captured_open(p.path, 16 << 20, 4096, asked, &kv);
    ASSERT_NE(kv, nullptr) << err;
    EXPECT_LT(uDepotSegmentBytes(kv), asked);
    EXPECT_NE(err.find("the requested segments of 4400 KiB"),
              std::string::npos) << err;
    EXPECT_NE(err.find("Using segments of " +
                       std::to_string(uDepotSegmentBytes(kv) >> 10) + " KiB"),
              std::string::npos) << err;
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

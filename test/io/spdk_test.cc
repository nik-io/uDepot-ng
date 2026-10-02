// Copyright (c) 2024-2026 Nikolas Ioannou
// SPDX-License-Identifier: BSD-3-Clause

#include "udepot/io/spdk.h"

#include <atomic>
#include <cstdint>
#include <cstring>
#include <string>
#include <thread>
#include <vector>

#include <gtest/gtest.h>

using udepot::SpdkIO;

class SpdkIOTest : public ::testing::Test {
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
        // 'SPDK' is the conventional path placeholder when the namespace is
        // selected by UDEPOT_NVMEF or by the first discovered namespace.
        ASSERT_EQ(io_.open("SPDK", 0), 0);
    }

    void TearDown() override {
        io_.close();
    }

    SpdkIO io_;
};

TEST_F(SpdkIOTest, WriteThenReadBack) {
    const size_t sector = 512;
    auto buf_w = io_.alloc_buffer(sector);
    ASSERT_NE(buf_w.data, nullptr);
    std::memset(buf_w.data, 0xAB, sector);

    ssize_t wrc = io_.pwrite(buf_w.data, sector, 0).run_sync();
    ASSERT_EQ(wrc, static_cast<ssize_t>(sector));

    auto buf_r = io_.alloc_buffer(sector);
    ASSERT_NE(buf_r.data, nullptr);
    std::memset(buf_r.data, 0, sector);

    ssize_t rrc = io_.pread(buf_r.data, sector, 0).run_sync();
    ASSERT_EQ(rrc, static_cast<ssize_t>(sector));
    EXPECT_EQ(std::memcmp(buf_w.data, buf_r.data, sector), 0);
}

TEST_F(SpdkIOTest, MultiSectorWriteAndRead) {
    const size_t sector = 512;
    const size_t count = 8 * sector;

    auto buf_w = io_.alloc_buffer(count);
    ASSERT_NE(buf_w.data, nullptr);
    for (size_t i = 0; i < count; ++i)
        static_cast<uint8_t*>(buf_w.data)[i] = static_cast<uint8_t>(i & 0xFF);

    ssize_t wrc = io_.pwrite(buf_w.data, count, 0).run_sync();
    ASSERT_EQ(wrc, static_cast<ssize_t>(count));

    auto buf_r = io_.alloc_buffer(count);
    ASSERT_NE(buf_r.data, nullptr);
    std::memset(buf_r.data, 0, count);

    ssize_t rrc = io_.pread(buf_r.data, count, 0).run_sync();
    ASSERT_EQ(rrc, static_cast<ssize_t>(count));
    EXPECT_EQ(std::memcmp(buf_w.data, buf_r.data, count), 0);
}

TEST_F(SpdkIOTest, WriteAtOffset) {
    const size_t sector = 512;
    const off_t offset = 4096;

    auto buf_w = io_.alloc_buffer(sector);
    ASSERT_NE(buf_w.data, nullptr);
    std::memset(buf_w.data, 0xCD, sector);

    ssize_t wrc = io_.pwrite(buf_w.data, sector, offset).run_sync();
    ASSERT_EQ(wrc, static_cast<ssize_t>(sector));

    auto buf_r = io_.alloc_buffer(sector);
    ASSERT_NE(buf_r.data, nullptr);
    std::memset(buf_r.data, 0, sector);

    ssize_t rrc = io_.pread(buf_r.data, sector, offset).run_sync();
    ASSERT_EQ(rrc, static_cast<ssize_t>(sector));
    EXPECT_EQ(std::memcmp(buf_w.data, buf_r.data, sector), 0);
}

TEST_F(SpdkIOTest, GetSizeReturnsNonZero) {
    EXPECT_GT(io_.get_size(), 0u);
}

TEST_F(SpdkIOTest, AllocBufferReturnsDmaBuffer) {
    auto buf = io_.alloc_buffer(4096);
    ASSERT_NE(buf.data, nullptr);
    EXPECT_EQ(buf.alloc, udepot::BufferAlloc::kDma);
}

// Regression: the completion poller ran on a dedicated thread and called
// get_thread_qpair(), which returned that thread's TLS qpair — not the
// submitting thread's.  Any I/O submitted from a non-poller thread would
// never have its completions processed, hanging forever.  The fix makes
// SpdkSubmitAwaitable poll completions inline on the submitting thread's
// own qpair (the poller thread now only drives admin completions).
//
// This test exercises the fix: multiple threads do simultaneous I/O.
// With the old bug, every non-poller thread's pwrite/pread would hang.
TEST_F(SpdkIOTest, MultiThreadedIO) {
    constexpr int kThreads = 4;
    constexpr size_t sector = 512;
    std::atomic<int> failures{0};

    std::vector<std::thread> threads;
    for (int t = 0; t < kThreads; ++t) {
        threads.emplace_back([&, t] {
            const off_t offset = static_cast<off_t>(t) * 4096;
            const uint8_t pattern = static_cast<uint8_t>(0xA0 + t);

            auto buf_w = io_.alloc_buffer(sector);
            if (!buf_w.data) { failures.fetch_add(1); return; }
            std::memset(buf_w.data, pattern, sector);

            ssize_t wrc = io_.pwrite(buf_w.data, sector, offset).run_sync();
            if (wrc != static_cast<ssize_t>(sector)) {
                failures.fetch_add(1); return;
            }

            auto buf_r = io_.alloc_buffer(sector);
            if (!buf_r.data) { failures.fetch_add(1); return; }
            std::memset(buf_r.data, 0, sector);

            ssize_t rrc = io_.pread(buf_r.data, sector, offset).run_sync();
            if (rrc != static_cast<ssize_t>(sector)) {
                failures.fetch_add(1); return;
            }

            if (std::memcmp(buf_w.data, buf_r.data, sector) != 0) {
                failures.fetch_add(1);
            }
        });
    }

    for (auto& th : threads) th.join();
    EXPECT_EQ(failures.load(), 0)
        << "multi-threaded I/O failed — completions likely not polled "
           "on the submitting thread's qpair";
}

// Regression (PR #3 review, finding 7): every I/O bounced through a fresh
// DMA buffer, so the zero-copy API still copied on SPDK. DMA memory that
// covers whole sectors now goes to the device as is.
TEST_F(SpdkIOTest, DmaBufferIoDoesNotBounce) {
    const size_t len = 8192;
    auto w = io_.alloc_buffer(len);
    auto r = io_.alloc_buffer(len);
    ASSERT_NE(w.data, nullptr);
    ASSERT_NE(r.data, nullptr);
    for (size_t i = 0; i < len; ++i)
        static_cast<uint8_t*>(w.data)[i] = static_cast<uint8_t>(i * 7);
    std::memset(r.data, 0, len);

    // Opens this thread's queue pair, so the count below starts from it.
    ASSERT_EQ(io_.pwrite(w.data, len, 8192).run_sync(),
              static_cast<ssize_t>(len));
    const uint64_t before = SpdkIO::thread_bounce_count();
    ASSERT_EQ(io_.pwrite(w.data, len, 16384).run_sync(),
              static_cast<ssize_t>(len));
    ASSERT_EQ(io_.pread(r.data, len, 16384).run_sync(),
              static_cast<ssize_t>(len));
    ASSERT_EQ(io_.pwrite_sync(w.data, len, 24576), static_cast<ssize_t>(len));
    EXPECT_EQ(SpdkIO::thread_bounce_count(), before);
    EXPECT_EQ(std::memcmp(w.data, r.data, len), 0);
}

// Memory SPDK cannot translate (an ordinary heap buffer) bounces, and the
// data still round-trips.
TEST_F(SpdkIOTest, HeapBufferBounces) {
    const size_t len = 4096;
    std::vector<uint8_t> w(len), r(len, 0);
    for (size_t i = 0; i < len; ++i) w[i] = static_cast<uint8_t>(i * 13 + 1);

    ASSERT_EQ(io_.pwrite(w.data(), len, 32768).run_sync(),
              static_cast<ssize_t>(len));
    const uint64_t before = SpdkIO::thread_bounce_count();
    ASSERT_EQ(io_.pwrite(w.data(), len, 32768).run_sync(),
              static_cast<ssize_t>(len));
    ASSERT_EQ(io_.pread(r.data(), len, 32768).run_sync(),
              static_cast<ssize_t>(len));
    EXPECT_EQ(SpdkIO::thread_bounce_count(), before + 2);
    EXPECT_EQ(w, r);
}

// A DMA buffer that does not cover whole sectors bounces too: the device
// would otherwise transfer past the bytes asked for.
TEST_F(SpdkIOTest, PartialSectorIoBounces) {
    const size_t sector = 512;
    auto w = io_.alloc_buffer(2 * sector);
    ASSERT_NE(w.data, nullptr);
    std::memset(w.data, 0x5A, 2 * sector);
    ASSERT_EQ(io_.pwrite(w.data, 2 * sector, 40960).run_sync(),
              static_cast<ssize_t>(2 * sector));

    auto r = io_.alloc_buffer(2 * sector);
    ASSERT_NE(r.data, nullptr);
    std::memset(r.data, 0, 2 * sector);
    const uint64_t before = SpdkIO::thread_bounce_count();
    // 100 bytes at a non-sector offset: only those may be written to r.
    ASSERT_EQ(io_.pread(r.data, 100, 40960 + 7).run_sync(), 100);
    EXPECT_EQ(SpdkIO::thread_bounce_count(), before + 1);
    for (size_t i = 0; i < 100; ++i)
        ASSERT_EQ(static_cast<uint8_t*>(r.data)[i], 0x5A) << i;
    for (size_t i = 100; i < 2 * sector; ++i)
        ASSERT_EQ(static_cast<uint8_t*>(r.data)[i], 0) << i;
}

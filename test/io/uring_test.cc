// Copyright (c) 2024-2026 Nikolas Ioannou
// SPDX-License-Identifier: BSD-3-Clause

#include "udepot/io/uring.h"

#include <cstring>
#include <filesystem>
#include <vector>

#include <gtest/gtest.h>

using udepot::UringIO;

class UringIOTest : public ::testing::Test {
protected:
    void SetUp() override {
        path_ = std::filesystem::temp_directory_path() / "udepot_uring_test";
    }

    void TearDown() override {
        std::filesystem::remove(path_);
    }

    std::filesystem::path path_;
};

TEST_F(UringIOTest, OpenAndClose) {
    UringIO io;
    EXPECT_EQ(io.open(path_.c_str(), 4096), 0);
    EXPECT_EQ(io.get_size(), 4096u);
    io.close();
}

TEST_F(UringIOTest, WriteAndRead) {
    UringIO io;
    ASSERT_EQ(io.open(path_.c_str(), 4096), 0);

    auto wbuf = io.alloc_buffer(4096);
    ASSERT_NE(wbuf.data, nullptr);
    const char msg[] = "hello uring";
    std::memcpy(wbuf.data, msg, sizeof(msg));

    auto wr = io.pwrite(wbuf.data, 4096, 0);
    EXPECT_EQ(wr.run_sync(), static_cast<ssize_t>(4096));

    auto rbuf = io.alloc_buffer(4096);
    ASSERT_NE(rbuf.data, nullptr);
    auto rd = io.pread(rbuf.data, 4096, 0);
    EXPECT_EQ(rd.run_sync(), static_cast<ssize_t>(4096));
    EXPECT_EQ(std::memcmp(rbuf.data, msg, sizeof(msg)), 0);
}

TEST_F(UringIOTest, WriteAtOffset) {
    UringIO io;
    ASSERT_EQ(io.open(path_.c_str(), 8192), 0);

    auto wbuf = io.alloc_buffer(4096);
    ASSERT_NE(wbuf.data, nullptr);
    const char msg[] = "at offset 4096";
    std::memcpy(wbuf.data, msg, sizeof(msg));

    auto wr = io.pwrite(wbuf.data, 4096, 4096);
    EXPECT_EQ(wr.run_sync(), static_cast<ssize_t>(4096));

    auto rbuf = io.alloc_buffer(4096);
    ASSERT_NE(rbuf.data, nullptr);
    auto rd = io.pread(rbuf.data, 4096, 4096);
    EXPECT_EQ(rd.run_sync(), static_cast<ssize_t>(4096));
    EXPECT_EQ(std::memcmp(rbuf.data, msg, sizeof(msg)), 0);
}

TEST_F(UringIOTest, AllocBuffer) {
    UringIO io;
    auto buf = io.alloc_buffer(1024);
    EXPECT_NE(buf.data, nullptr);
    EXPECT_GE(buf.capacity, 1024u);
    EXPECT_EQ(reinterpret_cast<uintptr_t>(buf.data) % 4096, 0u);
}

TEST_F(UringIOTest, AllocBufferRoundsUp) {
    UringIO io;
    auto buf = io.alloc_buffer(100);
    EXPECT_NE(buf.data, nullptr);
    EXPECT_GE(buf.capacity, 4096u);
}

TEST_F(UringIOTest, CoroutineChaining) {
    UringIO io;
    ASSERT_EQ(io.open(path_.c_str(), 8192), 0);

    auto write_then_read = [&]() -> udepot::CoroTask<ssize_t> {
        auto wbuf = io.alloc_buffer(4096);
        if (!wbuf.data) co_return -1;
        const char data[] = "chained";
        std::memcpy(wbuf.data, data, sizeof(data));

        ssize_t wr = co_await io.pwrite(wbuf.data, 4096, 0);
        if (wr < 0) co_return wr;

        auto rbuf = io.alloc_buffer(4096);
        if (!rbuf.data) co_return -1;
        ssize_t rd = co_await io.pread(rbuf.data, 4096, 0);
        if (rd < 0) co_return rd;

        co_return (std::memcmp(rbuf.data, data, sizeof(data)) == 0) ? rd : -1;
    };

    EXPECT_GT(write_then_read().run_sync(), 0);
}

TEST_F(UringIOTest, MultipleSequentialOps) {
    UringIO io;
    ASSERT_EQ(io.open(path_.c_str(), 8192), 0);

    auto wbuf = io.alloc_buffer(4096);
    ASSERT_NE(wbuf.data, nullptr);

    for (int i = 0; i < 10; ++i) {
        std::memset(wbuf.data, 0, 4096);
        uint32_t val = static_cast<uint32_t>(i);
        std::memcpy(wbuf.data, &val, sizeof(val));

        auto wr = io.pwrite(wbuf.data, 4096, 0);
        EXPECT_EQ(wr.run_sync(), static_cast<ssize_t>(4096));

        auto rbuf = io.alloc_buffer(4096);
        ASSERT_NE(rbuf.data, nullptr);
        auto rd = io.pread(rbuf.data, 4096, 0);
        EXPECT_EQ(rd.run_sync(), static_cast<ssize_t>(4096));

        uint32_t readback = 0;
        std::memcpy(&readback, rbuf.data, sizeof(readback));
        EXPECT_EQ(readback, val);
    }
}

// Write real blocks first: reads of never-written (unwritten-extent) blocks
// complete inline without device I/O, so nothing would stay in flight.
static void fill_blocks(UringIO& io, size_t block, int blocks) {
    auto buf = io.alloc_buffer(block * blocks);
    ASSERT_NE(buf.data, nullptr);
    std::memset(buf.data, 0x5a, block * blocks);
    ASSERT_EQ(io.pwrite(buf.data, block * blocks, 0).run_sync(),
              static_cast<ssize_t>(block * blocks));
}

// More I/Os in flight than the completion queue (2 x 1024) holds, all
// submitted from one thread without waiting. Every one must complete with
// the full length: a submit the kernel refuses (EBUSY/EAGAIN) is retried,
// never handed back as an I/O error while its SQE sits in the ring.
TEST_F(UringIOTest, MoreInFlightThanCompletionQueue) {
    constexpr int kInFlight = 8192;
    constexpr int kBlocks = 1024;
    constexpr size_t kBlock = 4096;
    UringIO io;
    ASSERT_EQ(io.open(path_.c_str(), kBlock * kBlocks), 0);
    fill_blocks(io, kBlock, kBlocks);

    auto buf = io.alloc_buffer(kBlock * kInFlight);
    ASSERT_NE(buf.data, nullptr);
    std::vector<udepot::CoroTask<ssize_t>> tasks;
    tasks.reserve(kInFlight);
    for (int i = 0; i < kInFlight; ++i)
        tasks.push_back(io.pread(static_cast<char*>(buf.data) + i * kBlock,
                                 kBlock, (i % kBlocks) * kBlock));
    int bad = 0;
    for (auto& t : tasks)
        if (t.run_sync() != static_cast<ssize_t>(kBlock)) ++bad;
    EXPECT_EQ(bad, 0);
}

// close() with I/O in flight must complete it rather than stop the poller
// and leave those coroutines suspended forever.
TEST_F(UringIOTest, CloseCompletesInFlightIo) {
    constexpr int kInFlight = 4096;
    constexpr int kBlocks = 1024;
    constexpr size_t kBlock = 4096;
    UringIO io;
    ASSERT_EQ(io.open(path_.c_str(), kBlock * kBlocks), 0);
    fill_blocks(io, kBlock, kBlocks);

    auto buf = io.alloc_buffer(kBlock * kInFlight);
    ASSERT_NE(buf.data, nullptr);
    std::vector<udepot::CoroTask<ssize_t>> tasks;
    tasks.reserve(kInFlight);
    for (int i = 0; i < kInFlight; ++i)
        tasks.push_back(io.pread(static_cast<char*>(buf.data) + i * kBlock,
                                 kBlock, (i % kBlocks) * kBlock));
    io.close();

    int stranded = 0;
    for (auto& t : tasks)
        if (!t.done()) ++stranded;
    EXPECT_EQ(stranded, 0) << "I/O left in flight by close()";
    if (stranded == 0) {
        for (auto& t : tasks) EXPECT_EQ(t.run_sync(), ssize_t{kBlock});
    }
}

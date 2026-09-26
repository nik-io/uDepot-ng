// Copyright (c) 2024-2026 Nikolas Ioannou
// SPDX-License-Identifier: BSD-3-Clause

#include "udepot/io/aio.h"

#include <cstring>
#include <filesystem>

#include <gtest/gtest.h>

using udepot::AioIO;

class AioIOTest : public ::testing::Test {
protected:
    void SetUp() override {
        path_ = std::filesystem::temp_directory_path() / "udepot_aio_test";
    }

    void TearDown() override {
        std::filesystem::remove(path_);
    }

    std::filesystem::path path_;
};

TEST_F(AioIOTest, OpenAndClose) {
    AioIO io;
    EXPECT_EQ(io.open(path_.c_str(), 4096), 0);
    EXPECT_EQ(io.get_size(), 4096u);
    io.close();
}

TEST_F(AioIOTest, WriteAndRead) {
    AioIO io;
    ASSERT_EQ(io.open(path_.c_str(), 4096), 0);

    auto wbuf = io.alloc_buffer(4096);
    ASSERT_NE(wbuf.data, nullptr);
    const char msg[] = "hello aio";
    std::memcpy(wbuf.data, msg, sizeof(msg));

    auto wr = io.pwrite(wbuf.data, 4096, 0);
    EXPECT_EQ(wr.run_sync(), static_cast<ssize_t>(4096));

    auto rbuf = io.alloc_buffer(4096);
    ASSERT_NE(rbuf.data, nullptr);
    auto rd = io.pread(rbuf.data, 4096, 0);
    EXPECT_EQ(rd.run_sync(), static_cast<ssize_t>(4096));
    EXPECT_EQ(std::memcmp(rbuf.data, msg, sizeof(msg)), 0);
}

TEST_F(AioIOTest, WriteAtOffset) {
    AioIO io;
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

TEST_F(AioIOTest, AllocBuffer) {
    AioIO io;
    auto buf = io.alloc_buffer(1024);
    EXPECT_NE(buf.data, nullptr);
    EXPECT_GE(buf.capacity, 1024u);
    EXPECT_EQ(reinterpret_cast<uintptr_t>(buf.data) % 4096, 0u);
}

TEST_F(AioIOTest, AllocBufferRoundsUp) {
    AioIO io;
    auto buf = io.alloc_buffer(100);
    EXPECT_NE(buf.data, nullptr);
    EXPECT_GE(buf.capacity, 4096u);
}

TEST_F(AioIOTest, CoroutineChaining) {
    AioIO io;
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

TEST_F(AioIOTest, MultipleSequentialOps) {
    AioIO io;
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

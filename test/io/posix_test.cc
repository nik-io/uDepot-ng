// Copyright (c) 2024-2026 Nikolas Ioannou
// SPDX-License-Identifier: BSD-3-Clause

#include "udepot/io/posix.h"

#include <cstdio>
#include <cstring>
#include <filesystem>

#include <gtest/gtest.h>

using udepot::PosixIO;

class PosixIOTest : public ::testing::Test {
protected:
    void SetUp() override {
        path_ = std::filesystem::temp_directory_path() / "udepot_posix_test";
    }

    void TearDown() override {
        std::filesystem::remove(path_);
    }

    std::filesystem::path path_;
};

TEST_F(PosixIOTest, OpenAndClose) {
    PosixIO io;
    EXPECT_EQ(io.open(path_.c_str(), 4096), 0);
    EXPECT_EQ(io.get_size(), 4096u);
    EXPECT_GE(io.fd(), 0);
    io.close();
}

TEST_F(PosixIOTest, WriteAndRead) {
    PosixIO io;
    ASSERT_EQ(io.open(path_.c_str(), 4096), 0);

    const char msg[] = "hello udepot-ng";
    auto wr = io.pwrite(msg, sizeof(msg), 0);
    EXPECT_EQ(wr.run_sync(), static_cast<ssize_t>(sizeof(msg)));

    char buf[sizeof(msg)] = {};
    auto rd = io.pread(buf, sizeof(buf), 0);
    EXPECT_EQ(rd.run_sync(), static_cast<ssize_t>(sizeof(buf)));
    EXPECT_EQ(std::memcmp(buf, msg, sizeof(msg)), 0);
}

TEST_F(PosixIOTest, WriteAtOffset) {
    PosixIO io;
    ASSERT_EQ(io.open(path_.c_str(), 8192), 0);

    const char msg[] = "at offset 4096";
    auto wr = io.pwrite(msg, sizeof(msg), 4096);
    EXPECT_EQ(wr.run_sync(), static_cast<ssize_t>(sizeof(msg)));

    char buf[sizeof(msg)] = {};
    auto rd = io.pread(buf, sizeof(buf), 4096);
    EXPECT_EQ(rd.run_sync(), static_cast<ssize_t>(sizeof(buf)));
    EXPECT_EQ(std::memcmp(buf, msg, sizeof(msg)), 0);
}

TEST_F(PosixIOTest, AllocBuffer) {
    PosixIO io;
    auto buf = io.alloc_buffer(1024);
    EXPECT_NE(buf.data, nullptr);
    EXPECT_EQ(buf.capacity, 1024u);
}

TEST_F(PosixIOTest, CoroutineChaining) {
    PosixIO io;
    ASSERT_EQ(io.open(path_.c_str(), 4096), 0);

    auto write_then_read = [&]() -> udepot::CoroTask<ssize_t> {
        const char data[] = "chained";
        ssize_t wr = co_await io.pwrite(data, sizeof(data), 0);
        if (wr < 0) co_return wr;

        char buf[sizeof(data)] = {};
        ssize_t rd = co_await io.pread(buf, sizeof(buf), 0);
        if (rd < 0) co_return rd;

        co_return (std::memcmp(buf, data, sizeof(data)) == 0) ? rd : -1;
    };

    EXPECT_GT(write_then_read().run_sync(), 0);
}

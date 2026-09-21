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

    const char msg[] = "hello aio";
    auto wr = io.pwrite(msg, sizeof(msg), 0);
    EXPECT_EQ(wr.run_sync(), static_cast<ssize_t>(sizeof(msg)));

    char buf[sizeof(msg)] = {};
    auto rd = io.pread(buf, sizeof(buf), 0);
    EXPECT_EQ(rd.run_sync(), static_cast<ssize_t>(sizeof(buf)));
    EXPECT_EQ(std::memcmp(buf, msg, sizeof(msg)), 0);
}

TEST_F(AioIOTest, WriteAtOffset) {
    AioIO io;
    ASSERT_EQ(io.open(path_.c_str(), 8192), 0);

    const char msg[] = "at offset 4096";
    auto wr = io.pwrite(msg, sizeof(msg), 4096);
    EXPECT_EQ(wr.run_sync(), static_cast<ssize_t>(sizeof(msg)));

    char buf[sizeof(msg)] = {};
    auto rd = io.pread(buf, sizeof(buf), 4096);
    EXPECT_EQ(rd.run_sync(), static_cast<ssize_t>(sizeof(buf)));
    EXPECT_EQ(std::memcmp(buf, msg, sizeof(msg)), 0);
}

TEST_F(AioIOTest, AllocBuffer) {
    AioIO io;
    auto buf = io.alloc_buffer(1024);
    EXPECT_NE(buf.data, nullptr);
    EXPECT_GE(buf.capacity, 1024u);
    EXPECT_EQ(reinterpret_cast<uintptr_t>(buf.data) % 512, 0u);
}

TEST_F(AioIOTest, AllocBufferRoundsUp) {
    AioIO io;
    auto buf = io.alloc_buffer(100);
    EXPECT_NE(buf.data, nullptr);
    EXPECT_GE(buf.capacity, 512u);
}

TEST_F(AioIOTest, CoroutineChaining) {
    AioIO io;
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

TEST_F(AioIOTest, MultipleSequentialOps) {
    AioIO io;
    ASSERT_EQ(io.open(path_.c_str(), 8192), 0);

    for (int i = 0; i < 10; ++i) {
        uint32_t val = static_cast<uint32_t>(i);
        off_t off = static_cast<off_t>(i) * sizeof(val);

        auto wr = io.pwrite(&val, sizeof(val), off);
        EXPECT_EQ(wr.run_sync(), static_cast<ssize_t>(sizeof(val)));
    }

    for (int i = 0; i < 10; ++i) {
        uint32_t val = 0;
        off_t off = static_cast<off_t>(i) * sizeof(val);

        auto rd = io.pread(&val, sizeof(val), off);
        EXPECT_EQ(rd.run_sync(), static_cast<ssize_t>(sizeof(val)));
        EXPECT_EQ(val, static_cast<uint32_t>(i));
    }
}

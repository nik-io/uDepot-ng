// Copyright (c) 2024-2026 Nikolas Ioannou
// SPDX-License-Identifier: BSD-3-Clause

#include "udepot/io/aio.h"

#include <atomic>
#include <cstring>
#include <filesystem>
#include <thread>
#include <vector>

#include <sys/syscall.h>
#include <unistd.h>

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

// A thread's context is destroyed when the thread exits, and the kernel
// may hand its ring's address to the next io_setup. Nothing the program
// does orders the old thread's ring accesses before the new thread's -- the
// kernel does -- so under TSAN this reported a race on the ring head,
// intermittently, whenever the reuse happened under load. Here each thread
// starts only once the previous one is gone from the kernel, a wait TSAN
// cannot see, and none is joined before the last has run; the kernel reuses
// the address often enough that one of the generations hits it.
TEST_F(AioIOTest, ANewThreadsContextMayReuseAnExitedThreadsRing) {
    constexpr int kGenerations = 32;
    AioIO io;
    ASSERT_EQ(io.open(path_.c_str(), 4096), 0);
    auto buf = io.alloc_buffer(4096);
    ASSERT_NE(buf.data, nullptr);
    std::memset(buf.data, 0x5a, 4096);
    ASSERT_EQ(io.pwrite(buf.data, 4096, 0).run_sync(), ssize_t{4096});

    struct Generation {
        ssize_t rc = 0;
        std::atomic<pid_t> tid{0};
    };
    std::vector<Generation> gens(kGenerations);
    std::vector<std::thread> threads;
    for (Generation& g : gens) {
        threads.emplace_back([&io, &buf, &g] {
            g.rc = io.pread(buf.data, 4096, 0).run_sync();
            g.tid.store(gettid(), std::memory_order_relaxed);
        });
        pid_t tid;
        while ((tid = g.tid.load(std::memory_order_relaxed)) == 0)
            std::this_thread::yield();
        // Gone from the kernel: its thread-local context has been destroyed.
        while (syscall(SYS_tgkill, getpid(), tid, 0) == 0)
            std::this_thread::yield();
    }
    for (std::thread& t : threads) t.join();
    for (const Generation& g : gens) EXPECT_EQ(g.rc, ssize_t{4096});
}

// close() with I/O in flight from the closing thread must complete it
// rather than leave those coroutines suspended forever. Blocks are written first:
// reads of never-written blocks complete without device I/O.
TEST_F(AioIOTest, CloseCompletesInFlightIo) {
    constexpr int kInFlight = 512;  // within the 1024-event AIO context
    constexpr int kBlocks = 512;
    constexpr size_t kBlock = 4096;
    AioIO io;
    ASSERT_EQ(io.open(path_.c_str(), kBlock * kBlocks), 0);
    {
        auto fill = io.alloc_buffer(kBlock * kBlocks);
        ASSERT_NE(fill.data, nullptr);
        std::memset(fill.data, 0x5a, kBlock * kBlocks);
        ASSERT_EQ(io.pwrite(fill.data, kBlock * kBlocks, 0).run_sync(),
                  static_cast<ssize_t>(kBlock * kBlocks));
    }

    auto buf = io.alloc_buffer(kBlock * kInFlight);
    ASSERT_NE(buf.data, nullptr);
    std::vector<udepot::CoroTask<ssize_t>> tasks;
    tasks.reserve(kInFlight);
    for (int i = 0; i < kInFlight; ++i)
        tasks.push_back(io.pread(static_cast<char*>(buf.data) + i * kBlock,
                                 kBlock, i * kBlock));
    io.close();

    int stranded = 0;
    for (auto& t : tasks)
        if (!t.done()) ++stranded;
    EXPECT_EQ(stranded, 0) << "I/O left in flight by close()";
    if (stranded == 0) {
        for (auto& t : tasks) EXPECT_EQ(t.run_sync(), ssize_t{kBlock});
    }
}

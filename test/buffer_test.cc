#include "udepot/buffer.h"

#include <cstring>

#include <gtest/gtest.h>

using udepot::BufferAlloc;
using udepot::IoBuffer;

TEST(IoBuffer, DefaultConstructEmpty) {
    IoBuffer buf;
    EXPECT_EQ(buf.data, nullptr);
    EXPECT_EQ(buf.length, 0u);
    EXPECT_EQ(buf.capacity, 0u);
    EXPECT_TRUE(buf.empty());
}

TEST(IoBuffer, AllocMalloc) {
    auto buf = IoBuffer::alloc_malloc(4096);
    EXPECT_NE(buf.data, nullptr);
    EXPECT_EQ(buf.length, 0u);
    EXPECT_EQ(buf.capacity, 4096u);
    EXPECT_EQ(buf.alloc, BufferAlloc::kMalloc);
    EXPECT_EQ(buf.remaining(), 4096u);
}

TEST(IoBuffer, AllocAligned) {
    auto buf = IoBuffer::alloc_aligned(4096);
    EXPECT_NE(buf.data, nullptr);
    EXPECT_EQ(buf.capacity, 4096u);
    EXPECT_EQ(buf.alloc, BufferAlloc::kAligned);
    EXPECT_EQ(reinterpret_cast<uintptr_t>(buf.data) % 4096, 0u);
}

TEST(IoBuffer, View) {
    char data[] = "hello world";
    auto buf = IoBuffer::view(data, sizeof(data));
    EXPECT_EQ(buf.data, data);
    EXPECT_EQ(buf.length, sizeof(data));
    EXPECT_EQ(buf.capacity, sizeof(data));
    EXPECT_EQ(buf.alloc, BufferAlloc::kNone);
    EXPECT_EQ(buf.remaining(), 0u);
}

TEST(IoBuffer, MoveConstruct) {
    auto a = IoBuffer::alloc_malloc(1024);
    void* p = a.data;
    IoBuffer b(std::move(a));
    EXPECT_EQ(b.data, p);
    EXPECT_EQ(b.capacity, 1024u);
    EXPECT_EQ(a.data, nullptr);
}

TEST(IoBuffer, MoveAssign) {
    auto a = IoBuffer::alloc_malloc(1024);
    auto b = IoBuffer::alloc_malloc(2048);
    void* p = a.data;
    b = std::move(a);
    EXPECT_EQ(b.data, p);
    EXPECT_EQ(b.capacity, 1024u);
    EXPECT_EQ(a.data, nullptr);
}

TEST(IoBuffer, WriteAndRead) {
    auto buf = IoBuffer::alloc_malloc(256);
    const char msg[] = "test data";
    std::memcpy(buf.data, msg, sizeof(msg));
    buf.length = sizeof(msg);
    EXPECT_EQ(std::memcmp(buf.data, msg, sizeof(msg)), 0);
    EXPECT_FALSE(buf.empty());
}

TEST(IoBuffer, ViewDoesNotFree) {
    auto owned = IoBuffer::alloc_malloc(64);
    void* p = owned.data;
    {
        auto v = IoBuffer::view(p, 64);
        EXPECT_EQ(v.data, p);
    }
    // owned buffer is still valid after the view is destroyed
    std::memset(owned.data, 0xff, 64);
    auto* bytes = static_cast<uint8_t*>(owned.data);
    EXPECT_EQ(bytes[0], 0xff);
}

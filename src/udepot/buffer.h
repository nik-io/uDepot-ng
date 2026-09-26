// Copyright (c) 2024-2026 Nikolas Ioannou
// SPDX-License-Identifier: BSD-3-Clause

#pragma once

#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <utility>

#ifdef UDEPOT_BUILD_SPDK
#include <rte_malloc.h>
#endif

namespace udepot {

// How the buffer was allocated — determines the deallocation path.
enum class BufferAlloc : uint8_t {
    kNone,      // not owned (view into external memory)
    kMalloc,    // std::free
    kAligned,   // std::free (from std::aligned_alloc)
    kDma,       // rte_free (SPDK DMA-safe memory)
};

// A contiguous, owned buffer.
//
// Zero-copy path: for put, the caller passes their buffer and the backend
// writes from it directly. For get, the caller passes a pre-allocated
// buffer and the backend reads into it. For SPDK, alloc_buffer() on the
// backend returns a DMA-safe buffer.
struct IoBuffer {
    void* data = nullptr;
    size_t length = 0;
    size_t capacity = 0;
    BufferAlloc alloc = BufferAlloc::kNone;

    IoBuffer() noexcept = default;

    IoBuffer(void* d, size_t len, size_t cap, BufferAlloc a) noexcept
        : data(d), length(len), capacity(cap), alloc(a) {}

    IoBuffer(const IoBuffer&) = delete;
    IoBuffer& operator=(const IoBuffer&) = delete;

    IoBuffer(IoBuffer&& other) noexcept
        : data(std::exchange(other.data, nullptr)),
          length(std::exchange(other.length, 0)),
          capacity(std::exchange(other.capacity, 0)),
          alloc(std::exchange(other.alloc, BufferAlloc::kNone)) {}

    IoBuffer& operator=(IoBuffer&& other) noexcept {
        if (this != &other) {
            free();
            data = std::exchange(other.data, nullptr);
            length = std::exchange(other.length, 0);
            capacity = std::exchange(other.capacity, 0);
            alloc = std::exchange(other.alloc, BufferAlloc::kNone);
        }
        return *this;
    }

    ~IoBuffer() { free(); }

    bool empty() const noexcept { return length == 0; }
    size_t remaining() const noexcept { return capacity - length; }

    // Allocate a buffer with malloc.
    static IoBuffer alloc_malloc(size_t size) {
        void* p = std::malloc(size);
        if (!p) return {};
        return {p, 0, size, BufferAlloc::kMalloc};
    }

    // Allocate an aligned buffer (for O_DIRECT).
    static IoBuffer alloc_aligned(size_t size, size_t alignment = 4096) {
        void* p = std::aligned_alloc(alignment, size);
        if (!p) return {};
        return {p, 0, size, BufferAlloc::kAligned};
    }

    // Allocate a DMA-safe buffer (SPDK hugepage memory).
    static IoBuffer alloc_dma(size_t size, size_t alignment = 512) {
#ifdef UDEPOT_BUILD_SPDK
        void* p = rte_malloc_socket(nullptr, size, alignment, SOCKET_ID_ANY);
        if (!p) return {};
        return {p, 0, size, BufferAlloc::kDma};
#else
        (void)alignment;
        (void)size;
        return {};
#endif
    }

    // Create a non-owning view into existing memory.
    static IoBuffer view(void* data, size_t length) {
        return {data, length, length, BufferAlloc::kNone};
    }

    static IoBuffer view(const void* data, size_t length) {
        return {const_cast<void*>(data), length, length, BufferAlloc::kNone};
    }

private:
    void free() noexcept {
        if (!data) return;
        switch (alloc) {
            case BufferAlloc::kMalloc:
            case BufferAlloc::kAligned:
                std::free(data);
                break;
            case BufferAlloc::kDma:
#ifdef UDEPOT_BUILD_SPDK
                rte_free(data);
#endif
                break;
            case BufferAlloc::kNone:
                break;
        }
        data = nullptr;
        length = 0;
        capacity = 0;
    }
};

}  // namespace udepot

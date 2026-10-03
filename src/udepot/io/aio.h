// Copyright (c) 2024-2026 Nikolas Ioannou
// SPDX-License-Identifier: BSD-3-Clause

#pragma once

#include <atomic>
#include <cstddef>
#include <sys/types.h>
#include <thread>

#include <linux/aio_abi.h>

#include "udepot/buffer.h"
#include "udepot/coro.h"
#include "udepot/io/fd_io.h"

namespace udepot {

// Kernel AIO backend. The context is sized for queue_depth() I/Os (the
// kernel rounds it up). An I/O the kernel refuses because the context is
// full fails with -EAGAIN, as in uDepot; nothing waits for room.
class AioIO {
public:
    static constexpr unsigned kDefaultQueueDepth = 1024;

    AioIO() noexcept = default;
    ~AioIO();

    AioIO(const AioIO&) = delete;
    AioIO& operator=(const AioIO&) = delete;

    // Before open(): the number of I/Os the caller expects to have in
    // flight at once; 0 keeps the default.
    void set_queue_depth(unsigned n) noexcept {
        queue_depth_ = n > 0 ? n : kDefaultQueueDepth;
    }
    unsigned queue_depth() const noexcept { return queue_depth_; }

    int open(const char* path, size_t size);
    // Completes all I/O already submitted, then tears down. No new I/O may
    // be submitted once close() has begun (UDepot::close guarantees this).
    void close();

    CoroTask<ssize_t> pread(void* buf, size_t count, off_t offset);
    CoroTask<ssize_t> pwrite(const void* buf, size_t count, off_t offset);

    ssize_t pwrite_sync(const void* buf, size_t count, off_t offset) {
        return pwrite_full_fd(fd_, buf, count, offset);
    }

    size_t get_size() const noexcept { return size_; }
    IoBuffer alloc_buffer(size_t size);

private:
    int fd_ = -1;
    size_t size_ = 0;
    unsigned queue_depth_ = kDefaultQueueDepth;
    aio_context_t ctx_ = 0;
    std::thread poller_;
    std::atomic<bool> running_{false};
    // Submitted and not yet completed; kept off the read-mostly fields.
    alignas(64) std::atomic<size_t> pending_{0};

    void poller_loop();
};

}  // namespace udepot

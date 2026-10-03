// Copyright (c) 2024-2026 Nikolas Ioannou
// SPDX-License-Identifier: BSD-3-Clause

#pragma once

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <mutex>
#include <sys/types.h>
#include <thread>

#include <linux/aio_abi.h>

#include "udepot/buffer.h"
#include "udepot/coro.h"
#include "udepot/io/fd_io.h"

namespace udepot {

struct AioRequest;

// Kernel AIO backend. The context is sized for queue_depth() I/Os; that is
// tracking state only, not a limit: the caller may have any number of I/Os
// outstanding. One the kernel refuses because its context is full (EAGAIN)
// waits, in submission order, and the poller submits it once completions
// free room. Waiting allocates nothing: the queue links the requests, which
// live in the suspended coroutines' frames. (uDepot failed such an I/O.)
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

    // I/Os that found the kernel's context full and had to wait, since
    // open(). For tests, to show that path actually ran.
    uint64_t waited_count() const noexcept {
        return waited_total_.load(std::memory_order_relaxed);
    }

private:
    friend struct AioSubmitAwaitable;

    int fd_ = -1;
    size_t size_ = 0;
    unsigned queue_depth_ = kDefaultQueueDepth;
    aio_context_t ctx_ = 0;
    std::thread poller_;
    std::atomic<bool> running_{false};
    // Submitted and not yet completed; kept off the read-mostly fields.
    alignas(64) std::atomic<size_t> pending_{0};
    // Requests waiting for room in the context. The count is read without
    // the lock.
    alignas(64) std::atomic<size_t> waiting_{0};
    std::atomic<uint64_t> waited_total_{0};
    std::mutex wait_mu_;
    AioRequest* wait_head_ = nullptr;
    AioRequest* wait_tail_ = nullptr;

    // Submits one request; 0, or the errno that refused it.
    int submit_one(AioRequest* req) noexcept;
    // Queues req behind any waiting and submits what the kernel takes.
    // Returns false if req itself failed (its result is set); other
    // requests that failed are resumed.
    bool wait_for_room(AioRequest* req);
    // Submits waiting requests until the kernel refuses one. Requests that
    // failed are linked into *failed; wait_mu_ held.
    void submit_waiting_locked(AioRequest** failed);
    void submit_waiting();

    void poller_loop();
};

}  // namespace udepot

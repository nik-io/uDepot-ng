// Copyright (c) 2024-2026 Nikolas Ioannou
// SPDX-License-Identifier: BSD-3-Clause

#pragma once

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <mutex>
#include <sys/types.h>
#include <thread>

#include <liburing.h>

#include "udepot/buffer.h"
#include "udepot/coro.h"
#include "udepot/io/fd_io.h"

namespace udepot {

struct UringRequest;

// io_uring backend. The ring is sized for queue_depth() I/Os (its
// completion queue holds twice that); that is tracking state only, not a
// limit: the caller may have any number of I/Os outstanding, and the
// kernel keeps completions past the completion queue's size on its
// overflow list. An I/O waits only if the kernel refuses submissions
// (EBUSY/EAGAIN) until the submission queue is full: then it waits, in
// submission order, and the poller submits it once it has reaped. Waiting
// allocates nothing: the queue links the requests, which live in the
// suspended coroutines' frames. (uDepot failed an I/O it could not get a
// submission entry for.)
class UringIO {
public:
    static constexpr unsigned kDefaultQueueDepth = 1024;

    UringIO() noexcept = default;
    ~UringIO();

    UringIO(const UringIO&) = delete;
    UringIO& operator=(const UringIO&) = delete;

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

    // I/Os that found the submission queue full and had to wait, since
    // open(). For tests.
    uint64_t waited_count() const noexcept {
        return waited_total_.load(std::memory_order_relaxed);
    }

private:
    friend struct UringSubmitAwaitable;

    int fd_ = -1;
    size_t size_ = 0;
    unsigned queue_depth_ = kDefaultQueueDepth;
    struct io_uring ring_{};
    bool ring_initialized_ = false;
    std::thread poller_;
    // Guards the submission queue and the wait queue.
    alignas(64) std::mutex sq_mutex_;
    UringRequest* wait_head_ = nullptr;
    UringRequest* wait_tail_ = nullptr;
    alignas(64) std::atomic<bool> running_{false};
    // In the ring (queued or submitted) and not yet reaped.
    alignas(64) std::atomic<size_t> pending_{0};
    // Waiting for a submission entry. The count is read without the lock.
    std::atomic<size_t> waiting_{0};
    // The kernel refused a submission (EBUSY/EAGAIN): SQEs are left in the
    // SQ for the poller to push once it has reaped.
    std::atomic<bool> sq_backlog_{false};
    std::atomic<uint64_t> waited_total_{0};

    // Gets an SQE for req and preps it; false if the SQ is full.
    bool queue_locked(UringRequest* req) noexcept;
    // Pushes queued SQEs to the kernel.
    void submit_locked() noexcept;
    // Moves waiting requests into the SQ while it has room, and submits.
    void submit_waiting_locked() noexcept;

    void poller_loop();
};

}  // namespace udepot

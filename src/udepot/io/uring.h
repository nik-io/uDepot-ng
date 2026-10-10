// Copyright (c) 2024-2026 Nikolas Ioannou
// SPDX-License-Identifier: BSD-3-Clause

#pragma once

#include <atomic>
#include <cstddef>
#include <mutex>
#include <sys/types.h>
#include <thread>

#include <liburing.h>

#include "udepot/buffer.h"
#include "udepot/coro.h"
#include "udepot/io/fd_io.h"

namespace udepot {

// io_uring backend. The ring is sized for queue_depth() I/Os (its
// completion queue holds twice that); the kernel accepts more in flight,
// keeping the extra completions on its overflow list. An I/O that finds no
// free submission entry (the kernel refusing submissions, EBUSY/EAGAIN,
// until the submission queue fills) fails with -EAGAIN, as in uDepot.
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
    // be submitted once close() has begun (uDepot::close guarantees this).
    void close();

    CoroTask<ssize_t> pread(void* buf, size_t count, off_t offset);
    CoroTask<ssize_t> pwrite(const void* buf, size_t count, off_t offset);

    ssize_t pwrite_sync(const void* buf, size_t count, off_t offset) {
        return pwrite_full_fd(fd_, buf, count, offset);
    }

    size_t get_size() const noexcept { return size_; }
    IoBuffer alloc_buffer(size_t size);

    // Test seam, never set in production: called before each
    // io_uring_submit; a negative return is used as its result instead
    // (e.g. -EBUSY, which this kernel never returns), 0 submits for real.
    inline static std::atomic<int (*)()> submit_test_hook{nullptr};

private:
    friend struct UringSubmitAwaitable;

    int fd_ = -1;
    size_t size_ = 0;
    unsigned queue_depth_ = kDefaultQueueDepth;
    struct io_uring ring_{};
    bool ring_initialized_ = false;
    std::thread poller_;
    // Guards the submission queue.
    alignas(64) std::mutex sq_mutex_;
    alignas(64) std::atomic<bool> running_{false};
    // In the ring (queued or submitted) and not yet reaped.
    alignas(64) std::atomic<size_t> pending_{0};
    // The kernel refused a submission (EBUSY/EAGAIN): SQEs are left in the
    // SQ for the poller to push once it has reaped.
    std::atomic<bool> sq_backlog_{false};

    // Pushes queued SQEs to the kernel; sq_mutex_ held.
    void submit_locked() noexcept;

    void poller_loop();
};

}  // namespace udepot

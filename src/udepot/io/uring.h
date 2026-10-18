// Copyright (c) 2024-2026 Nikolas Ioannou
// SPDX-License-Identifier: BSD-3-Clause

#pragma once

#include <atomic>
#include <cstddef>
#include <sys/types.h>

#include <liburing.h>

#include "udepot/buffer.h"
#include "udepot/coro.h"
#include "udepot/io/fd_io.h"

namespace udepot {

// io_uring backend. As uDepot's TRT keeps one ring per scheduler thread
// (UringState), each thread has a ring of its own, shared by every UringIO
// it uses: only that thread submits to it and reaps it (run_sync() drives
// the reaping through set_thread_poll), so a coroutine stays on the thread
// that submitted its I/O and the ring needs no lock.
//
// A ring is sized for queue_depth() I/Os (its completion queue holds twice
// that); the kernel accepts more in flight, keeping the extra completions
// on its overflow list. An I/O that finds no free submission entry (the
// kernel refusing submissions, EBUSY/EAGAIN, until the submission queue
// fills) fails with -EAGAIN, as in uDepot.
class UringIO {
public:
    static constexpr unsigned kDefaultQueueDepth = 1024;

    UringIO() noexcept = default;
    ~UringIO();

    UringIO(const UringIO&) = delete;
    UringIO& operator=(const UringIO&) = delete;

    // Before open(): the number of I/Os the caller expects to have in
    // flight at once on a thread; 0 keeps the default. open() sizes the
    // calling thread's ring for it (recreating it if it is idle), and rings
    // created from then on, on other threads, take it too; a thread's busy
    // ring keeps its size. As SpdkIO's queue pairs.
    void set_queue_depth(unsigned n) noexcept {
        queue_depth_ = n > 0 ? n : kDefaultQueueDepth;
    }
    unsigned queue_depth() const noexcept { return queue_depth_; }

    // Before open(): whether a poll that finds nothing completed yields
    // the CPU (sched_yield) rather than returning to spin, as uDepot's TRT
    // poller does (the default). An I/O the kernel cannot do inline (every
    // one on tmpfs, buffered writes, a device without nowait) runs on an
    // io-wq worker, which the scheduler wakes on the submitting thread's
    // CPU; a spinning thread holds it off until preemption. Rings are per
    // thread and shared by every store a thread uses, so this is
    // process-wide, as the depth of new rings: the last open() sets it.
    void set_poll_yields(bool yields) noexcept { poll_yields_set_ = yields; }

    int open(const char* path, size_t size);
    // Completes the I/O the calling thread has in flight (resuming its
    // coroutines); I/O other threads have in flight they reap themselves.
    // No new I/O may be submitted once close() has begun (uDepot::close
    // guarantees this). The threads' rings outlive it.
    void close();

    CoroTask<ssize_t> pread(void* buf, size_t count, off_t offset);
    CoroTask<ssize_t> pwrite(const void* buf, size_t count, off_t offset);

    ssize_t pwrite_sync(const void* buf, size_t count, off_t offset) {
        return pwrite_full_fd(fd_, buf, count, offset);
    }

    size_t get_size() const noexcept { return size_; }
    // Whether the file is open O_DIRECT (see open_direct): a filesystem
    // that refuses it gets buffered I/O, and no zero copy.
    bool direct() const noexcept { return direct_; }
    int fd_for_test() const noexcept { return fd_; }
    IoBuffer alloc_buffer(size_t size);

    // Test seam, never set in production: called before each
    // io_uring_submit; a negative return is used as its result instead
    // (e.g. -EBUSY, which this kernel never returns), 0 submits for real.
    inline static std::atomic<int (*)()> submit_test_hook{nullptr};

    // Reap this thread's completed I/Os and resume their coroutines, and
    // push submissions the kernel refused. Returns whether any of this
    // thread's I/O is still outstanding.
    static bool poll_thread();

    // How many of the calling thread's polls found nothing and yielded.
    static uint64_t thread_idle_yields() noexcept;

private:
    friend struct UringSubmitAwaitable;

    int fd_ = -1;
    size_t size_ = 0;
    bool direct_ = false;
    unsigned queue_depth_ = kDefaultQueueDepth;
    bool poll_yields_set_ = false;

    // Size of the rings threads create from now on.
    static std::atomic<unsigned> new_ring_depth_;
    // Whether an empty poll yields (set_poll_yields).
    static std::atomic<bool> poll_yields_;
};

}  // namespace udepot

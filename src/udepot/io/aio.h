// Copyright (c) 2024-2026 Nikolas Ioannou
// SPDX-License-Identifier: BSD-3-Clause

#pragma once

#include <atomic>
#include <cstddef>
#include <sys/types.h>

#include <linux/aio_abi.h>

#include "udepot/buffer.h"
#include "udepot/coro.h"
#include "udepot/io/fd_io.h"

namespace udepot {

// Kernel AIO backend. As uDepot's TRT keeps one AIO context per scheduler
// thread (AioState), each thread has a context of its own, shared by every
// AioIO it uses: an I/O is submitted there, its coroutine suspends, and
// the same thread reaps the completion when it polls (run_sync() drives
// it through set_thread_poll), so a coroutine stays on the thread that
// submitted its I/O. No thread hands completions to another.
//
// A context is sized for queue_depth() I/Os (the kernel rounds it up). An
// I/O the kernel refuses because the context is full fails with -EAGAIN,
// as in uDepot; nothing waits for room. A thread's first I/O creates its
// context; if none can be had (the system's contexts already hold
// fs.aio-max-nr requests) that I/O fails with -ENOMEM, not -EAGAIN: no
// completion of the thread's own frees the room, so a retry would spin.
// (uDepot exits.)
class AioIO {
public:
    static constexpr unsigned kDefaultQueueDepth = 1024;

    AioIO() noexcept = default;
    ~AioIO();

    AioIO(const AioIO&) = delete;
    AioIO& operator=(const AioIO&) = delete;

    // Before open(): the number of I/Os the caller expects to have in
    // flight at once on a thread; 0 keeps the default. open() sizes the
    // calling thread's context for it (recreating it if it is idle), and
    // contexts created from then on, on other threads, take it too; a
    // thread's busy context keeps its size. As SpdkIO's queue pairs.
    void set_queue_depth(unsigned n) noexcept {
        queue_depth_ = n > 0 ? n : kDefaultQueueDepth;
    }
    unsigned queue_depth() const noexcept { return queue_depth_; }

    int open(const char* path, size_t size);
    // Completes the I/O the calling thread has in flight (resuming its
    // coroutines); I/O other threads have in flight they reap themselves.
    // No new I/O may be submitted once close() has begun (uDepot::close
    // guarantees this). The threads' contexts outlive it.
    void close();

    CoroTask<ssize_t> pread(void* buf, size_t count, off_t offset);
    CoroTask<ssize_t> pwrite(const void* buf, size_t count, off_t offset);

    ssize_t pwrite_sync(const void* buf, size_t count, off_t offset) {
        return pwrite_full_fd(fd_, buf, count, offset);
    }

    size_t get_size() const noexcept { return size_; }
    IoBuffer alloc_buffer(size_t size);

    // Reap this thread's completed I/Os and resume their coroutines.
    // Returns whether any of this thread's I/O is still outstanding.
    static bool poll_thread();

    // Test seam, never set in production: called before each io_setup;
    // returning -errno fails it with that, as the kernel would with every
    // context taken (-EAGAIN). 0 lets it go on.
    using SetupHook = int (*)();
    inline static std::atomic<SetupHook> setup_test_hook{nullptr};

    // Test seam, never set in production: release builds reap completions
    // from the context's ring in user space, debug builds with io_getevents
    // (as uDepot's aio_user_getevents). Set, a debug build reads the ring
    // too, so a debug TSAN run covers what touches it. No effect in release.
    inline static std::atomic<bool> user_ring_test_override{false};

private:
    friend struct AioSubmitAwaitable;

    int fd_ = -1;
    size_t size_ = 0;
    unsigned queue_depth_ = kDefaultQueueDepth;

    // Size of the contexts threads create from now on.
    static std::atomic<unsigned> new_ctx_depth_;
};

}  // namespace udepot

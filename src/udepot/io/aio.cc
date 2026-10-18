// Copyright (c) 2024-2026 Nikolas Ioannou
// SPDX-License-Identifier: BSD-3-Clause

#include "udepot/io/aio.h"
#include "udepot/tsan.h"

#include <cerrno>
#include <coroutine>
#include <cstring>
#include <fcntl.h>
#include <sys/stat.h>
#include <sys/syscall.h>
#include <unistd.h>

namespace udepot {

static inline int sys_io_setup(unsigned nr, aio_context_t* ctx) {
    return static_cast<int>(syscall(SYS_io_setup, nr, ctx));
}

// The context is the address of its completion ring, mapped and unmapped by
// the kernel. A context another thread destroyed can come back at the same
// address from io_setup, so the old thread's ring accesses are ordered
// before the new one's by the kernel, which TSAN cannot see.
static inline int sys_io_destroy(aio_context_t ctx) {
    tsan_release_to_kernel(reinterpret_cast<void*>(ctx));
    return static_cast<int>(syscall(SYS_io_destroy, ctx));
}

static inline int sys_io_submit(aio_context_t ctx, long nr,
                                struct iocb** iocbs) {
    return static_cast<int>(syscall(SYS_io_submit, ctx, nr, iocbs));
}

static inline int sys_io_getevents(aio_context_t ctx, long min_nr,
                                   long max_nr, struct io_event* events,
                                   struct timespec* timeout) {
    return static_cast<int>(
        syscall(SYS_io_getevents, ctx, min_nr, max_nr, events, timeout));
}

// User-space AIO ring buffer — the kernel maps the aio_context_t as a ring
// that userspace can poll directly, avoiding the io_getevents syscall.
// Enabled only in release builds, matching uDepot's aio_user_getevents.
struct AioRing {
    unsigned id;
    unsigned nr;
    unsigned head;
    unsigned tail;
    unsigned magic;
    unsigned compat_features;
    unsigned incompat_features;
    unsigned header_length;
    struct io_event events[];
};

static constexpr unsigned kAioRingMagic = 0xa10a10a1;

static inline bool aio_ring_valid(aio_context_t ctx) {
#ifndef NDEBUG
    // Debug builds reap with io_getevents, as uDepot's, unless a test asks
    // for the ring (AioIO::user_ring_test_override).
    if (!AioIO::user_ring_test_override.load(std::memory_order_relaxed))
        return false;
#endif
    return reinterpret_cast<AioRing*>(ctx)->magic == kAioRingMagic;
}

static inline int aio_ring_getevents(aio_context_t ctx, unsigned max,
                                     struct io_event* events) {
    auto* ring = reinterpret_cast<AioRing*>(ctx);
    int i = 0;
    while (static_cast<unsigned>(i) < max) {
        unsigned head = ring->head;
        if (head == ring->tail)
            break;
        events[i] = ring->events[head];
#if defined(__x86_64__) || defined(__i386__)
        __asm__ __volatile__("lfence" ::: "memory");
#else
        std::atomic_thread_fence(std::memory_order_acquire);
#endif
        ring->head = (head + 1) % ring->nr;
        ++i;
    }
    return i;
}

struct AioRequest {
    struct iocb cb;
    ssize_t result;
    std::coroutine_handle<> handle;
};

namespace {

// The calling thread's context, as uDepot's thread_local AioState: only
// this thread submits to it and reaps it, so it needs no synchronization.
struct AioThread {
    aio_context_t ctx = 0;
    unsigned depth = 0;
    size_t pending = 0;  // submitted, not yet reaped

    // depth I/Os. Returns 0 or -errno: -EAGAIN when the system's AIO
    // contexts already hold aio-max-nr requests between them (every thread
    // has a context of its own, so a process with many threads doing AIO
    // at once can reach it).
    int setup(unsigned n) {
        aio_context_t c = 0;
        if (auto hook = AioIO::setup_test_hook.load(std::memory_order_relaxed))
            if (int rc = hook(); rc != 0) return rc;
        if (sys_io_setup(n, &c) < 0) return -errno;
        tsan_acquire_from_kernel(reinterpret_cast<void*>(c));
        if (ctx != 0) sys_io_destroy(ctx);
        ctx = c;
        depth = n;
        return 0;
    }

    // A thread exits with no I/O of its own in flight (every operation it
    // started has completed); reap any stragglers rather than destroy the
    // context under them.
    ~AioThread() {
        while (pending > 0) AioIO::poll_thread();
        if (ctx != 0) sys_io_destroy(ctx);
    }
};

thread_local AioThread tl_aio;

// No silent failure: a thread that can get no context says so, once (its
// every I/O fails the same way until one is freed), with the remedy.
void report_no_context(int rc, unsigned depth) {
    static thread_local bool reported = false;
    if (reported) return;
    reported = true;
    unsigned long max_nr = 0;
    if (FILE* f = std::fopen("/proc/sys/fs/aio-max-nr", "r")) {
        if (std::fscanf(f, "%lu", &max_nr) != 1) max_nr = 0;
        std::fclose(f);
    }
    std::fprintf(stderr,
                 "udepot: this thread can get no AIO context of %u I/Os (%s): "
                 "every thread doing AIO takes one, and the system's already "
                 "hold fs.aio-max-nr (%lu) between them. Its I/O fails with "
                 "-ENOMEM until one is freed. Raise fs.aio-max-nr, lower "
                 "StoreConfig::queue_depth, or use fewer threads.\n",
                 depth, std::strerror(-rc), max_nr);
}

}  // namespace

std::atomic<unsigned> AioIO::new_ctx_depth_{AioIO::kDefaultQueueDepth};

struct AioSubmitAwaitable {
    AioRequest* req;

    bool await_ready() noexcept { return false; }

    bool await_suspend(std::coroutine_handle<> h) noexcept {
        AioThread& t = tl_aio;
        if (t.ctx == 0) {
            const unsigned depth =
                AioIO::new_ctx_depth_.load(std::memory_order_relaxed);
            int rc = t.setup(depth);
            if (rc != 0) {
                // No context to be had: not a full queue (-EAGAIN), which
                // the thread's own completions would clear.
                req->result = rc == -EAGAIN ? -ENOMEM : rc;
                report_no_context(rc, depth);
                return false;
            }
            set_thread_poll(&AioIO::poll_thread);
        }
        req->handle = h;
        struct iocb* cbs[1] = {&req->cb};
        tsan_release_to_kernel(req);
        int rc = sys_io_submit(t.ctx, 1, cbs);
        if (rc != 1) {
            // EAGAIN: the context is full.
            req->result = (rc < 0) ? -errno : -EIO;
            return false;
        }
        ++t.pending;
        return true;
    }

    ssize_t await_resume() noexcept { return req->result; }
};

AioIO::~AioIO() { close(); }

int AioIO::open(const char* path, size_t size) {
    fd_ = open_direct(path, &direct_);
    if (fd_ < 0) {
        const int err = fd_;
        fd_ = -1;
        return err;
    }

    if (int rc = size_direct_file(fd_, path, size); rc != 0) {
        ::close(fd_);
        fd_ = -1;
        return rc;
    }

    size_ = size;

    // This thread's context, at this store's depth, unless it has I/O in
    // flight (as SpdkIO::open creates this thread's queue pair).
    AioThread& t = tl_aio;
    if (t.ctx == 0 || (t.depth != queue_depth_ && t.pending == 0)) {
        if (int rc = t.setup(queue_depth_); rc != 0) {
            ::close(fd_);
            fd_ = -1;
            // Not "retry": a depth past what aio-max-nr leaves.
            return rc == -EAGAIN ? -EINVAL : rc;
        }
        set_thread_poll(&AioIO::poll_thread);
    }
    new_ctx_depth_.store(queue_depth_, std::memory_order_relaxed);
    return 0;
}

void AioIO::close() {
    // The I/O this thread has in flight completes first; other threads
    // reap their own.
    while (poll_thread()) {}
    if (fd_ >= 0) {
        ::close(fd_);
        fd_ = -1;
    }
}

CoroTask<ssize_t> AioIO::pread(void* buf, size_t count, off_t offset) {
    AioRequest req{};
    std::memset(&req.cb, 0, sizeof(req.cb));
    req.cb.aio_fildes = static_cast<uint32_t>(fd_);
    req.cb.aio_lio_opcode = IOCB_CMD_PREAD;
    req.cb.aio_reqprio = 0;
    req.cb.aio_buf = reinterpret_cast<uint64_t>(buf);
    req.cb.aio_nbytes = count;
    req.cb.aio_offset = offset;
    req.cb.aio_data = reinterpret_cast<uint64_t>(&req);

    ssize_t result = co_await AioSubmitAwaitable{&req};
    co_return result;
}

CoroTask<ssize_t> AioIO::pwrite(const void* buf, size_t count, off_t offset) {
    AioRequest req{};
    std::memset(&req.cb, 0, sizeof(req.cb));
    req.cb.aio_fildes = static_cast<uint32_t>(fd_);
    req.cb.aio_lio_opcode = IOCB_CMD_PWRITE;
    req.cb.aio_reqprio = 0;
    req.cb.aio_buf = reinterpret_cast<uint64_t>(buf);
    req.cb.aio_nbytes = count;
    req.cb.aio_offset = offset;
    req.cb.aio_data = reinterpret_cast<uint64_t>(&req);

    ssize_t result = co_await AioSubmitAwaitable{&req};
    co_return result;
}

IoBuffer AioIO::alloc_buffer(size_t size) {
    size_t aligned = (size + 4095) & ~size_t{4095};
    return IoBuffer::alloc_aligned(aligned, 4096);
}

// As uDepot's TRT AIO poller task, run by the thread itself: the user ring
// when the kernel maps one (release builds, as uDepot), io_getevents
// without waiting otherwise. Events are taken off the ring before any
// coroutine resumes, so one that polls again (it may submit and wait)
// never sees them twice.
bool AioIO::poll_thread() {
    AioThread& t = tl_aio;
    if (t.pending == 0) return false;
    static constexpr int kMaxEvents = 8;
    struct io_event events[kMaxEvents];
    int n = 0;
    if (aio_ring_valid(t.ctx)) {
        n = aio_ring_getevents(t.ctx, kMaxEvents, events);
    } else {
        struct timespec zero = {0, 0};
        n = sys_io_getevents(t.ctx, 0, kMaxEvents, events, &zero);
        if (n < 0) n = 0;  // EINTR: nothing reaped this time
    }
    t.pending -= static_cast<size_t>(n);
    for (int i = 0; i < n; ++i) {
        auto* req = reinterpret_cast<AioRequest*>(events[i].data);
        tsan_acquire_from_kernel(req);
        req->result = events[i].res2 != 0
                          ? -EIO
                          : static_cast<ssize_t>(events[i].res);
        req->handle.resume();
    }
    return tl_aio.pending > 0;
}

}  // namespace udepot

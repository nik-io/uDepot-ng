// Copyright (c) 2024-2026 Nikolas Ioannou
// SPDX-License-Identifier: BSD-3-Clause

#include "udepot/io/aio.h"

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

static inline int sys_io_destroy(aio_context_t ctx) {
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
#ifdef NDEBUG
    return reinterpret_cast<AioRing*>(ctx)->magic == kAioRingMagic;
#else
    (void)ctx;
    return false;
#endif
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
        std::atomic_thread_fence(std::memory_order_acquire);
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

struct AioSubmitAwaitable {
    AioRequest* req;
    aio_context_t ctx;

    bool await_ready() noexcept { return false; }

    bool await_suspend(std::coroutine_handle<> h) noexcept {
        req->handle = h;
        struct iocb* cbs[1] = {&req->cb};
        int rc = sys_io_submit(ctx, 1, cbs);
        if (rc != 1) {
            req->result = (rc < 0) ? -errno : -EIO;
            return false;
        }
        return true;
    }

    ssize_t await_resume() noexcept { return req->result; }
};

AioIO::~AioIO() { close(); }

int AioIO::open(const char* path, size_t size) {
    // Match uDepot: O_DIRECT | O_NOATIME.  Fall back to buffered I/O if the
    // filesystem does not support O_DIRECT (e.g. tmpfs in tests).
    fd_ = ::open(path, O_RDWR | O_CREAT | O_DIRECT | O_NOATIME, 0666);
    if (fd_ < 0 && (errno == EINVAL || errno == ENOTSUP))
        fd_ = ::open(path, O_RDWR | O_CREAT, 0666);
    if (fd_ < 0) return -errno;

    struct stat st;
    if (::fstat(fd_, &st) < 0) {
        int err = errno;
        ::close(fd_);
        fd_ = -1;
        return -err;
    }

    if (static_cast<size_t>(st.st_size) < size) {
        if (::ftruncate(fd_, static_cast<off_t>(size)) < 0) {
            int err = errno;
            ::close(fd_);
            fd_ = -1;
            return -err;
        }
    }

    size_ = size;

    ctx_ = 0;
    if (sys_io_setup(1024, &ctx_) < 0) {
        int err = errno;
        ::close(fd_);
        fd_ = -1;
        return -err;
    }

    running_.store(true, std::memory_order_relaxed);
    poller_ = std::thread(&AioIO::poller_loop, this);
    return 0;
}

void AioIO::close() {
    if (running_.load(std::memory_order_relaxed)) {
        running_.store(false, std::memory_order_release);
        if (poller_.joinable())
            poller_.join();
    }

    if (ctx_ != 0) {
        sys_io_destroy(ctx_);
        ctx_ = 0;
    }

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

    ssize_t result = co_await AioSubmitAwaitable{&req, ctx_};
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

    ssize_t result = co_await AioSubmitAwaitable{&req, ctx_};
    co_return result;
}

IoBuffer AioIO::alloc_buffer(size_t size) {
    size_t aligned = (size + 4095) & ~size_t{4095};
    return IoBuffer::alloc_aligned(aligned, 4096);
}

void AioIO::poller_loop() {
    static constexpr int kMaxEvents = 8;
    struct io_event events[kMaxEvents];

    while (running_.load(std::memory_order_acquire)) {
        int n = 0;
        if (aio_ring_valid(ctx_))
            n = aio_ring_getevents(ctx_, kMaxEvents, events);
        if (n == 0) {
            struct timespec timeout = {0, 100'000'000};
            n = sys_io_getevents(ctx_, 1, kMaxEvents, events, &timeout);
        }
        if (n < 0) {
            if (errno == EINTR) continue;
            break;
        }

        for (int i = 0; i < n; ++i) {
            auto* req = reinterpret_cast<AioRequest*>(events[i].data);
            if (events[i].res2 != 0) {
                req->result = -EIO;
            } else {
                req->result = static_cast<ssize_t>(events[i].res);
            }
            req->handle.resume();
        }
    }

    // Drain remaining events on shutdown.
    for (;;) {
        struct timespec timeout = {0, 0};
        int n = sys_io_getevents(ctx_, 0, kMaxEvents, events, &timeout);
        if (n <= 0) break;
        for (int i = 0; i < n; ++i) {
            auto* req = reinterpret_cast<AioRequest*>(events[i].data);
            if (events[i].res2 != 0) {
                req->result = -EIO;
            } else {
                req->result = static_cast<ssize_t>(events[i].res);
            }
            req->handle.resume();
        }
    }
}

}  // namespace udepot

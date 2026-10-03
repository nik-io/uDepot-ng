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
    AioRequest* next;  // in AioIO's wait queue, or its failed list
};

struct AioSubmitAwaitable {
    AioRequest* req;
    AioIO* io;

    bool await_ready() noexcept { return false; }

    bool await_suspend(std::coroutine_handle<> h) noexcept {
        req->handle = h;
        // Behind waiting requests, if any: they go first.
        if (io->waiting_.load(std::memory_order_seq_cst) == 0) {
            int err = io->submit_one(req);
            if (err == 0) return true;
            if (err != EAGAIN) {
                req->result = -err;
                return false;
            }
        }
        return io->wait_for_room(req);
    }

    ssize_t await_resume() noexcept { return req->result; }
};

int AioIO::submit_one(AioRequest* req) noexcept {
    struct iocb* cbs[1] = {&req->cb};
    // Counted before submitting: the completion can arrive, and be counted
    // down, before io_submit returns.
    pending_.fetch_add(1, std::memory_order_seq_cst);
    tsan_release_to_kernel(req);
    int rc = sys_io_submit(ctx_, 1, cbs);
    if (rc == 1) return 0;
    int err = (rc < 0) ? errno : EIO;
    pending_.fetch_sub(1, std::memory_order_seq_cst);
    return err;
}

// The waiter counts itself in waiting_ and then retries; the poller reaps
// (freeing room) and then looks at waiting_. Both are seq_cst, so whichever
// goes second sees the other: room freed while a request is being queued
// is used by one of them, never by neither.
bool AioIO::wait_for_room(AioRequest* req) {
    AioRequest* failed = nullptr;
    {
        std::lock_guard<std::mutex> lock(wait_mu_);
        req->next = nullptr;
        if (wait_tail_)
            wait_tail_->next = req;
        else
            wait_head_ = req;
        wait_tail_ = req;
        waiting_.fetch_add(1, std::memory_order_seq_cst);
        waited_total_.fetch_add(1, std::memory_order_relaxed);
        submit_waiting_locked(&failed);
    }
    // A request that was submitted may already have completed and been
    // resumed by the poller, so req is compared, not dereferenced, unless
    // it is on the failed list.
    bool req_failed = false;
    while (failed) {
        AioRequest* r = failed;
        failed = r->next;
        if (r == req)
            req_failed = true;
        else
            r->handle.resume();
    }
    return !req_failed;
}

void AioIO::submit_waiting_locked(AioRequest** failed) {
    while (wait_head_) {
        AioRequest* r = wait_head_;
        AioRequest* next = r->next;  // r may complete once submitted
        int err = submit_one(r);
        if (err == EAGAIN) return;  // still full; the poller retries
        wait_head_ = next;
        if (!wait_head_) wait_tail_ = nullptr;
        waiting_.fetch_sub(1, std::memory_order_seq_cst);
        if (err != 0) {
            r->result = -err;
            r->next = *failed;
            *failed = r;
        }
    }
}

void AioIO::submit_waiting() {
    AioRequest* failed = nullptr;
    {
        std::lock_guard<std::mutex> lock(wait_mu_);
        submit_waiting_locked(&failed);
    }
    while (failed) {
        AioRequest* r = failed;
        failed = r->next;
        r->handle.resume();
    }
}

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
    waited_total_.store(0, std::memory_order_relaxed);

    ctx_ = 0;
    if (sys_io_setup(queue_depth_, &ctx_) < 0) {
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

    ssize_t result = co_await AioSubmitAwaitable{&req, this};
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

    ssize_t result = co_await AioSubmitAwaitable{&req, this};
    co_return result;
}

IoBuffer AioIO::alloc_buffer(size_t size) {
    size_t aligned = (size + 4095) & ~size_t{4095};
    return IoBuffer::alloc_aligned(aligned, 4096);
}

void AioIO::poller_loop() {
    static constexpr int kMaxEvents = 8;
    struct io_event events[kMaxEvents];

    // After close() clears running_, keep going until every submitted or
    // waiting I/O has completed; returning earlier would leave those
    // coroutines suspended forever.
    while (running_.load(std::memory_order_acquire) ||
           pending_.load(std::memory_order_acquire) > 0 ||
           waiting_.load(std::memory_order_acquire) > 0) {
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
            tsan_acquire_from_kernel(req);
            if (events[i].res2 != 0) {
                req->result = -EIO;
            } else {
                req->result = static_cast<ssize_t>(events[i].res);
            }
            pending_.fetch_sub(1, std::memory_order_seq_cst);
            req->handle.resume();
        }

        // Also after a timeout, as a backstop.
        if (waiting_.load(std::memory_order_seq_cst) > 0)
            submit_waiting();
    }
}

}  // namespace udepot

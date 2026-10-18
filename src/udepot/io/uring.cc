// Copyright (c) 2024-2026 Nikolas Ioannou
// SPDX-License-Identifier: BSD-3-Clause

#include "udepot/io/uring.h"
#include "udepot/tsan.h"

#include <cerrno>
#include <cstdio>
#include <cstdlib>
#include <coroutine>
#include <cstring>
#include <fcntl.h>
#include <mutex>
#include <thread>
#include <sys/stat.h>
#include <unistd.h>

namespace udepot {

struct UringRequest {
    ssize_t result;
    std::coroutine_handle<> handle;
};

namespace {

// The calling thread's ring, as uDepot's thread_local UringState: only this
// thread submits to it and reaps it, so it needs no synchronization.
struct UringThread {
    struct io_uring ring{};
    bool initialized = false;
    unsigned depth = 0;
    size_t pending = 0;  // in the ring (queued or submitted), not reaped

    // depth I/Os. Returns 0 or -errno.
    int setup(unsigned n) {
        struct io_uring r{};
        if (int rc = io_uring_queue_init(n, &r, 0); rc < 0) return rc;
        handoff(r, tsan_acquire_from_kernel);
        if (initialized) exit_ring();
        ring = r;
        initialized = true;
        depth = n;
        return 0;
    }

    void submit() noexcept;

    // A thread exits with no I/O of its own in flight (every operation it
    // started has completed); reap any stragglers rather than tear the ring
    // down under them.
    ~UringThread() {
        while (pending > 0) UringIO::poll_thread();
        if (initialized) exit_ring();
    }

    // The kernel maps the ring and liburing unmaps it with raw syscalls, so
    // a ring another thread tore down can come back at the same addresses
    // from io_uring_queue_init, its accesses ordered before the new ring's
    // by the kernel, which TSAN cannot see. Annotated on each mapping, as
    // either may be reused without the other.
    static void handoff(struct io_uring& r, void (*edge)(void*) noexcept) {
        edge(r.sq.ring_ptr);
        edge(r.cq.ring_ptr);
        edge(r.sq.sqes);
    }
    void exit_ring() {
        handoff(ring, tsan_release_to_kernel);
        io_uring_queue_exit(&ring);
    }
};

thread_local UringThread tl_uring;

// SQEs queued before they are pushed to the kernel together, as uDepot's
// UringState (iou_ioq_size_): fewer io_uring_enter calls when many I/Os
// are issued at once. The thread's next poll pushes a smaller batch.
constexpr unsigned kSubmitBatch = 128;

}  // namespace

std::atomic<unsigned> UringIO::new_ring_depth_{UringIO::kDefaultQueueDepth};

// liburing has published the SQEs to the ring before io_uring_submit can
// fail, so they cannot be withdrawn. EBUSY/EAGAIN clear as the thread
// reaps, so they are left for its next poll to push again rather than spun
// on here: this may be that poll itself, resuming a coroutine that
// submits. Anything else means the ring itself is unusable.
void UringThread::submit() noexcept {
    while (io_uring_sq_ready(&ring) > 0) {
        int rc = 0;
        if (auto hook = UringIO::submit_test_hook.load(std::memory_order_relaxed))
            rc = hook();
        if (rc == 0) rc = io_uring_submit(&ring);
        if (rc >= 0 || rc == -EINTR) continue;
        if (rc == -EBUSY || rc == -EAGAIN) return;
        fprintf(stderr, "io_uring_submit: %s\n", strerror(-rc));
        abort();
    }
}

struct UringSubmitAwaitable {
    UringRequest* req;
    UringIO* io;
    void* buf;
    size_t count;
    off_t offset;
    bool is_write;

    bool await_ready() noexcept { return false; }

    bool await_suspend(std::coroutine_handle<> h) noexcept {
        UringThread& t = tl_uring;
        if (!t.initialized) {
            int rc = t.setup(UringIO::new_ring_depth_.load(
                std::memory_order_relaxed));
            if (rc != 0) {
                req->result = rc;
                return false;
            }
            set_thread_poll(&UringIO::poll_thread);
        }
        req->handle = h;
        struct io_uring_sqe* sqe = io_uring_get_sqe(&t.ring);
        if (!sqe && io_uring_sq_ready(&t.ring) > 0) {
            // Full of SQEs still waiting for a batch: a ring smaller than
            // kSubmitBatch (uDepot's is never smaller) fills before one.
            // Push them, and refuse only what the kernel refuses.
            t.submit();
            sqe = io_uring_get_sqe(&t.ring);
        }
        if (!sqe) {  // full of SQEs the kernel refused for now
            req->result = -EAGAIN;
            return false;
        }
        if (is_write)
            io_uring_prep_write(sqe, io->fd_, buf,
                                static_cast<unsigned>(count), offset);
        else
            io_uring_prep_read(sqe, io->fd_, buf,
                               static_cast<unsigned>(count), offset);
        io_uring_sqe_set_data(sqe, req);
        ++t.pending;
        tsan_release_to_kernel(req);

        // From here the SQE belongs to the ring and will reach the kernel,
        // which completes it into req; so this must not resume the caller
        // with an error, or req and buf would be freed under it. It goes
        // with a full batch, or at the thread's next poll.
        if (io_uring_sq_ready(&t.ring) >= kSubmitBatch) t.submit();
        return true;
    }

    ssize_t await_resume() noexcept { return req->result; }
};

UringIO::~UringIO() { close(); }

int UringIO::open(const char* path, size_t size) {
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

    // This thread's ring, at this store's depth, unless it has I/O in
    // flight (as SpdkIO::open creates this thread's queue pair).
    UringThread& t = tl_uring;
    if (!t.initialized || (t.depth != queue_depth_ && t.pending == 0)) {
        if (int rc = t.setup(queue_depth_); rc != 0) {
            ::close(fd_);
            fd_ = -1;
            return rc;
        }
        set_thread_poll(&UringIO::poll_thread);
    }
    new_ring_depth_.store(queue_depth_, std::memory_order_relaxed);
    return 0;
}

void UringIO::close() {
    // The I/O this thread has in flight completes first; other threads
    // reap their own.
    while (poll_thread()) {}
    if (fd_ >= 0) {
        ::close(fd_);
        fd_ = -1;
    }
}

CoroTask<ssize_t> UringIO::pread(void* buf, size_t count, off_t offset) {
    UringRequest req{};
    ssize_t result = co_await UringSubmitAwaitable{&req, this, buf, count,
                                                   offset, false};
    co_return result;
}

CoroTask<ssize_t> UringIO::pwrite(const void* buf, size_t count, off_t offset) {
    UringRequest req{};
    ssize_t result = co_await UringSubmitAwaitable{
        &req, this, const_cast<void*>(buf), count, offset, true};
    co_return result;
}

IoBuffer UringIO::alloc_buffer(size_t size) {
    size_t aligned = (size + 4095) & ~size_t{4095};
    return IoBuffer::alloc_aligned(aligned, 4096);
}

// As uDepot's TRT io_uring poller task, run by the thread itself. The
// completions are taken off the ring before any coroutine resumes, so one
// that polls again (it may submit and wait) never sees them twice.
bool UringIO::poll_thread() {
    UringThread& t = tl_uring;
    if (t.pending == 0) return false;
    // Push what is queued (and what the kernel refused before), as uDepot's
    // poller flushes before it peeks.
    if (io_uring_sq_ready(&t.ring) > 0) t.submit();
    static constexpr unsigned kMaxCqes = 64;
    struct io_uring_cqe* cqes[kMaxCqes];
    const unsigned n = io_uring_peek_batch_cqe(&t.ring, cqes, kMaxCqes);
    // Nothing done: give up the CPU. The io-wq worker running a request the
    // kernel could not do inline is woken on this thread's CPU, and spinning
    // here holds it off until preemption (docs/architecture.md).
    if (n == 0) {
        std::this_thread::yield();
        return true;
    }
    UringRequest* done[kMaxCqes];
    for (unsigned i = 0; i < n; ++i) {
        done[i] = static_cast<UringRequest*>(io_uring_cqe_get_data(cqes[i]));
        tsan_acquire_from_kernel(done[i]);
        done[i]->result = cqes[i]->res;
    }
    io_uring_cq_advance(&t.ring, n);
    t.pending -= n;
    for (unsigned i = 0; i < n; ++i) done[i]->handle.resume();
    return tl_uring.pending > 0;
}

}  // namespace udepot

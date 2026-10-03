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
    void* buf;
    size_t count;
    off_t offset;
    bool is_write;
    UringRequest* next;  // in UringIO's wait queue
};

struct UringSubmitAwaitable {
    UringRequest* req;
    UringIO* io;

    bool await_ready() noexcept { return false; }

    // Never resumes the caller with an error: once an SQE is in the ring it
    // will reach the kernel, which completes it into req, so req and its
    // buffer must stay live until the poller resumes the caller.
    bool await_suspend(std::coroutine_handle<> h) noexcept {
        req->handle = h;
        std::lock_guard<std::mutex> lock(io->sq_mutex_);
        // Behind waiting requests, if any: they go first.
        if (!io->wait_head_ && io->queue_locked(req)) {
            io->submit_locked();
            return true;
        }
        req->next = nullptr;
        if (io->wait_tail_)
            io->wait_tail_->next = req;
        else
            io->wait_head_ = req;
        io->wait_tail_ = req;
        io->waiting_.fetch_add(1, std::memory_order_seq_cst);
        io->waited_total_.fetch_add(1, std::memory_order_relaxed);
        io->submit_waiting_locked();
        return true;
    }

    ssize_t await_resume() noexcept { return req->result; }
};

bool UringIO::queue_locked(UringRequest* req) noexcept {
    struct io_uring_sqe* sqe = io_uring_get_sqe(&ring_);
    if (!sqe) return false;  // full of SQEs the kernel refused for now
    if (req->is_write)
        io_uring_prep_write(sqe, fd_, req->buf,
                            static_cast<unsigned>(req->count), req->offset);
    else
        io_uring_prep_read(sqe, fd_, req->buf,
                           static_cast<unsigned>(req->count), req->offset);
    io_uring_sqe_set_data(sqe, req);
    pending_.fetch_add(1, std::memory_order_seq_cst);
    tsan_release_to_kernel(req);
    return true;
}

// liburing has published the SQEs to the ring before io_uring_submit can
// fail, so they cannot be withdrawn. EBUSY/EAGAIN clear as the poller
// reaps, so they are left for it to push again rather than spun on here:
// this may be the poller itself, resuming a coroutine that submits.
// Anything else means the ring itself is unusable.
void UringIO::submit_locked() noexcept {
    while (io_uring_sq_ready(&ring_) > 0) {
        int rc = io_uring_submit(&ring_);
        if (rc >= 0 || rc == -EINTR) continue;
        if (rc == -EBUSY || rc == -EAGAIN) {
            sq_backlog_.store(true, std::memory_order_seq_cst);
            return;
        }
        fprintf(stderr, "io_uring_submit: %s\n", strerror(-rc));
        abort();
    }
    sq_backlog_.store(false, std::memory_order_seq_cst);
}

// The waiter counts itself in waiting_ before trying for room; the poller
// reaps and then looks at waiting_ and sq_backlog_. Both are seq_cst, so
// whichever goes second sees the other.
void UringIO::submit_waiting_locked() noexcept {
    for (;;) {
        bool moved = false;
        while (wait_head_ && queue_locked(wait_head_)) {
            // Not touched again once submitted: it may complete.
            wait_head_ = wait_head_->next;
            if (!wait_head_) wait_tail_ = nullptr;
            waiting_.fetch_sub(1, std::memory_order_seq_cst);
            moved = true;
        }
        submit_locked();
        // Stopped on a full SQ that submitting has just emptied: go again.
        if (!moved || !wait_head_ ||
            sq_backlog_.load(std::memory_order_relaxed))
            return;
    }
}

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
    waited_total_.store(0, std::memory_order_relaxed);

    int rc = io_uring_queue_init(queue_depth_, &ring_, 0);
    if (rc < 0) {
        ::close(fd_);
        fd_ = -1;
        return rc;
    }
    ring_initialized_ = true;

    running_.store(true, std::memory_order_relaxed);
    poller_ = std::thread(&UringIO::poller_loop, this);
    return 0;
}

void UringIO::close() {
    if (running_.load(std::memory_order_relaxed)) {
        running_.store(false, std::memory_order_release);
        if (poller_.joinable())
            poller_.join();
    }

    if (ring_initialized_) {
        io_uring_queue_exit(&ring_);
        ring_initialized_ = false;
    }

    if (fd_ >= 0) {
        ::close(fd_);
        fd_ = -1;
    }
}

CoroTask<ssize_t> UringIO::pread(void* buf, size_t count, off_t offset) {
    UringRequest req{0, {}, buf, count, offset, false, nullptr};
    ssize_t result = co_await UringSubmitAwaitable{&req, this};
    co_return result;
}

CoroTask<ssize_t> UringIO::pwrite(const void* buf, size_t count, off_t offset) {
    UringRequest req{0, {}, const_cast<void*>(buf), count, offset, true,
                     nullptr};
    ssize_t result = co_await UringSubmitAwaitable{&req, this};
    co_return result;
}

IoBuffer UringIO::alloc_buffer(size_t size) {
    size_t aligned = (size + 4095) & ~size_t{4095};
    return IoBuffer::alloc_aligned(aligned, 4096);
}

void UringIO::poller_loop() {
    static constexpr int kMaxCqes = 64;
    struct io_uring_cqe* cqes[kMaxCqes];

    // After close() clears running_, keep going until every submitted or
    // waiting I/O has completed; returning earlier would leave those
    // coroutines suspended forever.
    while (running_.load(std::memory_order_acquire) ||
           pending_.load(std::memory_order_acquire) > 0 ||
           waiting_.load(std::memory_order_acquire) > 0) {
        int n = io_uring_peek_batch_cqe(&ring_, cqes, kMaxCqes);
        if (n == 0) {
            if (pending_.load(std::memory_order_acquire) > 0) {
                struct io_uring_cqe* cqe;
                struct __kernel_timespec ts = {0, 10'000'000};
                io_uring_wait_cqe_timeout(&ring_, &cqe, &ts);
            } else {
                std::this_thread::yield();
            }
        }

        for (int i = 0; i < n; ++i) {
            auto* req = static_cast<UringRequest*>(
                io_uring_cqe_get_data(cqes[i]));
            tsan_acquire_from_kernel(req);
            req->result = cqes[i]->res;
            io_uring_cqe_seen(&ring_, cqes[i]);
            pending_.fetch_sub(1, std::memory_order_seq_cst);
            req->handle.resume();
        }

        if (waiting_.load(std::memory_order_seq_cst) > 0 ||
            sq_backlog_.load(std::memory_order_seq_cst)) {
            std::lock_guard<std::mutex> lock(sq_mutex_);
            submit_waiting_locked();
        }
    }
}

}  // namespace udepot

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

struct UringSubmitAwaitable {
    UringRequest* req;
    UringIO* io;
    void* buf;
    size_t count;
    off_t offset;
    bool is_write;

    bool await_ready() noexcept { return false; }

    bool await_suspend(std::coroutine_handle<> h) noexcept {
        req->handle = h;
        std::lock_guard<std::mutex> lock(io->sq_mutex_);
        struct io_uring_sqe* sqe = io_uring_get_sqe(&io->ring_);
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
        io->pending_.fetch_add(1, std::memory_order_release);
        tsan_release_to_kernel(req);

        // From here the SQE belongs to the ring and will reach the kernel,
        // which completes it into req; so this must not resume the caller
        // with an error, or req and buf would be freed under it.
        io->submit_locked();
        return true;
    }

    ssize_t await_resume() noexcept { return req->result; }
};

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
            sq_backlog_.store(true, std::memory_order_release);
            return;
        }
        fprintf(stderr, "io_uring_submit: %s\n", strerror(-rc));
        abort();
    }
    sq_backlog_.store(false, std::memory_order_release);
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

void UringIO::poller_loop() {
    static constexpr int kMaxCqes = 64;
    struct io_uring_cqe* cqes[kMaxCqes];

    // After close() clears running_, keep going until every submitted I/O
    // has completed; returning earlier would leave those coroutines
    // suspended forever.
    while (running_.load(std::memory_order_acquire) ||
           pending_.load(std::memory_order_acquire) > 0) {
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
            pending_.fetch_sub(1, std::memory_order_relaxed);
            req->handle.resume();
        }

        // SQEs the kernel refused: push them again now that it has reaped.
        if (sq_backlog_.load(std::memory_order_acquire)) {
            std::lock_guard<std::mutex> lock(sq_mutex_);
            submit_locked();
        }
    }
}

}  // namespace udepot

// Copyright (c) 2024-2026 Nikolas Ioannou
// SPDX-License-Identifier: BSD-3-Clause

#include "udepot/io/uring.h"

#include <cerrno>
#include <coroutine>
#include <cstring>
#include <fcntl.h>
#include <mutex>
#include <sys/stat.h>
#include <unistd.h>

namespace udepot {

static constexpr unsigned kRingSize = 1024;

struct UringRequest {
    ssize_t result;
    std::coroutine_handle<> handle;
};

struct UringSubmitAwaitable {
    UringRequest* req;
    struct io_uring* ring;
    std::mutex* sq_mutex;
    int fd;
    void* buf;
    size_t count;
    off_t offset;
    bool is_write;
    std::atomic<size_t>* pending;

    bool await_ready() noexcept { return false; }

    bool await_suspend(std::coroutine_handle<> h) noexcept {
        req->handle = h;

        std::lock_guard<std::mutex> lock(*sq_mutex);

        struct io_uring_sqe* sqe = io_uring_get_sqe(ring);
        if (!sqe) {
            req->result = -EBUSY;
            return false;
        }

        if (is_write)
            io_uring_prep_write(sqe, fd, buf, static_cast<unsigned>(count), offset);
        else
            io_uring_prep_read(sqe, fd, buf, static_cast<unsigned>(count), offset);

        io_uring_sqe_set_data(sqe, req);
        pending->fetch_add(1, std::memory_order_release);

        int rc = io_uring_submit(ring);
        if (rc < 0) {
            pending->fetch_sub(1, std::memory_order_relaxed);
            req->result = rc;
            return false;
        }
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

    int rc = io_uring_queue_init(kRingSize, &ring_, 0);
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
    ssize_t result = co_await UringSubmitAwaitable{
        &req, &ring_, &sq_mutex_, fd_, buf, count, offset, false, &pending_};
    co_return result;
}

CoroTask<ssize_t> UringIO::pwrite(const void* buf, size_t count, off_t offset) {
    UringRequest req{};
    ssize_t result = co_await UringSubmitAwaitable{
        &req, &ring_, &sq_mutex_, fd_, const_cast<void*>(buf), count, offset,
        true, &pending_};
    co_return result;
}

IoBuffer UringIO::alloc_buffer(size_t size) {
    size_t aligned = (size + 4095) & ~size_t{4095};
    return IoBuffer::alloc_aligned(aligned, 4096);
}

void UringIO::poller_loop() {
    static constexpr int kMaxCqes = 64;
    struct io_uring_cqe* cqes[kMaxCqes];

    while (running_.load(std::memory_order_acquire)) {
        int n = io_uring_peek_batch_cqe(&ring_, cqes, kMaxCqes);
        if (n == 0) {
            if (pending_.load(std::memory_order_acquire) > 0) {
                struct io_uring_cqe* cqe;
                struct __kernel_timespec ts = {0, 10'000'000};
                io_uring_wait_cqe_timeout(&ring_, &cqe, &ts);
            } else {
                std::this_thread::yield();
            }
            continue;
        }

        for (int i = 0; i < n; ++i) {
            auto* req = static_cast<UringRequest*>(
                io_uring_cqe_get_data(cqes[i]));
            req->result = cqes[i]->res;
            io_uring_cqe_seen(&ring_, cqes[i]);
            pending_.fetch_sub(1, std::memory_order_relaxed);
            req->handle.resume();
        }
    }

    // Drain remaining completions on shutdown.
    for (;;) {
        int n = io_uring_peek_batch_cqe(&ring_, cqes, kMaxCqes);
        if (n <= 0) break;
        for (int i = 0; i < n; ++i) {
            auto* req = static_cast<UringRequest*>(
                io_uring_cqe_get_data(cqes[i]));
            req->result = cqes[i]->res;
            io_uring_cqe_seen(&ring_, cqes[i]);
            pending_.fetch_sub(1, std::memory_order_relaxed);
            req->handle.resume();
        }
    }
}

}  // namespace udepot

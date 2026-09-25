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

namespace udepot {

class UringIO {
public:
    UringIO() noexcept = default;
    ~UringIO();

    UringIO(const UringIO&) = delete;
    UringIO& operator=(const UringIO&) = delete;

    int open(const char* path, size_t size);
    void close();

    CoroTask<ssize_t> pread(void* buf, size_t count, off_t offset);
    CoroTask<ssize_t> pwrite(const void* buf, size_t count, off_t offset);

    size_t get_size() const noexcept { return size_; }
    IoBuffer alloc_buffer(size_t size);

private:
    int fd_ = -1;
    size_t size_ = 0;
    struct io_uring ring_{};
    bool ring_initialized_ = false;
    std::mutex sq_mutex_;
    std::thread poller_;
    std::atomic<bool> running_{false};
    std::atomic<size_t> pending_{0};

    void poller_loop();
};

}  // namespace udepot

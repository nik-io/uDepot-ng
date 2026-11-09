// Copyright (c) 2024-2026 Nikolas Ioannou
// SPDX-License-Identifier: BSD-3-Clause

#pragma once

#include <cerrno>
#include <cstddef>
#include <string>
#include <fcntl.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <unistd.h>

#include "udepot/buffer.h"
#include "udepot/coro.h"
#include "udepot/io/fd_io.h"

namespace udepot {

// Synchronous buffered POSIX I/O backend.
// All operations complete eagerly — the coroutine never suspends.
class PosixIO {
public:
    PosixIO() noexcept = default;
    ~PosixIO() { close(); }

    PosixIO(const PosixIO&) = delete;
    PosixIO& operator=(const PosixIO&) = delete;

    int open(const char* path, size_t size) {
        int flags = O_RDWR | O_CREAT;
        fd_ = ::open(path, flags, 0666);
        if (fd_ < 0) return -errno;

        size_t used = 0;
        bool fresh = false;
        if (int rc = size_store_fd(fd_, path, size, false, force_destroy_, &used, &fresh); rc != 0) {
            ::close(fd_);
            fd_ = -1;
            return rc;
        }

        size_ = used;
        fresh_ = fresh;
        path_ = path;
        return 0;
    }

    void close() {
        if (fd_ >= 0) {
            ::close(fd_);
            fd_ = -1;
        }
    }

    CoroTask<ssize_t> pread(void* buf, size_t count, off_t offset) {
        ssize_t ret = ::pread(fd_, buf, count, offset);
        co_return ret < 0 ? -errno : ret;
    }

    CoroTask<ssize_t> pwrite(const void* buf, size_t count, off_t offset) {
        ssize_t ret = ::pwrite(fd_, buf, count, offset);
        co_return ret < 0 ? -errno : ret;
    }

    ssize_t pwrite_sync(const void* buf, size_t count, off_t offset) {
        return pwrite_full_fd(fd_, buf, count, offset);
    }

    size_t get_size() const noexcept { return size_; }
    // Whether the last open() found a new or empty regular file: no store
    // on it to restore (see size_store_fd).
    bool fresh() const noexcept { return fresh_; }
    // Whether a file with data may be grown to a size above its length
    // (StoreConfig::force_destroy; true for a raw backend, which has no
    // store to protect: store.cc sets it from its config before open()).
    void set_force_destroy(bool v) noexcept { force_destroy_ = v; }
    // After a failed first open: a new file the open took to its size goes
    // back to empty (see truncate_fresh_file). Not for a device.
    void discard_fresh() noexcept {
        if (fresh_ && fd_ >= 0) truncate_fresh_file(fd_, path_.c_str());
        fresh_ = false;
    }

    IoBuffer alloc_buffer(size_t size) {
        return IoBuffer::alloc_malloc(size);
    }

    int fd() const noexcept { return fd_; }

private:
    int fd_ = -1;
    size_t size_ = 0;
    bool fresh_ = false;
    bool force_destroy_ = true;
    std::string path_;
};

}  // namespace udepot

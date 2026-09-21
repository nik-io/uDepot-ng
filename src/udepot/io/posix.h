#pragma once

#include <cerrno>
#include <cstddef>
#include <fcntl.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <unistd.h>

#include "udepot/buffer.h"
#include "udepot/coro.h"

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

    size_t get_size() const noexcept { return size_; }

    IoBuffer alloc_buffer(size_t size) {
        return IoBuffer::alloc_malloc(size);
    }

    int fd() const noexcept { return fd_; }

private:
    int fd_ = -1;
    size_t size_ = 0;
};

}  // namespace udepot

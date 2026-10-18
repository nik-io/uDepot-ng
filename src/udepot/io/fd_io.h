// Copyright (c) 2024-2026 Nikolas Ioannou
// SPDX-License-Identifier: BSD-3-Clause

#pragma once

#include <atomic>
#include <cerrno>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <fcntl.h>
#include <sys/types.h>
#include <unistd.h>

namespace udepot {

// Blocking pwrite of the whole buffer on an fd, retrying partial writes and
// EINTR. Returns count or -errno. For the fd-based backends' pwrite_sync().
inline ssize_t pwrite_full_fd(int fd, const void* buf, size_t count,
                              off_t offset) {
    auto* p = static_cast<const uint8_t*>(buf);
    size_t done = 0;
    while (done < count) {
        ssize_t n = ::pwrite(fd, p + done, count - done,
                             offset + static_cast<off_t>(done));
        if (n < 0) {
            if (errno == EINTR) continue;
            return -errno;
        }
        if (n == 0) return -EIO;
        done += static_cast<size_t>(n);
    }
    return static_cast<ssize_t>(count);
}

// Test seam, never set in production: open_direct() takes the filesystem
// as refusing O_DIRECT, as tmpfs before Linux 6.6 does.
inline std::atomic<bool> refuse_direct_test{false};

// Opens path for the O_DIRECT backends (AIO, io_uring), as uDepot does:
// O_DIRECT | O_NOATIME. A filesystem that refuses O_DIRECT gets buffered
// I/O, never silently: every value then goes through the page cache, a
// copy each way, so there is no zero copy. *direct says which it got.
// Returns the fd or -errno.
inline int open_direct(const char* path, bool* direct) {
    int fd = -1;
    if (refuse_direct_test.load(std::memory_order_relaxed))
        errno = EINVAL;
    else
        fd = ::open(path, O_RDWR | O_CREAT | O_DIRECT | O_NOATIME, 0666);
    if (fd >= 0) {
        *direct = true;
        return fd;
    }
    if (errno != EINVAL && errno != ENOTSUP) return -errno;
    const int refused = errno;
    fd = ::open(path, O_RDWR | O_CREAT, 0666);
    if (fd < 0) return -errno;
    *direct = false;
    std::fprintf(stderr,
                 "udepot: %s does not take O_DIRECT (%s); using buffered "
                 "I/O, which copies every value through the page cache: no "
                 "zero copy, a copy each way on every read and write.\n",
                 path, std::strerror(refused));
    return fd;
}

}  // namespace udepot

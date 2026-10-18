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
#include <sys/stat.h>
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

// Test seam, never set in production: size_direct_file() takes the
// filesystem as unable to preallocate (EOPNOTSUPP).
inline std::atomic<bool> refuse_fallocate_test{false};

// Sizes the O_DIRECT backends' store to `size` bytes. A regular file is
// preallocated (fallocate; uDepot ftruncates it, leaving it sparse), so no
// write has to allocate blocks: an O_DIRECT write into a hole cannot be
// done without blocking (io_uring hands it to an io-wq worker), and a
// store too large for the filesystem fails here with -ENOSPC rather than
// on some write later. Data already there is kept. A filesystem that
// cannot preallocate gets a sparse file, and the open says so. Anything
// else (a block device) is sized as before. Returns 0 or -errno.
inline int size_direct_file(int fd, const char* path, size_t size) {
    struct stat st;
    if (::fstat(fd, &st) < 0) return -errno;
    if (S_ISREG(st.st_mode)) {
        int rc = refuse_fallocate_test.load(std::memory_order_relaxed)
                     ? (errno = EOPNOTSUPP, -1)
                     : ::fallocate(fd, 0, 0, static_cast<off_t>(size));
        if (rc == 0) return 0;
        if (errno != EOPNOTSUPP) return -errno;
        std::fprintf(stderr,
                     "udepot: %s cannot be preallocated (%s); the store is a "
                     "sparse file, and a write to a block not yet written "
                     "allocates it: O_DIRECT cannot do that without blocking "
                     "(io_uring hands it to a kernel worker).\n",
                     path, std::strerror(EOPNOTSUPP));
    }
    if (static_cast<size_t>(st.st_size) < size &&
        ::ftruncate(fd, static_cast<off_t>(size)) < 0)
        return -errno;
    return 0;
}

}  // namespace udepot

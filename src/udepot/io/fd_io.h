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
#include <sys/ioctl.h>
#include <sys/mount.h>  // BLKGETSIZE64 (not <linux/fs.h>: it clashes)
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

// Sizes the store when fd is a block device, which is never truncated or
// preallocated (ftruncate fails on it, and its size is not ours to change).
// Its size is read with BLKGETSIZE64 (uDepot's register_local_region,
// udepot-lsa.cc, takes it from lseek(SEEK_END) and always uses the whole
// device; here, as the SPDK backend's `size > 0 ? size : namespace size`,
// a size asked for is used and 0 means the whole device). A size larger
// than the device fails with -ENOSPC, and says so; so does a device of 0
// bytes (an unbound loop device opens fine).
// Returns 0 if fd is not a block device (the caller sizes it as before,
// *used untouched), 1 if it is and *used is the size to use, or -errno.
// The caller closes fd on a negative return.
inline int size_block_device(int fd, const char* path, size_t size,
                             size_t* used) {
    struct stat st;
    if (::fstat(fd, &st) < 0) return -errno;
    if (!S_ISBLK(st.st_mode)) return 0;
    uint64_t bytes = 0;
    if (::ioctl(fd, BLKGETSIZE64, &bytes) < 0) return -errno;
    if (bytes == 0) {
        std::fprintf(stderr,
                     "udepot: block device %s has no capacity (0 bytes), "
                     "e.g. a loop device with nothing bound to it; there is "
                     "nothing to store on.\n",
                     path);
        return -ENOSPC;
    }
    if (size > bytes) {
        std::fprintf(stderr,
                     "udepot: block device %s is %llu bytes, smaller than "
                     "the %zu asked for; ask for at most %llu bytes, or 0 "
                     "for the whole device.\n",
                     path, static_cast<unsigned long long>(bytes), size,
                     static_cast<unsigned long long>(bytes));
        return -ENOSPC;
    }
    *used = size > 0 ? size : static_cast<size_t>(bytes);
    return 1;
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
// else is sized as before (a block device is not sized here at all: see
// size_block_device and size_store_fd, which the backends call). `size`
// must be above 0 (fallocate of 0 bytes fails): size_store_fd resolves a
// size of 0 to the file's length first. Returns 0 or -errno.
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

// Sizes the store on fd for the backends that open a path (posix, AIO,
// io_uring), setting *used to the size the store gets. Size 0 means the
// whole thing wherever there is one, as uDepot's register_local_region
// (udepot-lsa.cc:96-119), which takes the store size from lseek(SEEK_END):
//  - a block device: never resized, see size_block_device.
//  - a regular file with data: its whole length. A new or empty file has no
//    size to be the whole of: -EINVAL, with a message. The file is left
//    (uDepot creates it as well).
//  - anything else: sized as before.
// A size above 0 on a regular file is the size asked for: grown to it if
// smaller, and a larger file is used up to it.
// `preallocate` is the O_DIRECT backends' (size_direct_file, which keeps
// the data and fills any holes, so the store is never silently sparse);
// posix only grows the file with ftruncate. Returns 0 or -errno; the caller
// closes fd on a negative return. *fresh says whether fd is a regular file
// that was new or empty before this call: there is no store on it to find,
// which the store's open() needs to tell from a device or a file holding
// something that is not the store it was asked for (never fresh for a block
// device). force_destroy is the store's: whether a file with data may be
// grown to a size it was not created with.
inline int size_store_fd(int fd, const char* path, size_t size,
                         bool preallocate, bool force_destroy, size_t* used,
                         bool* fresh) {
    *fresh = false;
    const int blk = size_block_device(fd, path, size, used);
    if (blk < 0) return blk;
    if (blk > 0) return 0;
    struct stat st;
    if (::fstat(fd, &st) < 0) return -errno;
    *used = size;
    *fresh = S_ISREG(st.st_mode) && st.st_size == 0;
    // A store is created on a file of at least its size, so a file with
    // data that is shorter holds none of that size. Refuse before growing
    // it: the store's open refuses an unknown file, and a refused open must
    // leave it as it was.
    if (S_ISREG(st.st_mode) && !*fresh && !force_destroy && size > 0 &&
        static_cast<size_t>(st.st_size) < size) {
        std::fprintf(stderr,
                     "udepot: a store of %zu bytes cannot be on %s, which "
                     "is only %lld bytes and not a new or empty file: "
                     "reopen it with the size the store was created with, "
                     "or 0 for the whole file, or set "
                     "StoreConfig::force_destroy to start a new store, "
                     "which destroys it.\n",
                     size, path, static_cast<long long>(st.st_size));
        return -EEXIST;
    }
    if (size == 0 && S_ISREG(st.st_mode)) {
        if (st.st_size == 0) {
            std::fprintf(stderr,
                         "udepot: %s is a new or empty file, and size 0 asks "
                         "for its whole size, which it does not have; give "
                         "the store a size.\n",
                         path);
            return -EINVAL;
        }
        *used = static_cast<size_t>(st.st_size);
    }
    if (preallocate) return size_direct_file(fd, path, *used);
    if (static_cast<size_t>(st.st_size) < *used &&
        ::ftruncate(fd, static_cast<off_t>(*used)) < 0)
        return -errno;
    return 0;
}

// A first open that fails before the store's metadata is written took a new
// file to its size (preallocated, zero-filled): back to empty, so that a
// later open finds a new file again and not one that may hold a store.
inline void truncate_fresh_file(int fd, const char* path) {
    if (fd >= 0 && ::ftruncate(fd, 0) < 0)
        std::fprintf(stderr,
                     "udepot: could not truncate %s back to empty after the "
                     "open failed (%s); it is no longer a new file, so "
                     "opening it again needs StoreConfig::force_destroy.\n",
                     path, std::strerror(errno));
}

}  // namespace udepot

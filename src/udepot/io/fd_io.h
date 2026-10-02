// Copyright (c) 2024-2026 Nikolas Ioannou
// SPDX-License-Identifier: BSD-3-Clause

#pragma once

#include <cerrno>
#include <cstddef>
#include <cstdint>
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

}  // namespace udepot

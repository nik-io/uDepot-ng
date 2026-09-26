// Copyright (c) 2024-2026 Nikolas Ioannou
// SPDX-License-Identifier: BSD-3-Clause

#pragma once

#include <concepts>
#include <cstddef>
#include <sys/types.h>

#include "udepot/buffer.h"
#include "udepot/coro.h"

namespace udepot {

// Every I/O backend must satisfy this concept.
template <typename T>
concept IoBackend = requires(T io, void* buf, const void* cbuf, size_t n,
                             off_t off, const char* path, size_t size) {
    { io.open(path, size) } -> std::same_as<int>;
    { io.close() } -> std::same_as<void>;
    { io.pread(buf, n, off) } -> std::same_as<CoroTask<ssize_t>>;
    { io.pwrite(cbuf, n, off) } -> std::same_as<CoroTask<ssize_t>>;
    { io.get_size() } -> std::same_as<size_t>;
    { io.alloc_buffer(n) } -> std::same_as<IoBuffer>;
};

}  // namespace udepot

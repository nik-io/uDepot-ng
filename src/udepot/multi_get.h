// Copyright (c) 2024-2026 Nikolas Ioannou
// SPDX-License-Identifier: BSD-3-Clause

#pragma once

#include <algorithm>
#include <cerrno>
#include <cstdint>
#include <memory>
#include <new>
#include <optional>
#include <span>
#include <thread>

#include "udepot/store.h"

namespace udepot {

// Gets keys[i] into bufs[i] for i in [0, n) on the calling thread, with at
// most `window` gets in flight: one thread, no locks. Each get is an
// eager-start coroutine that submits its I/O and suspends; as one completes
// its slot is refilled, and poll_this_thread() reaps the completions. This
// is the slot pattern of test/perf/udepot_ng_bench.cc's op_window() (its
// --qd), which keeps up to qd operations in flight as legacy udepot-test
// keeps its --trt-ntasks TRT tasks busy.
//
// `window` must not exceed what the calling thread's queue holds: a get
// past it fails with -EAGAIN, which is reported in results[i] like any
// other failure, not retried. A get takes one queue entry at a time. On AIO
// that is AioIO::thread_queue_room(), which follows the thread's context,
// not the store's StoreConfig::queue_depth (a context is shared by every
// store on the thread). 0 is taken as 1. On PosixIO each get completes as
// it starts, so the batch degrades to serial gets.
//
// results[i] is the rc of get(keys[i], bufs[i]): 0, or -errno (a missing key
// is -ENOENT). Every results[i] is written before this returns; one is
// written as soon as its get completes. Returns 0, or -ENOMEM (nothing
// started, results untouched) if the window's slots cannot be allocated.
//
// Each bufs[i] must be a distinct GetBuffer: a buffer serves one get at a
// time. Keys and buffers must stay alive until this returns.
template <typename IO>
int multi_get(uDepot<IO>& store, uint32_t n, const uint8_t* const keys[],
              const uint32_t key_sizes[], GetBuffer* const bufs[],
              int results[], unsigned window) {
    if (n == 0) return 0;
    struct Slot {
        std::optional<CoroTask<int>> task;
        uint32_t i = 0;
    };
    const uint32_t nslots = std::min<uint32_t>(n, std::max(window, 1u));
    std::unique_ptr<Slot[]> slots(new (std::nothrow) Slot[nslots]);
    if (!slots) return -ENOMEM;

    uint32_t next = 0, done = 0;
    while (done < n) {
        for (uint32_t k = 0; k < nslots; ++k) {
            Slot& s = slots[k];
            // Reap first, so the slot it frees is refilled in this pass.
            if (s.task && s.task->await_ready()) {
                results[s.i] = s.task->run_sync();
                s.task.reset();
                ++done;
            }
            if (!s.task && next < n) {
                s.i = next++;
                s.task.emplace(store.get(
                    std::span<const uint8_t>(keys[s.i], key_sizes[s.i]),
                    bufs[s.i]));
            }
        }
        // As run_sync(): nothing of this thread's pending means the rest
        // are waiting on another thread's work; let it run.
        if (done < n && !poll_this_thread()) std::this_thread::yield();
    }
    return 0;
}

}  // namespace udepot

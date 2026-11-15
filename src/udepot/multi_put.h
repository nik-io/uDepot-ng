// Copyright (c) 2024-2026 Nikolas Ioannou
// SPDX-License-Identifier: BSD-3-Clause

#pragma once

#include <algorithm>
#include <cerrno>
#include <cstdint>
#include <cstring>
#include <memory>
#include <new>
#include <optional>
#include <span>
#include <thread>

#include "udepot/store.h"

namespace udepot {

// Puts keys[i] from bufs[i] for i in [0, n) on the calling thread, with at
// most `window` puts in flight: multi_get's pattern (multi_get.h) for the
// write side. One thread, no locks. Each put is an eager-start coroutine that
// submits its write and suspends; as one completes its slot is refilled, and
// poll_this_thread() reaps the completions.
//
// Legacy uDepot has no batched put: its memcache front end batches gets
// (cmd::handle_multiget, src/uDepot/net/mc-helpers.cc:245) but serves each
// set with one kv.put (mc-helpers.cc:533), and no KV interface has a batch
// put. This is the same call as put(key, PutBuffer&), n times, and the
// zero-copy rule is that put's: each write is issued from the PutBuffer, no
// value is copied.
//
// `window` must not exceed what the calling thread's queue holds: a put past
// it fails with -EAGAIN, reported in results[i] like any other failure, not
// retried. A put holds one queue entry at a time (its record's write, then
// its key's verify read). On AIO that room is AioIO::thread_queue_room();
// see multi_get.h. 0 is taken as 1. On PosixIO each put completes as it
// starts, so the batch degrades to serial puts. The window bounds this
// thread's queue only: a put that waits for space (allocate_or_wait, or a
// table grow) is resumed on the store's space-waker thread, and the I/O it
// issues from there goes to that thread's AIO context (store.cc,
// resume_waiters), as for any put that waits; that context is sized when
// the waker first submits, at the depth of the store opened last
// (AioIO::new_ctx_depth_), so a wide batch on a store that is out of space
// can find it full there and fail those puts with -EAGAIN.
//
// results[i] is the rc of put(keys[i], *bufs[i]): 0, or -errno (-EINVAL for
// a buffer that is not valid or was allocated for another key size). Every
// results[i] is written before this returns; one is written as soon as its
// put completes. A failed put does not stop the batch. Returns 0, or -ENOMEM
// (nothing started, results untouched) if the batch's bookkeeping cannot be
// allocated.
//
// Order. The batch is the same as put() called in list order where that
// matters: puts of equal keys in one batch run one after another, each
// started after the previous one of its key completed, so the last one in the
// list is what the key holds afterwards (as for serial puts), a failed one
// does not stop the later ones. Puts of different keys overlap freely. The
// start order is the list order: a put waiting for an earlier put of its key
// holds back the ones behind it (the cost of a batch with repeated keys is
// that it overlaps less; one without them pays only the scan that found none).
//
// Each bufs[i] must be a distinct, non-null PutBuffer: a put writes its buffer's header
// in place, and a buffer serves one put at a time. Keys are copied into the
// record before the put first suspends, but they and the buffers must stay
// alive until this returns.
template <typename IO>
int multi_put(uDepot<IO>& store, uint32_t n, const uint8_t* const keys[],
              const uint32_t key_sizes[], PutBuffer* const bufs[],
              int results[], unsigned window) {
    if (n == 0) return 0;
    struct Slot {
        std::optional<CoroTask<int>> task;
        uint32_t i = 0;
    };
    // prev: the previous index with the same key, or kNone; done: that
    // index's put has been reaped.
    constexpr uint32_t kNone = UINT32_MAX;
    struct Dep {
        uint32_t prev = kNone;
        bool done = false;
    };
    const uint32_t nslots = std::min<uint32_t>(n, std::max(window, 1u));
    std::unique_ptr<Slot[]> slots(new (std::nothrow) Slot[nslots]);
    std::unique_ptr<Dep[]> deps(new (std::nothrow) Dep[n]);
    if (!slots || !deps) return -ENOMEM;

    if (n > 1) {
        std::unique_ptr<uint32_t[]> order(new (std::nothrow) uint32_t[n]);
        if (!order) return -ENOMEM;
        for (uint32_t i = 0; i < n; ++i) order[i] = i;
        // Equal keys end up adjacent, in list order.
        auto less = [&](uint32_t a, uint32_t b) {
            if (key_sizes[a] != key_sizes[b]) return key_sizes[a] < key_sizes[b];
            int c = key_sizes[a] ? std::memcmp(keys[a], keys[b], key_sizes[a])
                                 : 0;
            return c != 0 ? c < 0 : a < b;
        };
        std::sort(order.get(), order.get() + n, less);
        for (uint32_t j = 1; j < n; ++j) {
            const uint32_t a = order[j - 1], b = order[j];
            if (key_sizes[a] == key_sizes[b] &&
                (key_sizes[a] == 0 ||
                 std::memcmp(keys[a], keys[b], key_sizes[a]) == 0))
                deps[b].prev = a;
        }
    }

    uint32_t next = 0, done = 0;
    while (done < n) {
        for (uint32_t k = 0; k < nslots; ++k) {
            Slot& s = slots[k];
            // Reap first, so the slot it frees is refilled in this pass.
            if (s.task && s.task->await_ready()) {
                results[s.i] = s.task->run_sync();
                s.task.reset();
                deps[s.i].done = true;
                ++done;
            }
            if (!s.task && next < n &&
                (deps[next].prev == kNone || deps[deps[next].prev].done)) {
                s.i = next++;
                s.task.emplace(store.put(
                    std::span<const uint8_t>(keys[s.i], key_sizes[s.i]),
                    *bufs[s.i]));
            }
        }
        // As run_sync(): nothing of this thread's pending means the rest
        // are waiting on another thread's work; let it run.
        if (done < n && !poll_this_thread()) std::this_thread::yield();
    }
    return 0;
}

}  // namespace udepot

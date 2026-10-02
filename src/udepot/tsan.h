// Copyright (c) 2024-2026 Nikolas Ioannou
// SPDX-License-Identifier: BSD-3-Clause

#pragma once

// ThreadSanitizer cannot see synchronization that goes through the kernel:
// a request handed to io_submit or an io_uring SQE on one thread and reaped
// by the poller on another is ordered by the kernel, not by anything TSAN
// tracks, so every access the two make to the request (and to the coroutine
// frame behind it) is reported as a race. These annotate that edge. They
// compile to nothing outside TSAN builds.

#if defined(__SANITIZE_THREAD__)
#define UDEPOT_TSAN 1
#elif defined(__has_feature)
#if __has_feature(thread_sanitizer)
#define UDEPOT_TSAN 1
#endif
#endif

#if defined(UDEPOT_TSAN)
extern "C" void __tsan_acquire(void* addr);
extern "C" void __tsan_release(void* addr);
#endif

namespace udepot {

// On the submitting thread, after its last write before the kernel owns
// the request.
inline void tsan_release_to_kernel([[maybe_unused]] void* req) noexcept {
#if defined(UDEPOT_TSAN)
    __tsan_release(req);
#endif
}

// On the reaping thread, before it touches the request.
inline void tsan_acquire_from_kernel([[maybe_unused]] void* req) noexcept {
#if defined(UDEPOT_TSAN)
    __tsan_acquire(req);
#endif
}

}  // namespace udepot

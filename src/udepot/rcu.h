// Copyright (c) 2024-2026 Nikolas Ioannou
// SPDX-License-Identifier: BSD-3-Clause

#pragma once

#include <array>
#include <atomic>
#include <cstdint>

namespace udepot {

// Per-thread epoch-based userspace RCU.
//
// Readers: zero shared-line atomic RMW. Cost is two thread-local stores and
// one global load (read-shared, rarely written by the writer).
//
// Writers: increment the global epoch and scan all registered threads until
// each has either gone quiescent or passed the target epoch.
//
// The nesting counter handles multiple coroutines on the same thread — the
// thread goes quiescent only when all coroutines have exited their critical
// sections.
class Rcu {
public:
    static constexpr uint32_t kMaxThreads = 256;

    // Opaque handle returned by register_thread(). Callers store it
    // per-thread and pass it to read_lock/read_unlock.
    class Token {
    public:
        Token() noexcept : slot_(kInvalid) {}
        bool valid() const noexcept { return slot_ != kInvalid; }

    private:
        static constexpr uint32_t kInvalid = UINT32_MAX;
        uint32_t slot_;
        explicit Token(uint32_t s) noexcept : slot_(s) {}
        friend class Rcu;
    };

    // RAII read-side critical section.
    class ReadGuard {
    public:
        ReadGuard(Rcu& rcu, Token t) noexcept : rcu_(rcu), token_(t) {
            rcu_.read_lock(token_);
        }
        ~ReadGuard() { rcu_.read_unlock(token_); }

        ReadGuard(const ReadGuard&) = delete;
        ReadGuard& operator=(const ReadGuard&) = delete;

    private:
        Rcu& rcu_;
        Token token_;
    };

    Rcu() noexcept = default;

    uint64_t id() const noexcept { return id_; }

    // Register the calling thread. Returns a Token for use with
    // read_lock/read_unlock. Must be called before any RCU operations
    // on this thread.
    Token register_thread() noexcept;

    // Unregister a previously registered thread. The thread must not be
    // inside a read-side critical section.
    void unregister_thread(Token t) noexcept;

    // Enter a read-side critical section. The thread must be registered.
    void read_lock(Token t) noexcept;

    // Exit a read-side critical section.
    void read_unlock(Token t) noexcept;

    // Wait until all threads that were in a read-side critical section at
    // the time of this call have exited. May spin.
    void synchronize() noexcept;

    uint32_t thread_count() const noexcept {
        return thread_count_.load(std::memory_order_relaxed);
    }

private:
    struct alignas(64) ThreadState {
        // Odd = active (in a critical section), 0 = quiescent.
        // When active, stores the global epoch at entry time | 1.
        std::atomic<uint64_t> epoch{0};
        uint32_t nesting{0};
        bool registered{false};
    };

    std::array<ThreadState, kMaxThreads> threads_{};
    alignas(64) std::atomic<uint64_t> global_epoch_{0};
    std::atomic<uint32_t> thread_count_{0};
    uint64_t id_ = next_id();

    static uint64_t next_id() noexcept;
};

}  // namespace udepot

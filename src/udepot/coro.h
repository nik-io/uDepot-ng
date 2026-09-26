// Copyright (c) 2024-2026 Nikolas Ioannou
// SPDX-License-Identifier: BSD-3-Clause

#pragma once

#include <atomic>
#include <cassert>
#include <coroutine>
#include <cstdint>
#include <exception>
#include <thread>
#include <utility>

namespace udepot {

template <typename T = int>
class [[nodiscard]] CoroTask {
public:
    struct promise_type;
    using handle_type = std::coroutine_handle<promise_type>;

    struct FinalAwaitable {
        bool await_ready() noexcept { return false; }

        std::coroutine_handle<> await_suspend(handle_type h) noexcept {
            auto& p = h.promise();
            p.completed_.store(true, std::memory_order_release);
            void* prev = p.continuation_.exchange(
                completed_tag(), std::memory_order_acq_rel);
            if (prev != nullptr)
                return std::coroutine_handle<>::from_address(prev);
            return std::noop_coroutine();
        }

        void await_resume() noexcept {}
    };

    struct promise_type {
        // nullptr = no continuation set (initial state).
        // completed_tag() = child has completed.
        // anything else = parent's coroutine handle address.
        std::atomic<void*> continuation_{nullptr};
        T result_{};
        std::atomic<bool> completed_{false};

        std::suspend_never initial_suspend() noexcept { return {}; }
        FinalAwaitable final_suspend() noexcept { return {}; }

        CoroTask get_return_object() noexcept {
            return CoroTask{handle_type::from_promise(*this)};
        }

        void return_value(T val) noexcept { result_ = val; }
        void unhandled_exception() noexcept { std::terminate(); }
    };

    CoroTask(const CoroTask&) = delete;
    CoroTask& operator=(const CoroTask&) = delete;

    CoroTask(CoroTask&& other) noexcept
        : handle_(std::exchange(other.handle_, {})) {}

    CoroTask& operator=(CoroTask&& other) noexcept {
        if (this != &other) {
            destroy();
            handle_ = std::exchange(other.handle_, {});
        }
        return *this;
    }

    ~CoroTask() { destroy(); }

    bool done() const noexcept { return !handle_ || handle_.done(); }

    // Awaitable interface — for co_await from another coroutine.
    bool await_ready() const noexcept {
        if (!handle_) return true;
        return handle_.promise().continuation_.load(
            std::memory_order_acquire) == completed_tag();
    }

    bool await_suspend(std::coroutine_handle<> h) noexcept {
        void* prev = handle_.promise().continuation_.exchange(
            h.address(), std::memory_order_acq_rel);
        return prev != completed_tag();
    }

    T await_resume() noexcept { return handle_.promise().result_; }

    // Drive to completion from non-coroutine code.
    // For sync backends (PosixIO), the coroutine completes eagerly.
    // For async backends (AioIO), yields until the poller resumes it.
    // TODO: reconsider busy-wait vs kernel-assisted wait (futex/eventfd)
    // once we have real device latency measurements.
    T run_sync() {
        while (!handle_.promise().completed_.load(std::memory_order_acquire))
            std::this_thread::yield();
        T result = handle_.promise().result_;
        destroy();
        return result;
    }

private:
    handle_type handle_;

    explicit CoroTask(handle_type h) noexcept : handle_(h) {}

    // Coroutine frames are pointer-aligned, so address 0x1 can never
    // be a valid frame and is safe to use as a sentinel.
    static void* completed_tag() noexcept {
        return reinterpret_cast<void*>(uintptr_t{1});
    }

    void destroy() noexcept {
        if (handle_) {
            assert(handle_.done() && "destroying a CoroTask that has not completed");
            handle_.destroy();
            handle_ = {};
        }
    }
};

}  // namespace udepot

#pragma once

#include <atomic>
#include <cassert>
#include <coroutine>
#include <cstdint>
#include <exception>
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
            h.promise().completed_.store(true, std::memory_order_release);
            return h.promise().continuation_;
        }

        void await_resume() noexcept {}
    };

    struct promise_type {
        std::coroutine_handle<> continuation_ = std::noop_coroutine();
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
    bool await_ready() const noexcept { return done(); }

    void await_suspend(std::coroutine_handle<> h) noexcept {
        handle_.promise().continuation_ = h;
    }

    T await_resume() noexcept { return handle_.promise().result_; }

    // Drive to completion from non-coroutine code.
    // For sync backends (PosixIO), the coroutine completes eagerly.
    // For async backends (AioIO), spins until the poller resumes it.
    T run_sync() {
        while (!handle_.promise().completed_.load(std::memory_order_acquire))
            ;
        T result = handle_.promise().result_;
        destroy();
        return result;
    }

private:
    handle_type handle_;

    explicit CoroTask(handle_type h) noexcept : handle_(h) {}

    void destroy() noexcept {
        if (handle_) {
            assert(handle_.done() && "destroying a CoroTask that has not completed");
            handle_.destroy();
            handle_ = {};
        }
    }
};

}  // namespace udepot

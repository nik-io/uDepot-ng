// Copyright (c) 2024-2026 Nikolas Ioannou
// SPDX-License-Identifier: BSD-3-Clause

#pragma once

#include <atomic>
#include <coroutine>
#include <cstddef>
#include <functional>
#include <mutex>
#include <string>
#include <sys/epoll.h>
#include <sys/socket.h>
#include <sys/types.h>
#include <thread>
#include <unordered_map>
#include <vector>

#include "udepot/coro.h"

namespace udepot {

// ─────────────────────────────────────────────────────────────────────────────
// EpollOpType — direction of an epoll-backed I/O operation
// ─────────────────────────────────────────────────────────────────────────────
enum class EpollOpType { kIn, kOut };

class EpollState;

// ─────────────────────────────────────────────────────────────────────────────
// EpollOpAwaitable
//
// Wraps a single retried-on-EAGAIN socket operation in a C++23 awaitable.
// The caller writes:  ssize_t r = co_await epoll.recv(fd, buf, len, 0);
//
// Lifecycle:
//  1. await_ready():   try the syscall. If it succeeds (or fails with a hard
//                      error), cache the result and return true (no suspend).
//                      If EAGAIN/EWOULDBLOCK, return false.
//  2. await_suspend(): publish this awaitable in the per-fd map so the poller
//                      can complete it when epoll fires.
//  3. The poller retries the syscall. EAGAIN means the readiness event was
//     stale (the data was consumed in an earlier await_ready on another
//     thread), so the awaitable stays published; otherwise the poller
//     records the result and resumes the coroutine.
//  4. await_resume():  return the result. Register the new fd on accept path.
// ─────────────────────────────────────────────────────────────────────────────
struct EpollOpAwaitable {
    EpollState* es_;
    int fd_;
    EpollOpType ty_;
    std::function<ssize_t()> syscall_;
    ssize_t ret_ = -1;
    int errno_ = 0;     // errno of ret_ when set by the poller
    bool ready_ = false;
    bool published_ = false;  // counted in pending_waits_
    bool reg_ = false;  // register new fd after accept
    std::coroutine_handle<> handle_;

    EpollOpAwaitable(EpollState* es, int fd, EpollOpType ty,
                     std::function<ssize_t()> fn, bool reg = false)
        : es_(es), fd_(fd), ty_(ty), syscall_(std::move(fn)), reg_(reg) {}

    EpollOpAwaitable(const EpollOpAwaitable&) = delete;
    EpollOpAwaitable& operator=(const EpollOpAwaitable&) = delete;
    EpollOpAwaitable(EpollOpAwaitable&&) = delete;

    bool await_ready() noexcept;
    bool await_suspend(std::coroutine_handle<> h) noexcept;
    ssize_t await_resume() noexcept;
};

// ─────────────────────────────────────────────────────────────────────────────
// EpollState — per-instance epoll context
//
// Ported from uDepot's trt::EpollState. Replaces TRT's LocalSingleAsyncObj
// with std::coroutine_handle<> for direct coroutine resumption.
// ─────────────────────────────────────────────────────────────────────────────
class EpollState {
    friend struct EpollOpAwaitable;

    enum class State { kUninitialized, kReady, kDraining, kDone };
    std::atomic<State> state_{State::kUninitialized};
    int epfd_ = -1;

    // Waiters are published and taken under fds_mu_. Once published, an
    // awaitable belongs to the poller (or shutdown_all) until resumed.
    struct FdInfo {
        uint32_t event_mask;
        int old_flags;
        EpollOpAwaitable* waiter_in_ = nullptr;
        EpollOpAwaitable* waiter_out_ = nullptr;

        FdInfo(uint32_t mask, int fl)
            : event_mask(mask), old_flags(fl) {}
    };
    std::unordered_map<int, FdInfo> fds_;
    std::mutex fds_mu_;
    std::atomic<size_t> pending_waits_{0};

    std::thread poller_;
    std::atomic<bool> running_{false};

    void notify_maybe(int fd, EpollOpType ty);

public:
    EpollState() = default;
    ~EpollState();

    EpollState(const EpollState&) = delete;
    EpollState& operator=(const EpollState&) = delete;

    int init();
    void stop();

    void register_fd(int fd, uint32_t event_mask);
    int deregister_fd(int fd);

    int listen(int sockfd, int backlog);

    EpollOpAwaitable accept(int sockfd, struct sockaddr* addr,
                            socklen_t* addrlen);
    EpollOpAwaitable accept_ll(int sockfd, struct sockaddr* addr,
                               socklen_t* addrlen);

    EpollOpAwaitable recv(int fd, void* buf, size_t len, int flags);
    EpollOpAwaitable send(int fd, const void* buf, size_t len, int flags);
    EpollOpAwaitable sendmsg(int fd, const struct msghdr* msg, int flags);
    EpollOpAwaitable recvmsg(int fd, struct msghdr* msg, int flags);

    int close_fd(int fd);

    bool is_running() const noexcept {
        return state_.load(std::memory_order_acquire) == State::kReady;
    }

private:
    void poller_loop();
    void shutdown_all();
};

// ─────────────────────────────────────────────────────────────────────────────
// Connection — wraps a socket fd with coroutine-based send/recv.
// Equivalent of uDepot's ConnectionTrtEpoll.
// ─────────────────────────────────────────────────────────────────────────────
class Connection {
public:
    Connection(EpollState& es, int fd) : es_(es), fd_(fd) {}
    ~Connection() = default;

    Connection(const Connection&) = delete;
    Connection& operator=(const Connection&) = delete;

    CoroTask<ssize_t> send(const void* buf, size_t len, int flags);
    CoroTask<ssize_t> recv(void* buf, size_t len, int flags);
    CoroTask<ssize_t> sendmsg(const struct msghdr* msg, int flags);
    CoroTask<ssize_t> recvmsg(struct msghdr* msg, int flags);

    int fd() const noexcept { return fd_; }

    // Receive exactly `len` bytes, retrying on partial reads.
    // Returns 0 on success, or -errno on error (including ECONNRESET on EOF).
    CoroTask<int> recv_full(void* buf, size_t len, int flags);

    // Send exactly `len` bytes, retrying on partial sends.
    // Returns 0 on success, or -errno on error.
    CoroTask<int> send_full(const void* buf, size_t len, int flags);

private:
    EpollState& es_;
    int fd_;
};

}  // namespace udepot

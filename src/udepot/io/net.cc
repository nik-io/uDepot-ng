// Copyright (c) 2024-2026 Nikolas Ioannou
// SPDX-License-Identifier: BSD-3-Clause

#include "udepot/io/net.h"

#include <cassert>
#include <cerrno>
#include <cstdio>
#include <cstring>
#include <fcntl.h>
#include <sys/epoll.h>
#include <sys/socket.h>
#include <unistd.h>

static constexpr int kMaxEvents = 128;

namespace udepot {

// ─────────────────────────────────────────────────────────────────────────────
// Helpers
// ─────────────────────────────────────────────────────────────────────────────
static int setnonblocking(int fd, int& old_flags) {
    old_flags = fcntl(fd, F_GETFL, 0);
    if (old_flags == -1) old_flags = 0;
    return fcntl(fd, F_SETFL, old_flags | O_NONBLOCK);
}

static int setnonblocking(int fd) {
    int unused;
    return setnonblocking(fd, unused);
}

// ─────────────────────────────────────────────────────────────────────────────
// EpollOpAwaitable
//
// Ported from uDepot's trt::EpollOpAwaitable. Replaces TRT's
// LocalSingleAsyncObj wakeup with direct coroutine_handle resumption.
// ─────────────────────────────────────────────────────────────────────────────
bool EpollOpAwaitable::await_ready() noexcept {
    if (es_->state_.load(std::memory_order_acquire) ==
        EpollState::State::kDraining) {
        errno = ESHUTDOWN;
        ret_ = -1;
        ready_ = true;
        return true;
    }

    ret_ = syscall_();
    if (ret_ != -1 || (errno != EAGAIN && errno != EWOULDBLOCK)) {
        if (ret_ > 0 && reg_) es_->register_fd(static_cast<int>(ret_), EPOLLIN);
        ready_ = true;
        return true;
    }
    return false;
}

bool EpollOpAwaitable::await_suspend(std::coroutine_handle<> h) noexcept {
    handle_ = h;

    std::lock_guard<std::mutex> lock(es_->fds_mu_);
    auto entry = es_->fds_.find(fd_);
    // Checked under fds_mu_: shutdown_all collects waiters under it, so a
    // waiter is either published before that or not at all.
    if (es_->state_.load(std::memory_order_acquire) !=
            EpollState::State::kReady ||
        entry == es_->fds_.end()) {
        ret_ = -1;
        errno_ = ESHUTDOWN;
        return false;
    }

    EpollOpAwaitable** slot = ty_ == EpollOpType::kIn
                                  ? &entry->second.waiter_in_
                                  : &entry->second.waiter_out_;
    assert(*slot == nullptr);
    // Counted before publishing: once published, the poller may complete
    // and destroy this awaitable before await_suspend returns.
    es_->pending_waits_.fetch_add(1, std::memory_order_relaxed);
    published_ = true;
    *slot = this;
    return true;
}

ssize_t EpollOpAwaitable::await_resume() noexcept {
    if (ready_) return ret_;

    if (published_)
        es_->pending_waits_.fetch_sub(1, std::memory_order_relaxed);
    if (ret_ < 0) {
        errno = errno_;
        return ret_;
    }
    if (ret_ > 0 && reg_) es_->register_fd(static_cast<int>(ret_), EPOLLIN);
    return ret_;
}

// ─────────────────────────────────────────────────────────────────────────────
// EpollState
//
// Ported from uDepot's trt::EpollState. The poller runs as a std::thread
// instead of a TRT coroutine task.
// ─────────────────────────────────────────────────────────────────────────────
EpollState::~EpollState() {
    stop();
}

int EpollState::init() {
    if (state_.load(std::memory_order_acquire) != State::kUninitialized)
        return -EINVAL;

    epfd_ = epoll_create1(EPOLL_CLOEXEC);
    if (epfd_ == -1) {
        perror("epoll_create1");
        return -errno;
    }

    state_.store(State::kReady, std::memory_order_release);
    running_.store(true, std::memory_order_relaxed);
    poller_ = std::thread(&EpollState::poller_loop, this);
    return 0;
}

void EpollState::stop() {
    State expected = State::kReady;
    if (!state_.compare_exchange_strong(expected, State::kDraining,
                                        std::memory_order_acq_rel))
        return;

    running_.store(false, std::memory_order_release);

    if (poller_.joinable())
        poller_.join();

    shutdown_all();

    ::close(epfd_);
    epfd_ = -1;
    state_.store(State::kDone, std::memory_order_release);
}

void EpollState::register_fd(int fd, uint32_t event_mask) {
    int old_flags;
    setnonblocking(fd, old_flags);

    {
        std::lock_guard<std::mutex> lock(fds_mu_);
        assert(fds_.find(fd) == fds_.end());
        fds_.emplace(std::piecewise_construct,
                     std::make_tuple(fd),
                     std::make_tuple(event_mask, old_flags));
    }

    struct epoll_event ev{};
    ev.events = event_mask;
    ev.data.fd = fd;
    int ret = epoll_ctl(epfd_, EPOLL_CTL_ADD, fd, &ev);
    if (ret < 0) {
        perror("epoll_ctl EPOLL_CTL_ADD");
        abort();
    }
}

int EpollState::deregister_fd(int fd) {
    {
        std::lock_guard<std::mutex> lock(fds_mu_);
        auto iter = fds_.find(fd);
        if (iter == fds_.end())
            return state_.load(std::memory_order_acquire) == State::kDone
                       ? 0 : -ENOENT;
        fds_.erase(iter);
    }
    epoll_ctl(epfd_, EPOLL_CTL_DEL, fd, nullptr);
    return 0;
}

int EpollState::listen(int sockfd, int backlog) {
    int ret = ::listen(sockfd, backlog);
    if (ret < 0) return -errno;
    setnonblocking(sockfd);
    register_fd(sockfd, EPOLLIN);
    return 0;
}

int EpollState::close_fd(int fd) {
    deregister_fd(fd);
    return ::close(fd);
}

void EpollState::notify_maybe(int fd, EpollOpType ty) {
    EpollOpAwaitable* w;
    {
        std::lock_guard<std::mutex> lock(fds_mu_);
        auto entry = fds_.find(fd);
        if (entry == fds_.end()) return;

        EpollOpAwaitable** slot = ty == EpollOpType::kIn
                                      ? &entry->second.waiter_in_
                                      : &entry->second.waiter_out_;
        w = *slot;
        if (!w) return;

        // Complete the operation here rather than in the resumed coroutine:
        // this event may be stale, its data consumed by an await_ready() that
        // ran on another thread after epoll_wait returned. Then the syscall
        // fails with EAGAIN and the waiter keeps waiting. Done under fds_mu_
        // so the slot cannot change; the syscall is non-blocking.
        ssize_t r = w->syscall_();
        if (r == -1 && (errno == EAGAIN || errno == EWOULDBLOCK)) return;
        w->ret_ = r;
        w->errno_ = errno;
        *slot = nullptr;
    }
    // Resume outside the lock — the coroutine may re-enter fds_ via
    // await_suspend or register_fd.
    w->handle_.resume();
}

void EpollState::shutdown_all() {
    // Take every waiter first, then resume: a resumed coroutine may call
    // close_fd/deregister_fd, which must not run during the iteration.
    std::vector<EpollOpAwaitable*> waiters;
    {
        std::lock_guard<std::mutex> lock(fds_mu_);
        for (auto& [fd, info] : fds_) {
            for (EpollOpAwaitable* w : {info.waiter_in_, info.waiter_out_}) {
                if (!w) continue;
                w->ret_ = -1;
                w->errno_ = ESHUTDOWN;
                waiters.push_back(w);
            }
            epoll_ctl(epfd_, EPOLL_CTL_DEL, fd, nullptr);
        }
        fds_.clear();
    }
    for (EpollOpAwaitable* w : waiters)
        w->handle_.resume();
}

void EpollState::poller_loop() {
    struct epoll_event events[kMaxEvents];

    while (running_.load(std::memory_order_acquire)) {
        int timeout = (pending_waits_.load(std::memory_order_relaxed) > 0) ? 10 : 100;
        int n = epoll_wait(epfd_, events, kMaxEvents, timeout);
        if (n < 0) {
            if (errno == EINTR) continue;
            perror("epoll_wait");
            break;
        }

        for (int i = 0; i < n; ++i) {
            int fd = events[i].data.fd;
            if (events[i].events & EPOLLIN)
                notify_maybe(fd, EpollOpType::kIn);
            if (events[i].events & EPOLLOUT)
                notify_maybe(fd, EpollOpType::kOut);
        }
    }
}

// ─────────────────────────────────────────────────────────────────────────────
// EpollState socket operation factories
// ─────────────────────────────────────────────────────────────────────────────
EpollOpAwaitable EpollState::accept(int sockfd, struct sockaddr* addr,
                                    socklen_t* addrlen) {
    if (state_.load(std::memory_order_acquire) == State::kDraining) {
        return {this, sockfd, EpollOpType::kIn, [=]() -> ssize_t {
            errno = ESHUTDOWN; return -1;
        }};
    }
    return {this, sockfd, EpollOpType::kIn,
            [=]() -> ssize_t { return static_cast<ssize_t>(::accept(sockfd, addr, addrlen)); },
            true};
}

EpollOpAwaitable EpollState::accept_ll(int sockfd, struct sockaddr* addr,
                                       socklen_t* addrlen) {
    if (state_.load(std::memory_order_acquire) == State::kDraining) {
        return {this, sockfd, EpollOpType::kIn, [=]() -> ssize_t {
            errno = ESHUTDOWN; return -1;
        }};
    }
    return {this, sockfd, EpollOpType::kIn,
            [=]() -> ssize_t { return static_cast<ssize_t>(::accept(sockfd, addr, addrlen)); }};
}

EpollOpAwaitable EpollState::recv(int fd, void* buf, size_t len, int flags) {
    return {this, fd, EpollOpType::kIn,
            [=]() -> ssize_t { return ::recv(fd, buf, len, flags); }};
}

EpollOpAwaitable EpollState::send(int fd, const void* buf, size_t len,
                                  int flags) {
    return {this, fd, EpollOpType::kOut,
            [=]() -> ssize_t { return ::send(fd, buf, len, flags); }};
}

EpollOpAwaitable EpollState::sendmsg(int fd, const struct msghdr* msg,
                                     int flags) {
    return {this, fd, EpollOpType::kOut,
            [=]() -> ssize_t { return ::sendmsg(fd, msg, flags); }};
}

EpollOpAwaitable EpollState::recvmsg(int fd, struct msghdr* msg, int flags) {
    return {this, fd, EpollOpType::kIn,
            [=]() -> ssize_t { return ::recvmsg(fd, msg, flags); }};
}

// ─────────────────────────────────────────────────────────────────────────────
// Connection
// ─────────────────────────────────────────────────────────────────────────────
CoroTask<ssize_t> Connection::send(const void* buf, size_t len, int flags) {
    co_return co_await es_.send(fd_, buf, len, flags);
}

CoroTask<ssize_t> Connection::recv(void* buf, size_t len, int flags) {
    co_return co_await es_.recv(fd_, buf, len, flags);
}

CoroTask<ssize_t> Connection::sendmsg(const struct msghdr* msg, int flags) {
    co_return co_await es_.sendmsg(fd_, msg, flags);
}

CoroTask<ssize_t> Connection::recvmsg(struct msghdr* msg, int flags) {
    co_return co_await es_.recvmsg(fd_, msg, flags);
}

CoroTask<int> Connection::recv_full(void* buf, size_t len, int flags) {
    auto* p = static_cast<uint8_t*>(buf);
    size_t total = 0;
    while (total < len) {
        ssize_t r = co_await es_.recv(fd_, p + total, len - total, flags);
        if (r < 0) co_return -errno;
        if (r == 0) co_return -ECONNRESET;
        total += static_cast<size_t>(r);
    }
    co_return 0;
}

CoroTask<int> Connection::send_full(const void* buf, size_t len, int flags) {
    auto* p = static_cast<const uint8_t*>(buf);
    size_t total = 0;
    while (total < len) {
        ssize_t r = co_await es_.send(fd_, p + total, len - total, flags);
        if (r < 0) co_return -errno;
        if (r == 0) co_return -ECONNRESET;
        total += static_cast<size_t>(r);
    }
    co_return 0;
}

}  // namespace udepot

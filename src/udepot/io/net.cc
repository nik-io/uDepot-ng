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
    if (es_->state_ == EpollState::State::kDraining) {
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
    assert(es_->state_ == EpollState::State::kReady);

    handle_ = h;

    {
        std::lock_guard<std::mutex> lock(es_->fds_mu_);
        auto entry = es_->fds_.find(fd_);
        assert(entry != es_->fds_.end());

        switch (ty_) {
            case EpollOpType::kIn:
                assert(!entry->second.handle_in_);
                entry->second.handle_in_ = h;
                break;
            case EpollOpType::kOut:
                assert(!entry->second.handle_out_);
                entry->second.handle_out_ = h;
                break;
        }
    }
    es_->pending_waits_.fetch_add(1, std::memory_order_relaxed);
    return true;
}

ssize_t EpollOpAwaitable::await_resume() noexcept {
    if (ready_) return ret_;

    es_->pending_waits_.fetch_sub(1, std::memory_order_relaxed);
    ret_ = syscall_();
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
    if (state_ != State::kUninitialized)
        return -EINVAL;

    epfd_ = epoll_create1(EPOLL_CLOEXEC);
    if (epfd_ == -1) {
        perror("epoll_create1");
        return -errno;
    }

    state_ = State::kReady;
    running_.store(true, std::memory_order_relaxed);
    poller_ = std::thread(&EpollState::poller_loop, this);
    return 0;
}

void EpollState::stop() {
    if (state_ != State::kReady)
        return;

    state_ = State::kDraining;
    running_.store(false, std::memory_order_release);

    if (poller_.joinable())
        poller_.join();

    shutdown_all();

    ::close(epfd_);
    epfd_ = -1;
    state_ = State::kDone;
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
            return state_ == State::kDone ? 0 : -ENOENT;
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
    std::coroutine_handle<> h;
    {
        std::lock_guard<std::mutex> lock(fds_mu_);
        auto entry = fds_.find(fd);
        if (entry == fds_.end()) return;

        std::coroutine_handle<>* hptr;
        switch (ty) {
            case EpollOpType::kIn:  hptr = &entry->second.handle_in_;  break;
            case EpollOpType::kOut: hptr = &entry->second.handle_out_; break;
            default: abort();
        }

        h = *hptr;
        if (!h) return;
        *hptr = {};
    }
    // Resume outside the lock — the coroutine may re-enter fds_ via
    // await_suspend or register_fd.
    h.resume();
}

void EpollState::shutdown_all() {
    // Called after the poller thread has been joined, so no lock needed
    // for the iteration itself. Individual handles are extracted and
    // resumed one at a time.
    for (auto iter = fds_.begin(); iter != fds_.end();) {
        auto& info = iter->second;
        if (info.handle_in_) {
            auto h = info.handle_in_;
            info.handle_in_ = {};
            h.resume();
        }
        if (info.handle_out_) {
            auto h = info.handle_out_;
            info.handle_out_ = {};
            h.resume();
        }
        epoll_ctl(epfd_, EPOLL_CTL_DEL, iter->first, nullptr);
        iter = fds_.erase(iter);
    }
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
    if (state_ == State::kDraining) {
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
    if (state_ == State::kDraining) {
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

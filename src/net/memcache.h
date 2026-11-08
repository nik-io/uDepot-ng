// Copyright (c) 2024-2026 Nikolas Ioannou
// SPDX-License-Identifier: BSD-3-Clause

#pragma once

#include <atomic>
#include <cstdint>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

namespace udepot {

// Test seams of MemcacheServer, never set in production. Atomic: tests set
// and clear them while the server's threads may be reading them.
struct MemcacheTestHooks {
    using WorkerHook = int (*)(unsigned worker);
    // Called in start() before the socketpair of each worker; returns the
    // errno that socketpair should fail with (0 lets it go on).
    inline static std::atomic<WorkerHook> socketpair_test_hook{nullptr};
    // Called on a worker's own thread each time its hand-off socket
    // delivers (also an fd, which a failure closes with the others queued):
    // returns the errno that receive should fail with (0 lets it go on). It
    // may block, which stalls the worker (it stops draining its socket).
    inline static std::atomic<WorkerHook> handoff_recv_test_hook{nullptr};
    // Called on whichever thread a connection finishes on, before it can be
    // reclaimed.
    using FinishHook = void (*)();
    inline static std::atomic<FinishHook> connection_finished_test_hook{
        nullptr};
};

// Memcache text protocol server for uDepot.
//
// Implements the standard memcache text protocol (GET, SET, ADD, REPLACE,
// DELETE, INCR, DECR, APPEND, PREPEND, STATS, VERSION, QUIT) on top of
// any uDepot<IO> store instance.
//
// Value storage format (matches uDepot):
//   [user_data][expiry: int64_t, 8 bytes][flags: uint32_t, 4 bytes]
// The last 12 bytes of the stored value are metadata, invisible to the
// memcache client.
//
// Threading model: one accept thread and a fixed pool of worker threads.
// A connection is a coroutine, multiplexed with the others on one worker,
// which runs that worker's socket epoll and its store I/O completions side
// by side (as uDepot's trt-memcache serves connections as tasks on its
// fixed set of TRT scheduler threads), so the threads, and with them the
// AIO contexts and io_uring rings (one per thread, each of the store's
// queue depth), are bounded by the pool, not by the connections.
template <typename Store>
class MemcacheServer {
public:
    struct Config {
        std::string bind_addr = "127.0.0.1";
        uint16_t port = 11211;
        int backlog = 128;
        // Worker threads serving connections; 0 means
        // std::thread::hardware_concurrency(), and at least 1. Each worker
        // has one AIO context / io_uring ring of the store's queue_depth
        // slots, shared by all its connections: it keeps many I/Os in
        // flight at once, but more than queue_depth concurrent operations
        // on one worker fail with -EAGAIN (the client gets a SERVER_ERROR),
        // as legacy's TRT threads do. Size queue_depth for the connections
        // per worker.
        unsigned workers = 0;
    };

    explicit MemcacheServer(Store& store);
    ~MemcacheServer();

    MemcacheServer(const MemcacheServer&) = delete;
    MemcacheServer& operator=(const MemcacheServer&) = delete;

    int start(const Config& config);
    void stop();

    bool is_running() const noexcept {
        return running_.load(std::memory_order_acquire);
    }

    uint16_t port() const noexcept { return port_; }

    // Worker threads of the running server (0 when stopped). Like
    // connections(), it reads the worker list that start() and stop()
    // build and tear down: call it from the thread that calls them, or
    // only while neither runs, never concurrently with either.
    unsigned workers() const noexcept {
        return static_cast<unsigned>(workers_.size());
    }

    // Connections the workers currently hold: accepted and not yet
    // finished and reclaimed. The count itself is atomic, but see
    // workers() for when this may be called.
    size_t connections() const noexcept;

    // Connections closed because no worker could take them (every
    // worker's hand-off queue full or broken). May be read at any time.
    uint64_t refused_connections() const noexcept {
        return refused_.load(std::memory_order_relaxed);
    }

    uint64_t bytes_stored() const noexcept {
        return bytes_stored_.load(std::memory_order_relaxed);
    }

    static constexpr size_t kMetadataSize = 12;  // 8 (expiry) + 4 (flags)
    static constexpr size_t kMaxKeyLen = 250;
    static constexpr size_t kMaxValueLen = 1 * 1024 * 1024;  // 1 MiB
    static constexpr int64_t kRealtimeMaxdelta = 60 * 60 * 24 * 30;  // 30 days

private:
    Store& store_;
    std::atomic<bool> running_{false};
    int listen_fd_ = -1;
    uint16_t port_ = 0;
    std::thread accept_thread_;
    struct Worker;
    std::vector<std::unique_ptr<Worker>> workers_;  // set by start()/stop()
    size_t next_worker_ = 0;                        // accept thread only
    uint64_t handoff_failures_ = 0;                 // accept thread only
    std::atomic<uint64_t> refused_{0};
    std::atomic<uint64_t> bytes_stored_{0};

    std::mutex conn_fds_mu_;
    std::vector<int> conn_fds_;

    void accept_loop();
    void stop_workers();
};

}  // namespace udepot

// Copyright (c) 2024-2026 Nikolas Ioannou
// SPDX-License-Identifier: BSD-3-Clause

#pragma once

#include <atomic>
#include <cstdint>
#include <string>
#include <thread>
#include <vector>

namespace udepot {

// Memcache text protocol server for UDepot.
//
// Implements the standard memcache text protocol (GET, SET, ADD, REPLACE,
// DELETE, INCR, DECR, APPEND, PREPEND, STATS, VERSION, QUIT) on top of
// any UDepot<IO> store instance.
//
// Value storage format (matches uDepot):
//   [user_data][expiry: int64_t, 8 bytes][flags: uint32_t, 4 bytes]
// The last 12 bytes of the stored value are metadata, invisible to the
// memcache client.
//
// Threading model: one accept thread, one handler thread per connection.
// Each handler thread runs a coroutine-based event loop for async I/O.
template <typename Store>
class MemcacheServer {
public:
    struct Config {
        std::string bind_addr = "127.0.0.1";
        uint16_t port = 11211;
        int backlog = 128;
    };

    explicit MemcacheServer(Store& store) : store_(store) {}
    ~MemcacheServer();

    MemcacheServer(const MemcacheServer&) = delete;
    MemcacheServer& operator=(const MemcacheServer&) = delete;

    int start(const Config& config);
    void stop();

    bool is_running() const noexcept {
        return running_.load(std::memory_order_acquire);
    }

    uint16_t port() const noexcept { return port_; }

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
    std::vector<std::thread> conn_threads_;
    std::atomic<uint64_t> bytes_stored_{0};

    void accept_loop();
    void handle_connection(int fd);
};

}  // namespace udepot

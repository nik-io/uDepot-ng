// Copyright (c) 2024-2026 Nikolas Ioannou
// SPDX-License-Identifier: BSD-3-Clause

#include "udepot/rcu.h"

#include <cassert>
#include <thread>

namespace udepot {

uint64_t Rcu::next_id() noexcept {
    static std::atomic<uint64_t> counter{1};
    return counter.fetch_add(1, std::memory_order_relaxed);
}

Rcu::Token Rcu::register_thread() noexcept {
    std::lock_guard<std::mutex> lock(register_mu_);
    uint32_t count = thread_count_.load(std::memory_order_relaxed);
    for (uint32_t i = 0; i < count; ++i) {
        if (!threads_[i].registered.load(std::memory_order_relaxed)) {
            threads_[i].registered.store(true, std::memory_order_relaxed);
            threads_[i].epoch.store(0, std::memory_order_relaxed);
            threads_[i].nesting = 0;
            return Token{i};
        }
    }
    assert(count < kMaxThreads && "too many concurrent RCU threads");
    threads_[count].registered.store(true, std::memory_order_relaxed);
    threads_[count].epoch.store(0, std::memory_order_relaxed);
    threads_[count].nesting = 0;
    thread_count_.store(count + 1, std::memory_order_release);
    return Token{count};
}

void Rcu::unregister_thread(Token t) noexcept {
    assert(t.valid());
    auto& ts = threads_[t.slot_];
    assert(ts.registered.load(std::memory_order_relaxed));
    assert(ts.nesting == 0 && "unregistering inside a critical section");
    ts.epoch.store(0, std::memory_order_release);
    ts.registered.store(false, std::memory_order_release);
}

void Rcu::read_lock(Token t) noexcept {
    assert(t.valid());
    auto& ts = threads_[t.slot_];
    if (ts.nesting++ == 0) {
        uint64_t e = global_epoch_.load(std::memory_order_relaxed);
        ts.epoch.store(e | 1, std::memory_order_release);
    }
}

void Rcu::read_unlock(Token t) noexcept {
    assert(t.valid());
    auto& ts = threads_[t.slot_];
    assert(ts.nesting > 0 && "read_unlock without matching read_lock");
    if (--ts.nesting == 0) {
        ts.epoch.store(0, std::memory_order_release);
    }
}

void Rcu::synchronize() noexcept {
    uint64_t target = global_epoch_.fetch_add(2, std::memory_order_acq_rel) + 2;
    uint32_t count = thread_count_.load(std::memory_order_acquire);
    for (uint32_t i = 0; i < count; ++i) {
        auto& ts = threads_[i];
        if (!ts.registered.load(std::memory_order_acquire)) continue;
        while (true) {
            uint64_t e = ts.epoch.load(std::memory_order_acquire);
            // Quiescent (0) or entered a new critical section after our
            // epoch bump (e >= target means the thread observed our new
            // epoch, so any prior critical section has ended).
            if (e == 0 || e >= target) break;
            std::this_thread::yield();
        }
    }
}

}  // namespace udepot

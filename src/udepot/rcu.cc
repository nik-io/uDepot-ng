#include "udepot/rcu.h"

#include <cassert>
#include <thread>

namespace udepot {

Rcu::Token Rcu::register_thread() noexcept {
    uint32_t slot = thread_count_.fetch_add(1, std::memory_order_relaxed);
    assert(slot < kMaxThreads && "too many RCU threads");
    auto& ts = threads_[slot];
    assert(!ts.registered && "slot already registered");
    ts.registered = true;
    ts.epoch.store(0, std::memory_order_relaxed);
    ts.nesting = 0;
    return Token{slot};
}

void Rcu::unregister_thread(Token t) noexcept {
    assert(t.valid());
    auto& ts = threads_[t.slot_];
    assert(ts.registered);
    assert(ts.nesting == 0 && "unregistering inside a critical section");
    ts.epoch.store(0, std::memory_order_release);
    ts.registered = false;
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
        if (!ts.registered) continue;
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

// Copyright (c) 2024-2026 Nikolas Ioannou
// SPDX-License-Identifier: BSD-3-Clause

#include "udepot/rcu.h"

#if defined(__linux__)
#include <linux/membarrier.h>
#include <sys/syscall.h>
#include <unistd.h>
#endif

#include <cerrno>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <thread>
#include <unordered_map>
#include <vector>

namespace udepot {

namespace {

// Live instances by id, so a thread exiting after an Rcu is destroyed never
// touches it. Taken only when an Rcu is created or destroyed and when a
// thread that used one exits.
std::mutex& live_mu() {
    static std::mutex mu;
    return mu;
}

std::unordered_map<uint64_t, Rcu*>& live() {
    static std::unordered_map<uint64_t, Rcu*> map;
    return map;
}

// Set once, before the first Rcu exists; read on every read_lock.
std::atomic<bool> g_reader_fence{true};

void init_barrier_mode() {
    static std::once_flag once;
    std::call_once(once, [] {
        const char* force = std::getenv("UDEPOT_RCU_READER_FENCE");
        bool fence = force && std::strcmp(force, "0") != 0;
#if defined(__linux__)
        if (!fence)
            fence = syscall(__NR_membarrier,
                            MEMBARRIER_CMD_REGISTER_PRIVATE_EXPEDITED,
                            0, 0) != 0;
#else
        fence = true;
#endif
        g_reader_fence.store(fence, std::memory_order_relaxed);
    });
}

inline void reader_barrier() noexcept {
    if (g_reader_fence.load(std::memory_order_relaxed))
        std::atomic_thread_fence(std::memory_order_seq_cst);
    else
        std::atomic_signal_fence(std::memory_order_seq_cst);
}

// A full barrier on the calling thread and on every thread that may be in a
// read-side critical section.
void heavy_barrier() noexcept {
    std::atomic_thread_fence(std::memory_order_seq_cst);
    if (g_reader_fence.load(std::memory_order_relaxed)) return;
#if defined(__linux__)
    if (syscall(__NR_membarrier, MEMBARRIER_CMD_PRIVATE_EXPEDITED, 0, 0) == 0)
        return;
    // Readers rely on this barrier; without it there is no grace period.
    std::fprintf(stderr, "rcu: membarrier failed: %s\n", std::strerror(errno));
    std::abort();
#endif
}

}  // namespace

// The calling thread's slot in each Rcu it has used. Released at thread exit.
struct ThreadSlots {
    struct Owned {
        uint64_t rcu_id;
        uint32_t slot;
    };
    Owned last{0, 0};  // most recently used; ids start at 1
    std::vector<Owned> owned;

    ~ThreadSlots() {
        for (const auto& o : owned)
            Rcu::release_slot_if_alive(o.rcu_id, o.slot);
    }
};

static thread_local ThreadSlots tl_slots;

// Drops from a thread's list the instances destroyed since it used them.
static void forget_destroyed(std::vector<ThreadSlots::Owned>& owned) {
    if (owned.empty()) return;
    std::lock_guard<std::mutex> lock(live_mu());
    std::erase_if(owned, [](const ThreadSlots::Owned& o) {
        return live().count(o.rcu_id) == 0;
    });
}

uint64_t Rcu::next_id() noexcept {
    static std::atomic<uint64_t> counter{1};
    return counter.fetch_add(1, std::memory_order_relaxed);
}

bool Rcu::reader_fence() noexcept {
    init_barrier_mode();
    return g_reader_fence.load(std::memory_order_relaxed);
}

Rcu::Rcu() {
    init_barrier_mode();
    std::lock_guard<std::mutex> lock(live_mu());
    live()[id_] = this;
}

Rcu::~Rcu() {
    // Run what is still queued, then stop the reclaimer.
    {
        std::lock_guard<std::mutex> lock(cb_mu_);
        cb_stop_ = true;
    }
    cb_cv_.notify_all();
    if (reclaimer_.joinable()) reclaimer_.join();

    std::lock_guard<std::mutex> lock(live_mu());
    live().erase(id_);
}

void Rcu::call(std::function<void()> fn) {
    std::lock_guard<std::mutex> lock(cb_mu_);
    cb_queue_.push_back(std::move(fn));
    ++cb_queued_;
    if (!reclaimer_.joinable())
        reclaimer_ = std::thread(&Rcu::reclaim_loop, this);
    cb_cv_.notify_all();
}

void Rcu::barrier() {
    std::unique_lock<std::mutex> lock(cb_mu_);
    const uint64_t target = cb_queued_;
    cb_cv_.wait(lock, [&] { return cb_done_ >= target; });
}

void Rcu::reclaim_loop() {
    std::unique_lock<std::mutex> lock(cb_mu_);
    for (;;) {
        cb_cv_.wait(lock, [&] { return cb_stop_ || !cb_queue_.empty(); });
        if (cb_queue_.empty()) return;  // stopping, nothing left
        auto batch = std::move(cb_queue_);
        cb_queue_.clear();
        lock.unlock();
        // One grace period covers the whole batch: each callback was queued
        // before this synchronize() began.
        synchronize();
        for (auto& fn : batch) fn();
        lock.lock();
        cb_done_ += batch.size();
        cb_cv_.notify_all();
    }
}

uint32_t Rcu::claim_slot() noexcept {
    std::lock_guard<std::mutex> lock(slots_mu_);
    for (uint32_t s = 0; s < kMaxThreads; ++s) {
        if (in_use_[s]) continue;
        in_use_[s] = true;
        // A reused slot keeps its counts: they only grow, and the previous
        // owner's last bumps happen-before this through slots_mu_.
        if (s >= slots_claimed_.load(std::memory_order_relaxed))
            slots_claimed_.store(s + 1, std::memory_order_relaxed);
        return s;
    }
    return kShared;
}


size_t Rcu::this_thread_instances() noexcept { return tl_slots.owned.size(); }

void Rcu::release_slot_if_alive(uint64_t id, uint32_t s) noexcept {
    std::lock_guard<std::mutex> lock(live_mu());
    auto it = live().find(id);
    if (it == live().end()) return;
    Rcu& rcu = *it->second;
    std::lock_guard<std::mutex> slots_lock(rcu.slots_mu_);
    rcu.in_use_[s] = false;
}

uint32_t Rcu::this_thread_slot() noexcept {
    auto& t = tl_slots;
    if (t.last.rcu_id == id_) return t.last.slot;
    for (const auto& o : t.owned) {
        if (o.rcu_id == id_) {
            t.last = o;
            return o.slot;
        }
    }
    uint32_t s = claim_slot();
    // The shared slot is not recorded as owned, so a later lookup that
    // misses the cache tries again to claim a slot of its own.
    if (s != kShared) {
        // A slot of this thread's own, once per instance: forget the
        // instances destroyed since (their slots went with them), so a
        // thread that outlives many stores keeps, and scans, only the live
        // ones. Never on the shared slot's path, which comes back here on
        // every switch between instances.
        forget_destroyed(t.owned);
        t.owned.push_back({id_, s});
    }
    t.last = {id_, s};
    return s;
}

uint32_t Rcu::read_lock() noexcept {
    uint32_t s = this_thread_slot();
    uint32_t idx = gp_idx_.load(std::memory_order_relaxed) & 1;
    auto& ctr = slot(s).lock[idx];
    if (s == kShared)
        ctr.fetch_add(1, std::memory_order_relaxed);
    else
        ctr.store(ctr.load(std::memory_order_relaxed) + 1,
                  std::memory_order_relaxed);
    reader_barrier();
    return idx;
}

void Rcu::read_unlock(uint32_t idx) noexcept {
    uint32_t s = this_thread_slot();
    auto& ctr = slot(s).unlock[idx];
    if (s == kShared)
        ctr.fetch_add(1, std::memory_order_release);
    else
        ctr.store(ctr.load(std::memory_order_relaxed) + 1,
                  std::memory_order_release);
}

bool Rcu::drained(uint32_t idx) const noexcept {
    // Unlocks first, then locks: with the barrier between them, every
    // unlock counted has its lock counted too, so equal sums mean no reader
    // of this index is still inside its critical section.
    uint64_t unlocks = shared_.unlock[idx].load(std::memory_order_acquire);
    for (const auto& s : slots_)
        unlocks += s.unlock[idx].load(std::memory_order_acquire);
    heavy_barrier();
    uint64_t locks = shared_.lock[idx].load(std::memory_order_relaxed);
    for (const auto& s : slots_)
        locks += s.lock[idx].load(std::memory_order_relaxed);
    return locks == unlocks;
}

void Rcu::synchronize() noexcept {
    std::lock_guard<std::mutex> lock(gp_mu_);
    // Order the caller's updates (an unpublished pointer, a cleared flag)
    // before the scans below, against readers' compiler-only barriers.
    heavy_barrier();

    // Readers that loaded the index before the previous flip may still be
    // counting on the other side; drain it before flipping back onto it.
    uint32_t cur = gp_idx_.load(std::memory_order_relaxed);
    while (!drained((cur + 1) & 1)) std::this_thread::yield();

    gp_idx_.store(cur + 1, std::memory_order_relaxed);
    heavy_barrier();

    while (!drained(cur & 1)) std::this_thread::yield();
    heavy_barrier();
}

}  // namespace udepot

// Copyright (c) 2024-2026 Nikolas Ioannou
// SPDX-License-Identifier: BSD-3-Clause

#pragma once

#include <array>
#include <atomic>
#include <condition_variable>
#include <cstdint>
#include <functional>
#include <mutex>
#include <thread>
#include <vector>

namespace udepot {

// Userspace RCU with per-thread counters (the sleepable-RCU algorithm).
//
// A read-side critical section bumps a lock counter for the current
// grace-period index on the slot of the thread that enters it, and an unlock
// counter for the same index on the slot of the thread that leaves it. The
// two may differ: a coroutine routinely starts on its caller's thread and
// finishes on an I/O backend's poller. Each slot is written only by the
// thread that owns it, so both bumps are a plain load and store; the read
// path has no atomic read-modify-write and writes no shared cache line.
//
// synchronize() flips the index and waits until, summed over all slots, the
// old index's unlocks equal its locks.
//
// Ordering. A reader's lock bump must be visible before its protected loads
// (StoreLoad, which x86 and arm64 both reorder). On Linux the writer supplies
// that barrier with membarrier(), so readers need only a compiler barrier,
// the trade srcu_read_lock_lite makes with synchronize_rcu(). Where
// membarrier is unavailable (macOS, old kernels, or forced with
// UDEPOT_RCU_READER_FENCE=1 for testing) readers issue a full fence instead.
// Unlock bumps are release stores either way.
class Rcu {
public:
    // Threads beyond this many share one slot updated with atomic RMW:
    // slower, never incorrect.
    static constexpr uint32_t kMaxThreads = 256;

    // RAII read-side critical section. May be destroyed on another thread.
    class ReadGuard {
    public:
        explicit ReadGuard(Rcu& rcu) noexcept
            : rcu_(rcu), idx_(rcu.read_lock()) {}
        ~ReadGuard() { rcu_.read_unlock(idx_); }

        ReadGuard(const ReadGuard&) = delete;
        ReadGuard& operator=(const ReadGuard&) = delete;

    private:
        Rcu& rcu_;
        uint32_t idx_;
    };

    Rcu();
    ~Rcu();

    Rcu(const Rcu&) = delete;
    Rcu& operator=(const Rcu&) = delete;

    uint64_t id() const noexcept { return id_; }

    // Enter a read-side critical section on the calling thread. Returns the
    // index to pass to read_unlock, which may be called on any thread.
    uint32_t read_lock() noexcept;
    void read_unlock(uint32_t idx) noexcept;

    // Wait until every read-side critical section that began before this
    // call has ended. May spin.
    void synchronize() noexcept;

    // Run fn once a grace period has elapsed (call_rcu): every read-side
    // section that began before this call has ended. Never waits; callbacks
    // run on a reclaimer thread, started on first use, which batches them
    // behind one synchronize(). Callable from inside a read section.
    void call(std::function<void()> fn);

    // Wait until every callback queued before this call has run
    // (rcu_barrier). Must not be called inside a read-side section.
    void barrier();

    // Slots ever claimed (high-water mark).
    uint32_t thread_count() const noexcept {
        return slots_claimed_.load(std::memory_order_relaxed);
    }

    // Whether readers use a full fence because membarrier is unavailable.
    static bool reader_fence() noexcept;

    // Rcu instances the calling thread holds a slot in. For tests.
    static size_t this_thread_instances() noexcept;

private:
    struct alignas(64) Slot {
        std::atomic<uint64_t> lock[2] = {};
        std::atomic<uint64_t> unlock[2] = {};
    };

    static constexpr uint32_t kShared = kMaxThreads;

    Slot& slot(uint32_t s) noexcept {
        return s == kShared ? shared_ : slots_[s];
    }
    uint32_t this_thread_slot() noexcept;
    uint32_t claim_slot() noexcept;
    bool drained(uint32_t idx) const noexcept;
    static void release_slot_if_alive(uint64_t id, uint32_t s) noexcept;
    static bool alive(uint64_t id) noexcept;
    static uint64_t next_id() noexcept;

    friend struct ThreadSlots;

    std::array<Slot, kMaxThreads> slots_{};
    Slot shared_{};  // updated with fetch_add by threads without a slot
    alignas(64) std::atomic<uint32_t> gp_idx_{0};
    std::atomic<uint32_t> slots_claimed_{0};
    std::array<bool, kMaxThreads> in_use_{};  // guarded by slots_mu_
    std::mutex slots_mu_;
    std::mutex gp_mu_;  // serializes synchronize()

    // Deferred callbacks (call/barrier).
    void reclaim_loop();
    std::mutex cb_mu_;
    std::condition_variable cb_cv_;
    std::vector<std::function<void()>> cb_queue_;  // cb_mu_
    uint64_t cb_queued_ = 0;                       // cb_mu_
    uint64_t cb_done_ = 0;                         // cb_mu_
    bool cb_stop_ = false;                         // cb_mu_
    std::thread reclaimer_;                        // started under cb_mu_
    uint64_t id_ = next_id();
};

}  // namespace udepot

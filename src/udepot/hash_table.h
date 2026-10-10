// Copyright (c) 2024-2026 Nikolas Ioannou
// SPDX-License-Identifier: BSD-3-Clause

#pragma once

#include <algorithm>
#include <atomic>
#include <cassert>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <memory>
#include <mutex>
#include <utility>
#include <vector>

#include "udepot/hash_entry.h"

namespace udepot {

// Hopscotch hash table with lock-free reads and stripe-locked writes.
//
// Each slot is an atomic<uint64_t> holding a packed HashEntry. Readers scan
// a bucket's neighborhood (kHopRange slots) without taking any lock.
//
// Writers lock stripes, as uDepot's uDepotMap does. A write for a bucket
// may look for a free slot up to kMaxDisplace neighborhoods past it and
// move entries forward to bring that slot into the neighborhood; it touches
// at most the range [bucket - kHopRange, bucket + kHopRange * (kMaxDisplace
// + 1)). Stripes are at least that long, so a write takes one or two
// adjacent stripe locks, in ascending order. Entries only ever move to a
// higher slot, written there before being cleared from the lower one, so a
// reader scanning upward always sees an entry in at least one position.
class HashTable {
public:
    // Upper bound on stripes per table (docs/architecture.md).
    static constexpr uint32_t kMaxStripes = 1024;
    // How many neighborhoods past its own a write searches for a free slot
    // (uDepot's _UDEPOT_HOP_MAX_BUCKET_DISPLACE).
    static constexpr uint64_t kMaxDisplace = 64;

    // Construct a hash table with 2^index_bits buckets.
    explicit HashTable(uint32_t index_bits);
    ~HashTable();

    HashTable(const HashTable&) = delete;
    HashTable& operator=(const HashTable&) = delete;

    // Lock-free lookup of the next entry whose tag matches, at a
    // neighborhood offset >= start_offset. Deleted entries are skipped
    // unless include_deleted. The caller must verify the key on disk (the
    // tag is a probabilistic filter). Returns an empty entry if none.
    HashEntry lookup(uint64_t hash, uint32_t start_offset = 0,
                     bool include_deleted = false) const noexcept;

    // The stripe locks covering every slot a write for `hash` can touch.
    // Hold across a check-then-act sequence (lookup, then *_locked). Never
    // hold across a co_await: a coroutine can resume on another thread.
    class WriteLock {
    public:
        WriteLock() = default;
        WriteLock(std::mutex* first, std::mutex* second);
        WriteLock(WriteLock&&) noexcept = default;
        WriteLock& operator=(WriteLock&&) noexcept = default;

    private:
        std::unique_lock<std::mutex> first_;
        std::unique_lock<std::mutex> second_;
    };
    WriteLock lock_for(uint64_t hash) {
        auto [first, last] = stripes_for(hash);
        return lock_stripes(first, last);
    }

    // The stripes a write for `hash` locks: [first, last], last == first or
    // first + 1. Tables with the same index_bits share stripe geometry, so
    // a resize locks the same stripes in the old and the new tables.
    std::pair<uint64_t, uint64_t> stripes_for(uint64_t hash) const noexcept;
    WriteLock lock_stripes(uint64_t first, uint64_t last);

    uint64_t stripe_of_slot(uint64_t idx) const noexcept {
        return idx / slots_per_stripe_;
    }
    uint64_t stripe_begin(uint64_t stripe) const noexcept {
        return stripe * slots_per_stripe_;
    }
    uint64_t stripe_end(uint64_t stripe) const noexcept {
        return std::min((stripe + 1) * slots_per_stripe_, total_slots());
    }
    uint64_t bucket_of(uint64_t hash) const noexcept {
        return hash_to_bucket(hash);
    }

    // The scans behind lookup() and entry_at(), with each neighborhood slot
    // read through slot(idx) (acquire). A resizing directory reads each
    // slot from the old or the new table, by its stripe's migration.
    template <typename SlotFn>
    static HashEntry scan(uint64_t bucket, uint8_t tag, uint32_t start_offset,
                          bool include_deleted, SlotFn&& slot) {
        for (uint32_t i = start_offset; i < HashEntry::kHopRange; ++i) {
            HashEntry entry = slot(bucket + i);
            if (entry.empty()) continue;
            if (entry.bucket_offset() != i) continue;
            if (entry.key_tag() != tag) continue;
            if (entry.deleted() && !include_deleted) continue;
            return entry;
        }
        return HashEntry{};
    }
    template <typename SlotFn>
    static HashEntry scan_pba(uint64_t bucket, uint8_t tag, uint64_t pba,
                              SlotFn&& slot) {
        for (uint32_t i = 0; i < HashEntry::kHopRange; ++i) {
            HashEntry entry = slot(bucket + i);
            if (!entry.empty() && entry.bucket_offset() == i &&
                entry.key_tag() == tag && entry.pba() == pba)
                return entry;
        }
        return HashEntry{};
    }

    // Insert a live entry. Returns 0, or -1 if no free slot is within reach
    // (the directory needs to grow).
    int insert(uint64_t hash, uint16_t kv_size, uint64_t pba);

    // Replace the entry (live or deleted) at (hash, old_pba) in place.
    // new_kv_size 0 makes it a deleted entry pointing at a tombstone.
    // Returns false if old_pba is no longer present.
    bool update(uint64_t hash, uint64_t old_pba,
                uint16_t new_kv_size, uint64_t new_pba);

    // Clear the slot holding (hash, pba). Returns true if found.
    bool remove(uint64_t hash, uint64_t pba);

    // Lock-free: the entry (live or deleted) for (hash, pba), or empty.
    HashEntry entry_at(uint64_t hash, uint64_t pba) const noexcept {
        uint64_t idx = find_pba(hash, pba);
        return idx < total_slots() ? load_slot(idx) : HashEntry{};
    }

    // As above, for callers already holding lock_for(hash).
    int insert_locked(uint64_t hash, uint16_t kv_size, uint64_t pba);
    bool update_locked(uint64_t hash, uint64_t old_pba,
                       uint16_t new_kv_size, uint64_t new_pba);
    bool remove_locked(uint64_t hash, uint64_t pba);

    uint32_t index_bits() const noexcept { return index_bits_; }
    uint64_t num_buckets() const noexcept { return num_buckets_; }
    uint64_t num_stripes() const noexcept { return num_stripes_; }

    // Direct slot access: persisting and restoring the index, and
    // migrating a stripe during a resize.
    HashEntry load_slot(uint64_t idx, std::memory_order order =
                                          std::memory_order_relaxed) const
        noexcept {
        return HashEntry::load(slots_[idx], order);
    }

    // Set a slot's raw entry: restoring a persisted table (nothing reads or
    // writes it meanwhile), or migrating a stripe into it during a resize
    // (published to readers by the stripe's migrated flag).
    void restore_slot(uint64_t idx, uint64_t raw) noexcept {
        slots_[idx].store(raw, std::memory_order_relaxed);
    }

    uint64_t total_slots() const noexcept {
        return num_buckets_ + HashEntry::kHopRange;
    }

private:
    struct alignas(64) Stripe {
        std::mutex mu;
    };

    uint32_t index_bits_;
    uint64_t num_buckets_;
    uint64_t bucket_mask_;
    uint64_t num_stripes_;
    uint64_t slots_per_stripe_;

    std::unique_ptr<std::atomic<uint64_t>[]> slots_;
    std::unique_ptr<Stripe[]> stripes_;

    uint64_t hash_to_bucket(uint64_t hash) const noexcept {
        return hash & bucket_mask_;
    }

    // First free slot in the search window of `bucket`, or total_slots().
    uint64_t find_free_slot(uint64_t bucket) const noexcept;

    // Move an entry from below free_idx into it, bringing the free slot
    // closer to target. Returns false if no entry can move.
    bool displace_toward(uint64_t target, uint64_t& free_idx);

    // Slot index of the entry for (hash, pba), or total_slots().
    uint64_t find_pba(uint64_t hash, uint64_t pba) const noexcept;
};

}  // namespace udepot

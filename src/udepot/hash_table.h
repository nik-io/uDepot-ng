// Copyright (c) 2024-2026 Nikolas Ioannou
// SPDX-License-Identifier: BSD-3-Clause

#pragma once

#include <atomic>
#include <cassert>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <memory>
#include <mutex>
#include <vector>

#include "udepot/hash_entry.h"
#include "udepot/table_region.h"

namespace udepot {

// Hopscotch hash table with lock-free reads and stripe-locked writes.
//
// The table lives in a TableRegion, the mapping of its index segment, as
// uDepot's uDepotMap lives in its mmap'd directory segment: the slots start
// past the region's header, and their number follows from the region's
// size (uDepotMap::restore: the largest power of two of buckets, plus one
// neighborhood, that fits between header and footer). A smaller table is a
// smaller segment.
//
// Each slot is a uint64_t holding a packed HashEntry, accessed atomically.
// Readers scan a bucket's neighborhood (kHopRange slots) without taking any
// lock.
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

    // A table over `region`, whose slots it takes as they are: a restored
    // table's, or a new one's to clear(). The region must hold at least
    // one bucket (index_bits_for() != 0).
    explicit HashTable(TableRegion region);
    ~HashTable();

    // log2 of the buckets a region of net_bytes holds, or 0 if it holds no
    // more than one neighborhood.
    static uint32_t index_bits_for(size_t net_bytes) noexcept;

    // Mark every slot unused (uDepot's uDepotMap::init: memset to -1).
    void clear() noexcept;

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
    WriteLock lock_for(uint64_t hash);

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

    // Direct slot access for directory rehash during grow.
    HashEntry load_slot(uint64_t idx) const noexcept {
        return HashEntry::load(slot(idx), std::memory_order_relaxed);
    }

    uint64_t total_slots() const noexcept {
        return num_buckets_ + HashEntry::kHopRange;
    }

    // Bytes from the region's start to the end of the slots.
    size_t used_bytes() const noexcept {
        return TableRegion::kMdBytes + total_slots() * sizeof(uint64_t);
    }

    const TableRegion& region() const noexcept { return region_; }
    // Give a table mapped without a segment its segment (crash recovery).
    void set_segment(uint64_t grain) noexcept { region_.set_grain(grain); }

private:
    struct alignas(64) Stripe {
        std::mutex mu;
    };

    TableRegion region_;
    uint32_t index_bits_;
    uint64_t num_buckets_;
    uint64_t bucket_mask_;
    uint64_t num_stripes_;
    uint64_t slots_per_stripe_;

    uint64_t* slots_;  // in region_, past its header
    std::unique_ptr<Stripe[]> stripes_;

    std::atomic_ref<uint64_t> slot(uint64_t idx) const noexcept {
        return std::atomic_ref<uint64_t>(slots_[idx]);
    }

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

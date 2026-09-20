#pragma once

#include <atomic>
#include <cassert>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <memory>
#include <mutex>

#include "udepot/hash_entry.h"

namespace udepot {

// Hopscotch hash table with lock-free reads and stripe-locked writes.
//
// Each slot is an atomic<uint64_t> holding a packed HashEntry. Readers
// scan the neighborhood without acquiring any lock. Writers take a stripe
// lock to serialize modifications to overlapping neighborhoods.
class HashTable {
public:
    static constexpr uint32_t kDefaultStripeLocks = 1024;

    // Construct a hash table with 2^index_bits buckets.
    explicit HashTable(uint32_t index_bits,
                       uint32_t num_stripe_locks = kDefaultStripeLocks);
    ~HashTable();

    HashTable(const HashTable&) = delete;
    HashTable& operator=(const HashTable&) = delete;

    // Lock-free lookup. Returns the matching entry, or an empty entry if
    // not found. The caller must verify the key on disk (the tag is a
    // probabilistic filter).
    HashEntry lookup(uint64_t hash) const noexcept;

    // Insert an entry under the stripe lock. Returns 0 on success, -1 if
    // the neighborhood is full (table needs to grow).
    int insert(uint64_t hash, uint16_t kv_size, uint64_t pba);

    // Remove the entry matching (hash, pba) under the stripe lock.
    // Returns true if found and removed.
    bool remove(uint64_t hash, uint64_t pba);

    uint32_t index_bits() const noexcept { return index_bits_; }
    uint64_t num_buckets() const noexcept { return num_buckets_; }

    // Direct slot access for directory rehash during grow.
    HashEntry load_slot(uint64_t idx) const noexcept {
        return HashEntry::load(slots_[idx], std::memory_order_relaxed);
    }

    uint64_t total_slots() const noexcept {
        return num_buckets_ + HashEntry::kHopRange;
    }

private:
    uint32_t index_bits_;
    uint64_t num_buckets_;
    uint64_t bucket_mask_;
    uint32_t num_stripe_locks_;

    std::unique_ptr<std::atomic<uint64_t>[]> slots_;
    std::unique_ptr<std::mutex[]> stripe_locks_;

    uint64_t hash_to_bucket(uint64_t hash) const noexcept {
        return hash & bucket_mask_;
    }

    uint32_t stripe_for_bucket(uint64_t bucket) const noexcept {
        return static_cast<uint32_t>(bucket % num_stripe_locks_);
    }

    // Find an empty slot within hop range of the target bucket, using
    // the hopscotch displacement chain. Returns the index of the empty
    // slot, or num_buckets_ if none found.
    uint64_t find_free_slot(uint64_t bucket) const noexcept;

    // Move an entry closer to the target bucket via displacement.
    // Returns true if a swap was made, false if stuck.
    bool displace_toward(uint64_t target, uint64_t& free_idx);
};

}  // namespace udepot

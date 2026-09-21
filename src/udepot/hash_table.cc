#include "udepot/hash_table.h"

#include <bit>

namespace udepot {

HashTable::HashTable(uint32_t index_bits, uint32_t num_stripe_locks)
    : index_bits_(index_bits),
      num_buckets_(1ULL << index_bits),
      bucket_mask_(num_buckets_ - 1),
      num_stripe_locks_(num_stripe_locks),
      slots_(std::make_unique<std::atomic<uint64_t>[]>(
          num_buckets_ + HashEntry::kHopRange)),
      stripe_locks_(std::make_unique<std::mutex[]>(num_stripe_locks)) {
    for (uint64_t i = 0; i < num_buckets_ + HashEntry::kHopRange; ++i) {
        slots_[i].store(HashEntry::kEmpty, std::memory_order_relaxed);
    }
}

HashTable::~HashTable() = default;

HashEntry HashTable::lookup(uint64_t hash, uint32_t start_offset) const noexcept {
    uint64_t bucket = hash_to_bucket(hash);
    uint8_t tag = hash_to_tag(hash);

    for (uint32_t i = start_offset; i < HashEntry::kHopRange; ++i) {
        uint64_t idx = bucket + i;
        HashEntry entry = HashEntry::load(slots_[idx]);
        if (entry.empty()) continue;
        if (entry.bucket_offset() != i) continue;
        if (entry.key_tag() != tag) continue;
        return entry;
    }
    return HashEntry{};
}

int HashTable::insert(uint64_t hash, uint16_t kv_size, uint64_t pba) {
    uint64_t bucket = hash_to_bucket(hash);
    uint8_t tag = hash_to_tag(hash);

    std::lock_guard<std::mutex> lock(stripe_locks_[stripe_for_bucket(bucket)]);

    // Find an empty slot, potentially displacing entries.
    uint64_t free_idx = find_free_slot(bucket);
    if (free_idx >= num_buckets_ + HashEntry::kHopRange) return -1;

    // Displace the empty slot toward our target bucket.
    while (free_idx - bucket >= HashEntry::kHopRange) {
        if (!displace_toward(bucket, free_idx)) return -1;
    }

    uint8_t offset = static_cast<uint8_t>(free_idx - bucket);
    HashEntry entry = HashEntry::make(offset, tag, kv_size, pba);
    HashEntry::store(slots_[free_idx], entry);
    return 0;
}

bool HashTable::remove(uint64_t hash, uint64_t pba) {
    uint64_t bucket = hash_to_bucket(hash);
    uint8_t tag = hash_to_tag(hash);

    std::lock_guard<std::mutex> lock(stripe_locks_[stripe_for_bucket(bucket)]);

    for (uint32_t i = 0; i < HashEntry::kHopRange; ++i) {
        uint64_t idx = bucket + i;
        HashEntry entry = HashEntry::load(slots_[idx],
                                          std::memory_order_relaxed);
        if (entry.empty()) continue;
        if (entry.bucket_offset() != i) continue;
        if (entry.key_tag() != tag) continue;
        if (entry.pba() != pba) continue;
        HashEntry::clear(slots_[idx]);
        return true;
    }
    return false;
}

uint64_t HashTable::find_free_slot(uint64_t bucket) const noexcept {
    uint64_t limit = num_buckets_ + HashEntry::kHopRange;
    for (uint64_t idx = bucket; idx < limit; ++idx) {
        if (slots_[idx].load(std::memory_order_relaxed) == HashEntry::kEmpty) {
            return idx;
        }
    }
    return limit;
}

bool HashTable::displace_toward(uint64_t target, uint64_t& free_idx) {
    // Look backward from free_idx for an entry whose home bucket is
    // closer to target and that can be moved to free_idx.
    for (uint32_t dist = HashEntry::kHopRange - 1; dist > 0; --dist) {
        if (free_idx < dist) continue;
        uint64_t candidate_idx = free_idx - dist;
        if (candidate_idx < target) break;

        HashEntry entry = HashEntry::load(slots_[candidate_idx],
                                          std::memory_order_relaxed);
        if (entry.empty()) continue;

        uint64_t home = candidate_idx - entry.bucket_offset();
        uint8_t new_offset =
            static_cast<uint8_t>(free_idx - home);
        if (new_offset >= HashEntry::kHopRange) continue;

        // Move: write entry at new position first, then clear old.
        HashEntry moved = HashEntry::make(new_offset, entry.key_tag(),
                                          entry.kv_size(), entry.pba());
        HashEntry::store(slots_[free_idx], moved);
        HashEntry::clear(slots_[candidate_idx]);

        free_idx = candidate_idx;
        return true;
    }
    return false;
}

}  // namespace udepot

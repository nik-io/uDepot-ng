// Copyright (c) 2024-2026 Nikolas Ioannou
// SPDX-License-Identifier: BSD-3-Clause

#include "udepot/hash_table.h"

#include <algorithm>
#include <memory>
#include <new>

namespace udepot {

namespace {

// A write searches [bucket, bucket + kSearchEnd) for a free slot and may
// move entries whose home is up to one neighborhood below its bucket, so it
// touches at most kWriteReach slots starting at bucket - kHopRange.
constexpr uint64_t kSearchEnd =
    HashEntry::kHopRange * (HashTable::kMaxDisplace + 1);
constexpr uint64_t kWriteReach = HashEntry::kHopRange + kSearchEnd;

}  // namespace

HashTable::WriteLock::WriteLock(std::mutex* first, std::mutex* second)
    : first_(*first) {
    if (second) second_ = std::unique_lock<std::mutex>(*second);
}

HashTable::HashTable(uint32_t index_bits, Uninitialized)
    : index_bits_(index_bits),
      num_buckets_(1ULL << index_bits),
      bucket_mask_(num_buckets_ - 1),
      slots_(static_cast<std::atomic<uint64_t>*>(::operator new(
          (num_buckets_ + HashEntry::kHopRange) * sizeof(std::atomic<uint64_t>),
          std::align_val_t{64}))) {
    // As uDepot's uDepotMap::restore(): halve the stripe count until each
    // stripe covers a write's whole reach, so a write needs at most two.
    num_stripes_ = kMaxStripes;
    while (num_stripes_ > 1 &&
           (total_slots() + num_stripes_ - 1) / num_stripes_ < kWriteReach)
        num_stripes_ /= 2;
    slots_per_stripe_ = (total_slots() + num_stripes_ - 1) / num_stripes_;
    stripes_ = std::make_unique<Stripe[]>(num_stripes_);
}

HashTable::HashTable(uint32_t index_bits)
    : HashTable(index_bits, Uninitialized{}) {
    for (uint64_t s = 0; s < num_stripes_; ++s) init_stripe(s);
}

void HashTable::init_stripe(uint64_t stripe) noexcept {
    for (uint64_t i = stripe_begin(stripe); i < stripe_end(stripe); ++i)
        std::construct_at(&slots_[i], HashEntry::kEmpty);
}

HashTable::~HashTable() = default;

HashEntry HashTable::lookup(uint64_t hash, uint32_t start_offset,
                            bool include_deleted) const noexcept {
    return scan(hash_to_bucket(hash), hash_to_tag(hash), start_offset,
                include_deleted,
                [this](uint64_t idx) { return HashEntry::load(slots_[idx]); });
}

std::pair<uint64_t, uint64_t> HashTable::stripes_for(uint64_t hash) const
    noexcept {
    uint64_t bucket = hash_to_bucket(hash);
    uint64_t lo = bucket > HashEntry::kHopRange ? bucket - HashEntry::kHopRange
                                                : 0;
    uint64_t hi = std::min(bucket + kSearchEnd, total_slots()) - 1;
    uint64_t s1 = lo / slots_per_stripe_;
    uint64_t s2 = hi / slots_per_stripe_;
    assert(s2 - s1 <= 1);
    return {s1, s2};
}

HashTable::WriteLock HashTable::lock_stripes(uint64_t first, uint64_t last) {
    assert(last == first || last == first + 1);
    return WriteLock(&stripes_[first].mu,
                     last != first ? &stripes_[last].mu : nullptr);
}

int HashTable::insert(uint64_t hash, uint16_t kv_size, uint64_t pba) {
    auto lock = lock_for(hash);
    return insert_locked(hash, kv_size, pba);
}

bool HashTable::update(uint64_t hash, uint64_t old_pba,
                       uint16_t new_kv_size, uint64_t new_pba) {
    auto lock = lock_for(hash);
    return update_locked(hash, old_pba, new_kv_size, new_pba);
}

bool HashTable::remove(uint64_t hash, uint64_t pba) {
    auto lock = lock_for(hash);
    return remove_locked(hash, pba);
}

int HashTable::insert_locked(uint64_t hash, uint16_t kv_size, uint64_t pba) {
    uint64_t bucket = hash_to_bucket(hash);
    uint8_t tag = hash_to_tag(hash);

    uint64_t free_idx = find_free_slot(bucket);
    if (free_idx >= total_slots()) return -1;

    while (free_idx - bucket >= HashEntry::kHopRange) {
        if (!displace_toward(bucket, free_idx)) return -1;
    }

    uint8_t offset = static_cast<uint8_t>(free_idx - bucket);
    HashEntry::store(slots_[free_idx],
                     HashEntry::make(offset, tag, kv_size, pba));
    return 0;
}

uint64_t HashTable::find_pba(uint64_t hash, uint64_t pba) const noexcept {
    uint64_t bucket = hash_to_bucket(hash);
    uint8_t tag = hash_to_tag(hash);
    for (uint32_t i = 0; i < HashEntry::kHopRange; ++i) {
        HashEntry entry = HashEntry::load(slots_[bucket + i],
                                          std::memory_order_relaxed);
        if (entry.empty()) continue;
        if (entry.bucket_offset() != i) continue;
        if (entry.key_tag() != tag) continue;
        if (entry.pba() != pba) continue;
        return bucket + i;
    }
    return total_slots();
}

bool HashTable::update_locked(uint64_t hash, uint64_t old_pba,
                              uint16_t new_kv_size, uint64_t new_pba) {
    uint64_t idx = find_pba(hash, old_pba);
    if (idx >= total_slots()) return false;
    uint64_t bucket = hash_to_bucket(hash);
    HashEntry::store(slots_[idx],
                     HashEntry::make(static_cast<uint8_t>(idx - bucket),
                                     hash_to_tag(hash), new_kv_size, new_pba));
    return true;
}

bool HashTable::remove_locked(uint64_t hash, uint64_t pba) {
    uint64_t idx = find_pba(hash, pba);
    if (idx >= total_slots()) return false;
    HashEntry::clear(slots_[idx]);
    return true;
}

uint64_t HashTable::find_free_slot(uint64_t bucket) const noexcept {
    uint64_t limit = std::min(bucket + kSearchEnd, total_slots());
    for (uint64_t idx = bucket; idx < limit; ++idx) {
        if (HashEntry::load(slots_[idx], std::memory_order_relaxed).empty())
            return idx;
    }
    return total_slots();
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
        if (free_idx - home >= HashEntry::kHopRange) continue;

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

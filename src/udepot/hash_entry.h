// Copyright (c) 2024-2026 Nikolas Ioannou
// SPDX-License-Identifier: BSD-3-Clause

#pragma once

#include <atomic>
#include <cstdint>

namespace udepot {

// 8-byte packed hash entry — fits in a single atomic load on x86-64.
//
// Layout (64 bits), uDepot's HashEntry bitfields (declared bucket_offset,
// key_tag, kv_size, pba, so allocated from the least significant bit). The
// tables are persisted as they are in memory (index segments, store.cc), so
// this is an on-disk format:
//   [63:24] pba            (40 bits, physical block address in grains)
//   [23:13] kv_size        (11 bits, key+value size in grains)
//   [12: 5] key_tag        (8 bits, hash fingerprint for fast rejection)
//   [ 4: 0] bucket_offset  (5 bits, 0–31 hop distance)
//
// As in uDepot, a slot is unused when its pba is all ones, and an entry with
// kv_size 0 is deleted: it keeps the key's tombstone pba so the key's
// ordering against later writes survives, and crash recovery and GC can
// find it. A live entry is at least one grain.
class HashEntry {
public:
    static constexpr int kBucketOffsetBits = 5;
    static constexpr int kKeyTagBits = 8;
    static constexpr int kKvSizeBits = 11;
    static constexpr int kPbaBits = 40;

    static constexpr uint64_t kPbaMask = (1ULL << kPbaBits) - 1;
    static constexpr uint64_t kKvSizeMask = (1ULL << kKvSizeBits) - 1;
    static constexpr uint64_t kKeyTagMask = (1ULL << kKeyTagBits) - 1;
    static constexpr uint64_t kBucketOffsetMask =
        (1ULL << kBucketOffsetBits) - 1;

    static constexpr int kBucketOffsetShift = 0;
    static constexpr int kKeyTagShift = kBucketOffsetBits;
    static constexpr int kKvSizeShift = kBucketOffsetBits + kKeyTagBits;
    static constexpr int kPbaShift =
        kBucketOffsetBits + kKeyTagBits + kKvSizeBits;

    static constexpr uint32_t kHopRange = 1U << kBucketOffsetBits;  // 32
    static constexpr uint64_t kEmpty = ~uint64_t{0};
    static constexpr uint64_t kUnusedPba = kPbaMask;

    HashEntry() noexcept = default;

    static HashEntry make(uint8_t bucket_offset, uint8_t key_tag,
                          uint16_t kv_size, uint64_t pba) {
        uint64_t raw = 0;
        raw |= (static_cast<uint64_t>(bucket_offset) & kBucketOffsetMask)
               << kBucketOffsetShift;
        raw |= (static_cast<uint64_t>(key_tag) & kKeyTagMask) << kKeyTagShift;
        raw |= (static_cast<uint64_t>(kv_size) & kKvSizeMask) << kKvSizeShift;
        raw |= (pba & kPbaMask) << kPbaShift;
        return HashEntry{raw};
    }

    uint8_t bucket_offset() const noexcept {
        return (raw_ >> kBucketOffsetShift) & kBucketOffsetMask;
    }

    uint8_t key_tag() const noexcept {
        return (raw_ >> kKeyTagShift) & kKeyTagMask;
    }

    uint16_t kv_size() const noexcept {
        return (raw_ >> kKvSizeShift) & kKvSizeMask;
    }

    uint64_t pba() const noexcept {
        return (raw_ >> kPbaShift) & kPbaMask;
    }

    bool empty() const noexcept { return pba() == kUnusedPba; }
    bool deleted() const noexcept { return !empty() && kv_size() == 0; }
    uint64_t raw() const noexcept { return raw_; }

    // Atomic access for lock-free reads.
    static HashEntry load(const std::atomic<uint64_t>& slot,
                          std::memory_order order = std::memory_order_acquire) {
        return HashEntry{slot.load(order)};
    }

    static void store(std::atomic<uint64_t>& slot, HashEntry entry,
                      std::memory_order order = std::memory_order_release) {
        slot.store(entry.raw_, order);
    }

    static void clear(std::atomic<uint64_t>& slot,
                      std::memory_order order = std::memory_order_release) {
        slot.store(kEmpty, order);
    }

    bool operator==(const HashEntry& other) const noexcept {
        return raw_ == other.raw_;
    }

private:
    uint64_t raw_ = kEmpty;
    explicit HashEntry(uint64_t raw) noexcept : raw_(raw) {}
};

static_assert(sizeof(HashEntry) == 8);

// Extract an 8-bit tag from a hash value.
inline uint8_t hash_to_tag(uint64_t hash) {
    return static_cast<uint8_t>(hash >> 56);
}

}  // namespace udepot

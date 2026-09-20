#pragma once

#include <atomic>
#include <cstdint>

namespace udepot {

// 8-byte packed hash entry — fits in a single atomic load on x86-64.
//
// Layout (64 bits):
//   [63:59] bucket_offset  (5 bits, 0–31 hop distance)
//   [58:51] key_tag        (8 bits, hash fingerprint for fast rejection)
//   [50:40] kv_size        (11 bits, key+value size in grains)
//   [39: 0] pba            (40 bits, physical block address in grains)
//
// An entry is empty when all 64 bits are zero.
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

    static constexpr int kPbaShift = 0;
    static constexpr int kKvSizeShift = kPbaBits;
    static constexpr int kKeyTagShift = kPbaBits + kKvSizeBits;
    static constexpr int kBucketOffsetShift =
        kPbaBits + kKvSizeBits + kKeyTagBits;

    static constexpr uint32_t kHopRange = 1U << kBucketOffsetBits;  // 32
    static constexpr uint64_t kEmpty = 0;

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

    bool empty() const noexcept { return raw_ == kEmpty; }
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

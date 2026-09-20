#include "udepot/store.h"

#include <cerrno>
#include <cstring>

#include "udepot/io/posix.h"

namespace udepot {

template <typename IO>
uint16_t UDepot<IO>::compute_crc16(const KvHeader& hdr,
                                   std::span<const uint8_t> key,
                                   std::span<const uint8_t> val) {
    uint16_t crc = 0xFFFF;

    auto update = [&crc](const uint8_t* data, size_t len) {
        for (size_t i = 0; i < len; ++i) {
            crc ^= static_cast<uint16_t>(data[i]) << 8;
            for (int j = 0; j < 8; ++j) {
                if (crc & 0x8000)
                    crc = (crc << 1) ^ 0x1021;
                else
                    crc <<= 1;
            }
        }
    };

    update(reinterpret_cast<const uint8_t*>(&hdr), sizeof(hdr));
    update(key.data(), key.size());
    update(val.data(), val.size());
    return crc;
}

template <typename IO>
UDepot<IO>::UDepot() = default;

template <typename IO>
UDepot<IO>::~UDepot() { close(); }

template <typename IO>
int UDepot<IO>::open(const StoreConfig& config) {
    grain_size_ = config.grain_size;
    total_grains_ = config.size / grain_size_;
    next_grain_.store(0, std::memory_order_relaxed);

    int rc = io_.open(config.path, config.size);
    if (rc != 0) return rc;

    rcu_token_ = rcu_.register_thread();
    directory_ = new Directory(rcu_, config.initial_tables, config.index_bits);
    return 0;
}

template <typename IO>
void UDepot<IO>::close() {
    delete directory_;
    directory_ = nullptr;
    if (rcu_token_.valid()) {
        rcu_.unregister_thread(rcu_token_);
        rcu_token_ = Rcu::Token{};
    }
    io_.close();
}

template <typename IO>
uint64_t UDepot<IO>::allocate_grains(uint64_t count) {
    uint64_t grain = next_grain_.fetch_add(count, std::memory_order_relaxed);
    if (grain + count > total_grains_) return UINT64_MAX;
    return grain;
}

template <typename IO>
void UDepot<IO>::invalidate_grains(uint64_t, uint64_t) {
    // Bump allocator: no-op. Salsa would reclaim these via GC.
}

template <typename IO>
CoroTask<int> UDepot<IO>::put(std::span<const uint8_t> key,
                              std::span<const uint8_t> val) {
    if (key.empty() || key.size() > UINT16_MAX)
        co_return -EINVAL;
    if (val.size() > UINT32_MAX)
        co_return -EINVAL;

    uint64_t hash = hash_key(key);
    uint64_t grains_needed = kv_total_grains(key.size(), val.size());

    if (grains_needed > HashEntry::kKvSizeMask)
        co_return -EINVAL;

    // Allocate space on device.
    uint64_t grain = allocate_grains(grains_needed);
    if (grain == UINT64_MAX) co_return -ENOSPC;

    // Build the on-disk entry.
    size_t total = grains_needed * grain_size_;
    IoBuffer buf = io_.alloc_buffer(total);
    if (!buf.data) {
        invalidate_grains(grain, grains_needed);
        co_return -ENOMEM;
    }

    auto* p = static_cast<uint8_t*>(buf.data);
    KvHeader hdr;
    hdr.key_size = static_cast<uint16_t>(key.size());
    hdr.val_size = static_cast<uint32_t>(val.size());
    hdr.timestamp = 0;

    std::memcpy(p, &hdr, sizeof(hdr));
    std::memcpy(p + sizeof(hdr), key.data(), key.size());
    std::memcpy(p + sizeof(hdr) + key.size(), val.data(), val.size());

    KvSuffix suffix;
    suffix.crc16 = compute_crc16(hdr, key, val);
    std::memcpy(p + sizeof(hdr) + key.size() + val.size(),
                &suffix, sizeof(suffix));

    // Zero any padding between the suffix and the end of the grain-aligned
    // region.
    size_t used = kv_total_bytes(key.size(), val.size());
    if (total > used)
        std::memset(p + used, 0, total - used);

    buf.length = total;

    // Write to device.
    ssize_t written = co_await io_.pwrite(buf.data, total,
                                          grain_to_offset(grain));
    if (written != static_cast<ssize_t>(total)) {
        invalidate_grains(grain, grains_needed);
        co_return (written < 0) ? static_cast<int>(written) : -EIO;
    }

    // Insert into hash directory under RCU.
    rcu_.read_lock(rcu_token_);

    int rc = directory_->insert(hash,
                                static_cast<uint16_t>(grains_needed),
                                grain);

    rcu_.read_unlock(rcu_token_);

    if (rc != 0) {
        invalidate_grains(grain, grains_needed);
        co_return -ENOSPC;
    }

    co_return 0;
}

template <typename IO>
CoroTask<int> UDepot<IO>::verify_key_at_pba(
    uint64_t pba, uint16_t kv_grains,
    std::span<const uint8_t> key, KvHeader* hdr_out) {

    size_t read_size = sizeof(KvHeader) + key.size();
    // Round up to grain boundary for the read.
    size_t aligned_size = ((read_size + grain_size_ - 1) / grain_size_) *
                          grain_size_;
    if (aligned_size > static_cast<size_t>(kv_grains) * grain_size_)
        aligned_size = static_cast<size_t>(kv_grains) * grain_size_;

    IoBuffer buf = io_.alloc_buffer(aligned_size);
    if (!buf.data) co_return -ENOMEM;

    ssize_t nread = co_await io_.pread(buf.data, aligned_size,
                                       grain_to_offset(pba));
    if (nread < static_cast<ssize_t>(read_size)) {
        co_return -EIO;
    }

    auto* p = static_cast<const uint8_t*>(buf.data);
    KvHeader hdr;
    std::memcpy(&hdr, p, sizeof(hdr));

    if (hdr.key_size != key.size()) co_return -ENOENT;

    if (std::memcmp(p + sizeof(hdr), key.data(), key.size()) != 0)
        co_return -ENOENT;

    if (hdr_out) *hdr_out = hdr;
    co_return 0;
}

template <typename IO>
CoroTask<int> UDepot<IO>::get(std::span<const uint8_t> key,
                              uint8_t* val_out, size_t val_buf_size,
                              size_t* val_size_out) {
    if (key.empty()) co_return -EINVAL;

    uint64_t hash = hash_key(key);
    uint8_t tag = hash_to_tag(hash);

    rcu_.read_lock(rcu_token_);

    HashEntry entry = directory_->lookup(hash);

    if (entry.empty()) {
        rcu_.read_unlock(rcu_token_);
        co_return -ENOENT;
    }

    if (entry.key_tag() != tag) {
        rcu_.read_unlock(rcu_token_);
        co_return -ENOENT;
    }

    uint64_t pba = entry.pba();
    uint16_t kv_grains = entry.kv_size();

    // Read the full KV entry from disk.
    size_t read_bytes = static_cast<size_t>(kv_grains) * grain_size_;
    IoBuffer buf = io_.alloc_buffer(read_bytes);
    if (!buf.data) {
        rcu_.read_unlock(rcu_token_);
        co_return -ENOMEM;
    }

    ssize_t nread = co_await io_.pread(buf.data, read_bytes,
                                       grain_to_offset(pba));
    rcu_.read_unlock(rcu_token_);

    if (nread < static_cast<ssize_t>(sizeof(KvHeader))) {
        co_return -EIO;
    }

    auto* p = static_cast<const uint8_t*>(buf.data);
    KvHeader hdr;
    std::memcpy(&hdr, p, sizeof(hdr));

    // Verify key match.
    if (hdr.key_size != key.size()) co_return -ENOENT;

    size_t entry_total = kv_total_bytes(hdr.key_size, hdr.val_size);
    if (entry_total > read_bytes) co_return -EIO;

    if (std::memcmp(p + sizeof(hdr), key.data(), key.size()) != 0)
        co_return -ENOENT;

    // Verify CRC.
    auto key_span = std::span<const uint8_t>(p + sizeof(hdr), hdr.key_size);
    auto val_span = std::span<const uint8_t>(
        p + sizeof(hdr) + hdr.key_size, hdr.val_size);

    KvSuffix suffix;
    std::memcpy(&suffix, p + sizeof(hdr) + hdr.key_size + hdr.val_size,
                sizeof(suffix));
    uint16_t expected = compute_crc16(hdr, key_span, val_span);
    if (suffix.crc16 != expected) co_return -EIO;

    // Copy value to caller's buffer.
    if (val_size_out) *val_size_out = hdr.val_size;

    if (val_out && val_buf_size > 0) {
        size_t to_copy = std::min(val_buf_size, static_cast<size_t>(hdr.val_size));
        std::memcpy(val_out, p + sizeof(hdr) + hdr.key_size, to_copy);
    }

    co_return 0;
}

template <typename IO>
CoroTask<int> UDepot<IO>::del(std::span<const uint8_t> key) {
    if (key.empty()) co_return -EINVAL;

    uint64_t hash = hash_key(key);
    uint8_t tag = hash_to_tag(hash);

    rcu_.read_lock(rcu_token_);

    HashEntry entry = directory_->lookup(hash);

    if (entry.empty() || entry.key_tag() != tag) {
        rcu_.read_unlock(rcu_token_);
        co_return -ENOENT;
    }

    uint64_t pba = entry.pba();
    uint16_t kv_grains = entry.kv_size();

    // Verify the key on disk before removing.
    int rc = co_await verify_key_at_pba(pba, kv_grains, key, nullptr);
    if (rc != 0) {
        rcu_.read_unlock(rcu_token_);
        co_return -ENOENT;
    }

    bool removed = directory_->remove(hash, pba);

    rcu_.read_unlock(rcu_token_);

    if (removed) {
        invalidate_grains(pba, kv_grains);
        co_return 0;
    }

    co_return -ENOENT;
}

template <typename IO>
CoroTask<int> UDepot<IO>::exists(std::span<const uint8_t> key,
                                 size_t* val_size_out) {
    if (key.empty()) co_return -EINVAL;

    uint64_t hash = hash_key(key);
    uint8_t tag = hash_to_tag(hash);

    rcu_.read_lock(rcu_token_);

    HashEntry entry = directory_->lookup(hash);

    if (entry.empty() || entry.key_tag() != tag) {
        rcu_.read_unlock(rcu_token_);
        co_return -ENOENT;
    }

    uint64_t pba = entry.pba();
    uint16_t kv_grains = entry.kv_size();

    KvHeader hdr;
    int rc = co_await verify_key_at_pba(pba, kv_grains, key, &hdr);

    rcu_.read_unlock(rcu_token_);

    if (rc != 0) co_return -ENOENT;

    if (val_size_out) *val_size_out = hdr.val_size;
    co_return 0;
}

// Explicit instantiation for PosixIO.
template class UDepot<PosixIO>;

}  // namespace udepot

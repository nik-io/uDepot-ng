#include "udepot/store.h"

#include <array>
#include <cerrno>
#include <cstdio>
#include <cstdlib>
#include <cstring>

#include "udepot/io/aio.h"
#include "udepot/io/posix.h"

#include "frontends/usalsa++/Scm.hh"
#include "frontends/usalsa++/SalsaMD.hh"

namespace udepot {

// CRC-CCITT (0x1021) lookup table, computed at compile time.
static constexpr auto kCrc16Table = [] {
    std::array<uint16_t, 256> t{};
    for (int i = 0; i < 256; ++i) {
        uint16_t crc = static_cast<uint16_t>(i) << 8;
        for (int j = 0; j < 8; ++j)
            crc = (crc & 0x8000) ? (crc << 1) ^ 0x1021 : crc << 1;
        t[i] = crc;
    }
    return t;
}();

static uint16_t crc16_update(uint16_t crc, const uint8_t* data, size_t len) {
    for (size_t i = 0; i < len; ++i)
        crc = kCrc16Table[((crc >> 8) ^ data[i]) & 0xFF] ^ (crc << 8);
    return crc;
}

template <typename IO>
uint16_t UDepot<IO>::compute_crc16(const KvHeader& hdr) {
    uint16_t crc = 0xFFFF;
    crc = crc16_update(crc, reinterpret_cast<const uint8_t*>(&hdr),
                       sizeof(hdr));
    return crc;
}

template <typename IO>
UDepot<IO>::UDepot() = default;

template <typename IO>
UDepot<IO>::~UDepot() { close(); }

template <typename IO>
Rcu::Token UDepot<IO>::thread_token() {
    struct TokenEntry {
        Rcu* rcu;
        Rcu::Token token;
    };
    struct TokenStore {
        std::vector<TokenEntry> entries;
        ~TokenStore() {
            for (auto& e : entries)
                if (e.token.valid()) e.rcu->unregister_thread(e.token);
        }
    };
    thread_local TokenStore store;
    for (auto& e : store.entries) {
        if (e.rcu == &rcu_) return e.token;
    }
    auto tok = rcu_.register_thread();
    store.entries.push_back({&rcu_, tok});
    return tok;
}

static inline uint64_t align_up(uint64_t val, uint64_t align) {
    return (val + align - 1) / align * align;
}

template <typename IO>
int UDepot<IO>::open(const StoreConfig& config) {
    grain_size_ = config.grain_size;
    total_grains_ = config.size / grain_size_;

    int rc = io_.open(config.path, config.size);
    if (rc != 0) return rc;

    directory_ = new Directory(rcu_, config.initial_tables, config.index_bits);

    // Salsa initialization — matches uDepot's init_local().
    seg_md_grains_ = align_up(sizeof(salsa::salsa_seg_md), grain_size_) /
                     grain_size_;

    uint64_t segment_size = config.segment_size;
    if (segment_size == 0)
        segment_size = (1ULL << 29) / grain_size_ + 2;

    // Purging GC (type=1) for v0: entries in reclaimed segments are
    // dropped rather than relocated.  Matches uDepot's MC variants.
    static constexpr uint32_t kGcType = 1;
    static constexpr uint32_t kGcLowWm = 20;
    static constexpr uint32_t kGcHighWm = 40;

    // Halving retry loop (matches uDepot): if the segment size is too
    // large for the device, halve and try again.
    while (true) {
        char argv_buf[256];
        snprintf(argv_buf, sizeof(argv_buf),
                 "scm_dev= dev_size=%lu grain_size=%u"
                 " segment_size=%lu gc_type=%u gc_low_wm=%u gc_high_wm=%u"
                 " gc_thread_nr=1 simulation=1",
                 static_cast<unsigned long>(config.size),
                 grain_size_,
                 static_cast<unsigned long>(segment_size),
                 kGcType, kGcLowWm, kGcHighWm);

        char* argv[16];
        int argc = 0;
        char* token = strtok(argv_buf, " ");
        while (token && argc < 15) {
            argv[argc++] = token;
            token = strtok(nullptr, " ");
        }

        scm_ = new salsa::Scm();
        rc = scm_->init(argc, argv, config.overprovision);
        if (rc == 0) break;

        delete scm_;
        scm_ = nullptr;
        segment_size /= 2;
        if (segment_size <= 1) {
            delete directory_;
            directory_ = nullptr;
            io_.close();
            return -EINVAL;
        }
    }

    // Initialize our SalsaCtlr with 1 stream, 1 relocation stream.
    rc = salsa::SalsaCtlr::init(scm_, seg_md_grains_, 1, 1);
    if (rc != 0) {
        delete scm_;
        scm_ = nullptr;
        delete directory_;
        directory_ = nullptr;
        io_.close();
        return -rc;
    }

    rc = scm_->init_threads();
    if (rc != 0) {
        salsa::SalsaCtlr::shutdown();
        delete scm_;
        scm_ = nullptr;
        delete directory_;
        directory_ = nullptr;
        io_.close();
        return -rc;
    }

    return 0;
}

template <typename IO>
void UDepot<IO>::close() {
    if (scm_) {
        scm_->exit_threads();
        salsa::SalsaCtlr::shutdown();
        if (gc_rcu_token_.valid()) {
            rcu_.unregister_thread(gc_rcu_token_);
            gc_rcu_token_ = Rcu::Token{};
        }
        delete scm_;
        scm_ = nullptr;
    }

    delete directory_;
    directory_ = nullptr;
    io_.close();
}

template <typename IO>
uint64_t UDepot<IO>::allocate_grains(uint64_t count) {
    u64 grain_out = 0;
    int rc = salsa::SalsaCtlr::allocate_grains(
        static_cast<u64>(count), &grain_out);
    if (rc != 0) return UINT64_MAX;
    return grain_out;
}

template <typename IO>
void UDepot<IO>::invalidate_grains(uint64_t grain, uint64_t count) {
    salsa::SalsaCtlr::invalidate_grains(
        static_cast<u64>(grain), static_cast<u64>(count), false);
}

// GC callback — called from salsa's GC thread when it reclaims a segment.
// Purging GC: walk the segment's KV entries and remove them from the
// hash directory so no dangling references remain.
template <typename IO>
int UDepot<IO>::gc_callback(u64 grain_start, u64 grain_nr) {
    if (!gc_rcu_token_.valid())
        gc_rcu_token_ = rcu_.register_thread();

    // Net segment excludes the per-segment metadata grains at the tail.
    uint64_t end_grain = grain_start + grain_nr - seg_md_grains_;
    uint64_t grain = grain_start;

    while (grain < end_grain) {
        // Read the first grain to get the KvHeader.
        IoBuffer buf = io_.alloc_buffer(grain_size_);
        if (!buf.data) break;

        ssize_t nread = io_.pread(buf.data, grain_size_,
                                  grain_to_offset(grain)).run_sync();
        if (nread < static_cast<ssize_t>(sizeof(KvHeader))) {
            grain++;
            continue;
        }

        auto* p = static_cast<const uint8_t*>(buf.data);
        KvHeader hdr;
        std::memcpy(&hdr, p, sizeof(hdr));

        if (hdr.key_size == 0) {
            grain++;
            continue;
        }

        uint64_t entry_grains = kv_total_grains(hdr.key_size, hdr.val_size);
        if (entry_grains == 0 || grain + entry_grains > end_grain) {
            grain++;
            continue;
        }

        // Read the key for hashing.  Most keys fit in the first grain.
        const uint8_t* key_data = nullptr;
        IoBuffer key_buf{};
        size_t key_end = sizeof(KvHeader) + hdr.key_size;

        if (key_end <= static_cast<size_t>(grain_size_)) {
            key_data = p + sizeof(KvHeader);
        } else {
            size_t aligned = ((key_end + grain_size_ - 1) / grain_size_) *
                             grain_size_;
            key_buf = io_.alloc_buffer(aligned);
            if (!key_buf.data) {
                grain += entry_grains;
                continue;
            }
            nread = io_.pread(key_buf.data, aligned,
                              grain_to_offset(grain)).run_sync();
            if (nread < static_cast<ssize_t>(key_end)) {
                grain += entry_grains;
                continue;
            }
            key_data = static_cast<const uint8_t*>(key_buf.data) +
                       sizeof(KvHeader);
        }

        uint64_t hash = CityHash64(
            reinterpret_cast<const char*>(key_data), hdr.key_size);

        rcu_.read_lock(gc_rcu_token_);
        directory_->remove(hash, grain);
        rcu_.read_unlock(gc_rcu_token_);

        grain += entry_grains;
    }

    return 0;
}

// Segment metadata callback — called when salsa allocates a new segment.
// No crash recovery in v0, so this is a no-op.
template <typename IO>
void UDepot<IO>::seg_md_callback(u64, u64) {}

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
    suffix.crc16 = compute_crc16(hdr);
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

    // Lookup-before-write: check if the key already exists in the
    // directory and update in place if so (upsert semantics, matching
    // legacy uDepot's local_put_mbuff / lookup_mbuff_put).
    Rcu::Token tok = thread_token();
    rcu_.read_lock(tok);

    uint64_t old_pba = UINT64_MAX;
    uint16_t old_kv_grains = 0;

    for (uint32_t start = 0; ; ) {
        HashEntry entry = directory_->lookup(hash, start);
        if (entry.empty()) break;

        start = entry.bucket_offset() + 1;

        int vrc = co_await verify_key_at_pba(
            entry.pba(), entry.kv_size(), key, nullptr);
        if (vrc == 0) {
            old_pba = entry.pba();
            old_kv_grains = entry.kv_size();
            break;
        }
    }

    int rc;
    if (old_pba != UINT64_MAX) {
        // Key exists — atomically update the directory entry.
        bool updated = directory_->update(
            hash, old_pba,
            static_cast<uint16_t>(grains_needed), grain);
        if (updated) {
            rcu_.read_unlock(tok);
            invalidate_grains(old_pba, old_kv_grains);
            co_return 0;
        }
        // Entry was concurrently removed — fall through to insert.
    }

    rc = directory_->insert(hash,
                            static_cast<uint16_t>(grains_needed),
                            grain);

    rcu_.read_unlock(tok);

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
    Rcu::Token tok = thread_token();

    rcu_.read_lock(tok);

    // Iterate through all tag-matching entries to handle collisions.
    for (uint32_t start = 0; ; ) {
        HashEntry entry = directory_->lookup(hash, start);
        if (entry.empty()) break;

        start = entry.bucket_offset() + 1;

        uint64_t pba = entry.pba();
        uint16_t kv_grains = entry.kv_size();

        size_t read_bytes = static_cast<size_t>(kv_grains) * grain_size_;
        IoBuffer buf = io_.alloc_buffer(read_bytes);
        if (!buf.data) {
            rcu_.read_unlock(tok);
            co_return -ENOMEM;
        }

        ssize_t nread = co_await io_.pread(buf.data, read_bytes,
                                           grain_to_offset(pba));
        if (nread < static_cast<ssize_t>(sizeof(KvHeader)))
            continue;

        auto* p = static_cast<const uint8_t*>(buf.data);
        KvHeader hdr;
        std::memcpy(&hdr, p, sizeof(hdr));

        if (hdr.key_size != key.size()) continue;

        size_t entry_total = kv_total_bytes(hdr.key_size, hdr.val_size);
        if (entry_total > read_bytes) continue;

        if (std::memcmp(p + sizeof(hdr), key.data(), key.size()) != 0)
            continue;

#ifndef NDEBUG
        // Debug-only CRC verification (matches uDepot's _UDEPOT_DATA_DEBUG_VERIFY).
        {
            KvSuffix suffix;
            std::memcpy(&suffix, p + sizeof(hdr) + hdr.key_size + hdr.val_size,
                        sizeof(suffix));
            uint16_t expected = compute_crc16(hdr);
            if (suffix.crc16 != expected) {
                rcu_.read_unlock(tok);
                co_return -EIO;
            }
        }
#endif

        if (val_size_out) *val_size_out = hdr.val_size;
        if (val_out && val_buf_size > 0) {
            size_t to_copy = std::min(val_buf_size,
                                      static_cast<size_t>(hdr.val_size));
            std::memcpy(val_out, p + sizeof(hdr) + hdr.key_size, to_copy);
        }

        rcu_.read_unlock(tok);
        co_return 0;
    }

    rcu_.read_unlock(tok);
    co_return -ENOENT;
}

template <typename IO>
CoroTask<int> UDepot<IO>::del(std::span<const uint8_t> key) {
    if (key.empty()) co_return -EINVAL;

    uint64_t hash = hash_key(key);
    Rcu::Token tok = thread_token();

    rcu_.read_lock(tok);

    for (uint32_t start = 0; ; ) {
        HashEntry entry = directory_->lookup(hash, start);
        if (entry.empty()) break;

        start = entry.bucket_offset() + 1;

        uint64_t pba = entry.pba();
        uint16_t kv_grains = entry.kv_size();

        int rc = co_await verify_key_at_pba(pba, kv_grains, key, nullptr);
        if (rc != 0) continue;

        bool removed = directory_->remove(hash, pba);

        rcu_.read_unlock(tok);

        if (removed) {
            invalidate_grains(pba, kv_grains);
            co_return 0;
        }

        co_return -ENOENT;
    }

    rcu_.read_unlock(tok);
    co_return -ENOENT;
}

template <typename IO>
CoroTask<int> UDepot<IO>::exists(std::span<const uint8_t> key,
                                 size_t* val_size_out) {
    if (key.empty()) co_return -EINVAL;

    uint64_t hash = hash_key(key);
    Rcu::Token tok = thread_token();

    rcu_.read_lock(tok);

    for (uint32_t start = 0; ; ) {
        HashEntry entry = directory_->lookup(hash, start);
        if (entry.empty()) break;

        start = entry.bucket_offset() + 1;

        uint64_t pba = entry.pba();
        uint16_t kv_grains = entry.kv_size();

        KvHeader hdr;
        int rc = co_await verify_key_at_pba(pba, kv_grains, key, &hdr);
        if (rc != 0) continue;

        rcu_.read_unlock(tok);

        if (val_size_out) *val_size_out = hdr.val_size;
        co_return 0;
    }

    rcu_.read_unlock(tok);
    co_return -ENOENT;
}

// Explicit instantiations.
template class UDepot<PosixIO>;
template class UDepot<AioIO>;

}  // namespace udepot

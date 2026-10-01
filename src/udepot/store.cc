// Copyright (c) 2024-2026 Nikolas Ioannou
// SPDX-License-Identifier: BSD-3-Clause

#include "udepot/store.h"

#include <array>
#include <cerrno>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <thread>

#include "udepot/io/aio.h"
#include "udepot/io/posix.h"
#ifdef UDEPOT_BUILD_URING
#include "udepot/io/uring.h"
#endif
#ifdef UDEPOT_BUILD_SPDK
#include "udepot/io/spdk.h"
#endif

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

// CRC32 (same polynomial as zlib) for device/segment metadata checksums.
static constexpr auto kCrc32Table = [] {
    std::array<uint32_t, 256> t{};
    for (uint32_t i = 0; i < 256; ++i) {
        uint32_t crc = i;
        for (int j = 0; j < 8; ++j)
            crc = (crc >> 1) ^ (crc & 1 ? 0xEDB88320u : 0);
        t[i] = crc;
    }
    return t;
}();

static uint32_t crc32_update(uint32_t crc, const uint8_t* data, size_t len) {
    crc ^= 0xFFFFFFFFu;
    for (size_t i = 0; i < len; ++i)
        crc = kCrc32Table[(crc ^ data[i]) & 0xFF] ^ (crc >> 8);
    return crc ^ 0xFFFFFFFFu;
}

static constexpr uint32_t kGlobalSeed = 0xDEADBEEF;

template <typename IO>
uint16_t UDepot<IO>::compute_crc16(const KvHeader& hdr) {
    uint16_t crc = 0xFFFF;
    crc = crc16_update(crc, reinterpret_cast<const uint8_t*>(&hdr),
                       sizeof(hdr));
    return crc;
}

template <typename IO>
uint32_t UDepot<IO>::compute_crc32(
    uint32_t seed, const uint8_t* data, size_t len) {
    uint32_t crc = crc32_update(seed, data, len);
    return crc32_update(crc, reinterpret_cast<const uint8_t*>(&kGlobalSeed),
                        sizeof(kGlobalSeed));
}

template <typename IO>
UDepot<IO>::UDepot() = default;

template <typename IO>
UDepot<IO>::~UDepot() { close(); }

static inline uint64_t align_up(uint64_t val, uint64_t align) {
    return (val + align - 1) / align * align;
}

// ── Device / segment metadata ──────────────────────────────────────────────

template <typename IO>
uint64_t UDepot<IO>::dev_md_grain_offset() const {
    uint64_t md_grains = align_up(sizeof(salsa::salsa_dev_md), grain_size_) /
                         grain_size_;
    return total_grains_ - md_grains;
}

template <typename IO>
int UDepot<IO>::persist_dev_md() {
    salsa::salsa_dev_md md{};
    md.physical_size = static_cast<u64>(total_grains_ * grain_size_);
    md.logical_size = md.physical_size;
    md.segment_size = static_cast<u64>(get_seg_size());
    md.grain_size = grain_size_;
    md.seed = seed_;
    md.csum = compute_crc32(
        static_cast<uint32_t>(md.seed),
        reinterpret_cast<const uint8_t*>(&md),
        sizeof(md) - sizeof(md.csum));

    size_t aligned = align_up(sizeof(md), grain_size_);
    IoBuffer buf = io_.alloc_buffer(aligned);
    if (!buf.data) return -ENOMEM;
    std::memset(buf.data, 0, aligned);
    std::memcpy(buf.data, &md, sizeof(md));

    ssize_t w = io_.pwrite(buf.data, aligned,
                           grain_to_offset(dev_md_grain_offset())).run_sync();
    return (w == static_cast<ssize_t>(aligned)) ? 0 : -EIO;
}

template <typename IO>
bool UDepot<IO>::validate_dev_md(salsa::salsa_dev_md* md_out) {
    size_t aligned = align_up(sizeof(salsa::salsa_dev_md), grain_size_);
    IoBuffer buf = io_.alloc_buffer(aligned);
    if (!buf.data) return false;

    ssize_t r = io_.pread(buf.data, aligned,
                          grain_to_offset(dev_md_grain_offset())).run_sync();
    if (r < static_cast<ssize_t>(sizeof(salsa::salsa_dev_md)))
        return false;

    salsa::salsa_dev_md md;
    std::memcpy(&md, buf.data, sizeof(md));

    uint32_t expected = compute_crc32(
        static_cast<uint32_t>(md.seed),
        reinterpret_cast<const uint8_t*>(&md),
        sizeof(md) - sizeof(md.csum));

    if (md.csum != expected) return false;
    if (md.grain_size != grain_size_) return false;
    if (md.physical_size != total_grains_ * grain_size_) return false;

    if (md_out) *md_out = md;
    return true;
}

template <typename IO>
int UDepot<IO>::persist_seg_md(uint64_t grain_start, uint64_t timestamp) {
    salsa::salsa_seg_md md{};
    md.segment_size = static_cast<u64>(get_seg_size());
    md.grain_size = grain_size_;
    md.timestamp = timestamp;
    md.seed = seed_;
    md.ctlr_type = get_ctlr_id();

    static constexpr size_t csum_off = offsetof(salsa::salsa_seg_md, csum);
    md.csum = compute_crc32(
        static_cast<uint32_t>(md.seed),
        reinterpret_cast<const uint8_t*>(&md), csum_off);

    md.write_nr = 0;
    md.reloc_nr = 0;

    size_t md_bytes = static_cast<size_t>(seg_md_grains_) * grain_size_;
    IoBuffer buf = io_.alloc_buffer(md_bytes);
    if (!buf.data) return -ENOMEM;
    std::memset(buf.data, 0, md_bytes);
    std::memcpy(buf.data, &md, sizeof(md));

    ssize_t w = io_.pwrite(buf.data, md_bytes,
                           grain_to_offset(grain_start)).run_sync();
    return (w == static_cast<ssize_t>(md_bytes)) ? 0 : -EIO;
}

template <typename IO>
bool UDepot<IO>::validate_seg_md(const salsa::salsa_seg_md& md) const {
    if (md.seed != seed_) return false;
    if (md.segment_size != static_cast<u64>(get_seg_size())) return false;
    if (md.grain_size != grain_size_) return false;

    static constexpr size_t csum_off = offsetof(salsa::salsa_seg_md, csum);
    uint32_t expected = compute_crc32(
        static_cast<uint32_t>(md.seed),
        reinterpret_cast<const uint8_t*>(&md), csum_off);

    return md.csum == expected;
}

// ── Crash recovery ─────────────────────────────────────────────────────────

template <typename IO>
int UDepot<IO>::crash_recovery() {
    uint64_t seg_size = get_seg_size();
    uint64_t data_grains = seg_size - seg_md_grains_;
    uint64_t max_timestamp = 0;

    struct TombstoneEntry {
        uint64_t hash;
        uint64_t pba;
        uint16_t kv_grains;
    };
    std::vector<TombstoneEntry> tombstones;

    for (auto it = scm_->begin(); it != scm_->end(); ++it) {
        salsa::GrainRange range = *it;
        uint64_t seg_base = range.grain_start;
        uint64_t seg_idx = scm_->grain_to_seg_idx(seg_base);

        // Read segment metadata from tail.
        uint64_t md_grain = seg_base + seg_size - seg_md_grains_;
        size_t md_bytes = static_cast<size_t>(seg_md_grains_) * grain_size_;
        IoBuffer md_buf = io_.alloc_buffer(md_bytes);
        if (!md_buf.data) continue;

        ssize_t r = io_.pread(md_buf.data, md_bytes,
                              grain_to_offset(md_grain)).run_sync();
        if (r < static_cast<ssize_t>(sizeof(salsa::salsa_seg_md)))
            continue;

        salsa::salsa_seg_md seg_md;
        std::memcpy(&seg_md, md_buf.data, sizeof(seg_md));
        if (!validate_seg_md(seg_md))
            continue;

        uint64_t ts = seg_md.timestamp;
        seg_timestamps_[seg_idx].store(ts, std::memory_order_relaxed);
        if (ts > max_timestamp)
            max_timestamp = ts;

        // Mark the entire segment as in use.
        scm_->restore_grain_range(seg_base, seg_size, get_ctlr_id());

        // Walk data entries in this segment.
        uint64_t grain = seg_base;
        while (grain < seg_base + data_grains) {
            IoBuffer hdr_buf = io_.alloc_buffer(grain_size_);
            if (!hdr_buf.data) break;

            ssize_t nr = io_.pread(hdr_buf.data, grain_size_,
                                   grain_to_offset(grain)).run_sync();
            if (nr < static_cast<ssize_t>(sizeof(KvHeader))) {
                grain++;
                continue;
            }

            auto* p = static_cast<const uint8_t*>(hdr_buf.data);
            KvHeader hdr;
            std::memcpy(&hdr, p, sizeof(hdr));

            if (hdr.key_size == 0) {
                grain++;
                continue;
            }

            uint64_t entry_grains = kv_total_grains(hdr.key_size, hdr.val_size);
            if (entry_grains == 0 ||
                grain + entry_grains > seg_base + data_grains) {
                grain++;
                continue;
            }

            // Validate timestamp matches segment.
            if (hdr.timestamp != ts) {
                grain++;
                continue;
            }

            // Validate CRC16.
            size_t entry_bytes = kv_total_bytes(hdr.key_size, hdr.val_size);
            size_t read_bytes = static_cast<size_t>(entry_grains) * grain_size_;
            IoBuffer entry_buf = io_.alloc_buffer(read_bytes);
            if (!entry_buf.data) {
                grain += entry_grains;
                continue;
            }
            nr = io_.pread(entry_buf.data, read_bytes,
                           grain_to_offset(grain)).run_sync();
            if (nr < static_cast<ssize_t>(entry_bytes)) {
                grain += entry_grains;
                continue;
            }

            auto* ep = static_cast<const uint8_t*>(entry_buf.data);
            KvSuffix suffix;
            std::memcpy(&suffix,
                        ep + sizeof(KvHeader) + hdr.key_size + hdr.val_size,
                        sizeof(suffix));
            if (suffix.crc16 != compute_crc16(hdr)) {
                grain += entry_grains;
                continue;
            }

            // Entry is valid. Hash the key.
            const uint8_t* key_data = ep + sizeof(KvHeader);
            uint64_t hash = CityHash64(
                reinterpret_cast<const char*>(key_data), hdr.key_size);
            uint16_t kv_grains =
                static_cast<uint16_t>(entry_grains);

            if (hdr.val_size == 0) {
                // Tombstone — defer until after full scan.
                tombstones.push_back({hash, grain, kv_grains});
                grain += entry_grains;
                continue;
            }

            // Try insert into directory.
            HashEntry existing = directory_->lookup(hash);
            if (existing.empty()) {
                // No conflict — insert.
                int rc = directory_->insert(hash, kv_grains, grain);
                while (rc != 0) {
                    int grc = directory_->grow();
                    if (grc != 0) return -ENOMEM;
                    rc = directory_->insert(hash, kv_grains, grain);
                }
            } else {
                // Conflict — verify key match and use total ordering.
                size_t ex_read = static_cast<size_t>(existing.kv_size()) *
                                 grain_size_;
                IoBuffer ex_buf = io_.alloc_buffer(ex_read);
                if (!ex_buf.data) {
                    grain += entry_grains;
                    continue;
                }
                ssize_t exr = io_.pread(
                    ex_buf.data, ex_read,
                    grain_to_offset(existing.pba())).run_sync();

                bool key_matches = false;
                if (exr >= static_cast<ssize_t>(sizeof(KvHeader))) {
                    auto* exhp = static_cast<const uint8_t*>(ex_buf.data);
                    KvHeader ex_hdr;
                    std::memcpy(&ex_hdr, exhp, sizeof(ex_hdr));
                    if (ex_hdr.key_size == hdr.key_size &&
                        std::memcmp(exhp + sizeof(KvHeader),
                                    key_data, hdr.key_size) == 0)
                        key_matches = true;
                }

                if (key_matches) {
                    // Same key — total ordering decides.
                    uint64_t old_seg =
                        scm_->grain_to_seg_idx(existing.pba());
                    uint64_t new_seg = seg_idx;
                    bool new_is_newer;
                    if (old_seg != new_seg) {
                        new_is_newer =
                            seg_timestamps_[old_seg].load(
                                std::memory_order_relaxed) <
                            seg_timestamps_[new_seg].load(
                                std::memory_order_relaxed);
                    } else {
                        new_is_newer = existing.pba() < grain;
                    }

                    if (new_is_newer) {
                        invalidate_grains(existing.pba(),
                                          existing.kv_size());
                        directory_->update(
                            hash, existing.pba(), kv_grains, grain);
                    } else {
                        invalidate_grains(grain, kv_grains);
                    }
                } else {
                    // Hash collision with different key — just insert.
                    int rc = directory_->insert(hash, kv_grains, grain);
                    while (rc != 0) {
                        int grc = directory_->grow();
                        if (grc != 0) return -ENOMEM;
                        rc = directory_->insert(hash, kv_grains, grain);
                    }
                }
            }

            grain += entry_grains;
        }
    }

    // Process tombstones: remove matching entries from the directory.
    for (auto& tomb : tombstones) {
        HashEntry entry = directory_->lookup(tomb.hash);
        if (entry.empty()) {
            invalidate_grains(tomb.pba, tomb.kv_grains);
            continue;
        }

        // Read the tombstone's key to verify match.
        size_t tomb_read = static_cast<size_t>(tomb.kv_grains) * grain_size_;
        IoBuffer tomb_buf = io_.alloc_buffer(tomb_read);
        if (!tomb_buf.data) {
            invalidate_grains(tomb.pba, tomb.kv_grains);
            continue;
        }
        ssize_t tr = io_.pread(tomb_buf.data, tomb_read,
                               grain_to_offset(tomb.pba)).run_sync();
        if (tr < static_cast<ssize_t>(sizeof(KvHeader))) {
            invalidate_grains(tomb.pba, tomb.kv_grains);
            continue;
        }

        auto* tp = static_cast<const uint8_t*>(tomb_buf.data);
        KvHeader tomb_hdr;
        std::memcpy(&tomb_hdr, tp, sizeof(tomb_hdr));

        // Check if the existing entry has the same key.
        size_t ex_read = static_cast<size_t>(entry.kv_size()) * grain_size_;
        IoBuffer ex_buf = io_.alloc_buffer(ex_read);
        if (!ex_buf.data) {
            invalidate_grains(tomb.pba, tomb.kv_grains);
            continue;
        }
        ssize_t er = io_.pread(ex_buf.data, ex_read,
                               grain_to_offset(entry.pba())).run_sync();
        if (er < static_cast<ssize_t>(sizeof(KvHeader))) {
            invalidate_grains(tomb.pba, tomb.kv_grains);
            continue;
        }

        auto* ep = static_cast<const uint8_t*>(ex_buf.data);
        KvHeader ex_hdr;
        std::memcpy(&ex_hdr, ep, sizeof(ex_hdr));

        bool key_matches = ex_hdr.key_size == tomb_hdr.key_size &&
                           std::memcmp(ep + sizeof(KvHeader),
                                       tp + sizeof(KvHeader),
                                       tomb_hdr.key_size) == 0;

        if (key_matches) {
            // Total ordering: is the tombstone newer?
            uint64_t old_seg = scm_->grain_to_seg_idx(entry.pba());
            uint64_t tomb_seg = scm_->grain_to_seg_idx(tomb.pba);
            bool tomb_is_newer;
            if (old_seg != tomb_seg) {
                tomb_is_newer =
                    seg_timestamps_[old_seg].load(
                        std::memory_order_relaxed) <
                    seg_timestamps_[tomb_seg].load(
                        std::memory_order_relaxed);
            } else {
                tomb_is_newer = entry.pba() < tomb.pba;
            }

            if (tomb_is_newer) {
                directory_->remove(tomb.hash, entry.pba());
                invalidate_grains(entry.pba(), entry.kv_size());
            }
        }

        invalidate_grains(tomb.pba, tomb.kv_grains);
    }

    // Restore seg_alloc_nr so future allocations get higher timestamps.
    if (max_timestamp > 0)
        restore_seg_alloc_nr(max_timestamp);

    return 0;
}

template <typename IO>
int UDepot<IO>::open(const StoreConfig& config) {
    grain_size_ = config.grain_size;
    total_grains_ = config.size / grain_size_;

    int rc = io_.open(config.path, config.size);
    if (rc != 0) return rc;

    // Salsa initialization — matches uDepot's init_local().
    seg_md_grains_ = align_up(sizeof(salsa::salsa_seg_md), grain_size_) /
                     grain_size_;

    // Try to restore from existing device metadata.
    bool restored = false;
    salsa::salsa_dev_md dev_md{};
    uint64_t segment_size = config.segment_size;

    if (!config.force_destroy && validate_dev_md(&dev_md)) {
        restored = true;
        seed_ = dev_md.seed;
        segment_size = dev_md.segment_size;
    } else {
        struct timespec ts;
        clock_gettime(CLOCK_MONOTONIC, &ts);
        seed_ = static_cast<uint64_t>(ts.tv_sec);
    }

    if (segment_size == 0)
        segment_size = (1ULL << 29) / grain_size_ + 2;

    // Start with 1 table for recovery (will grow as entries are inserted),
    // config.initial_tables for fresh.
    uint32_t initial_tables = restored ? 1 : config.initial_tables;
    directory_ = new Directory(rcu_, initial_tables, config.index_bits);

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

    // Allocate per-segment timestamp and dirty-flag arrays.
    num_segments_ = total_grains_ / get_seg_size();
    seg_timestamps_ = std::make_unique<std::atomic<uint64_t>[]>(num_segments_);
    seg_md_dirty_ = std::make_unique<std::atomic<bool>[]>(num_segments_);
    for (uint64_t i = 0; i < num_segments_; ++i) {
        seg_timestamps_[i].store(0, std::memory_order_relaxed);
        seg_md_dirty_[i].store(false, std::memory_order_relaxed);
    }

    if (restored) {
        rc = crash_recovery();
        if (rc != 0) {
            salsa::SalsaCtlr::shutdown();
            delete scm_;
            scm_ = nullptr;
            delete directory_;
            directory_ = nullptr;
            io_.close();
            return rc;
        }
    } else {
        rc = persist_dev_md();
        if (rc != 0) {
            salsa::SalsaCtlr::shutdown();
            delete scm_;
            scm_ = nullptr;
            delete directory_;
            directory_ = nullptr;
            io_.close();
            return rc;
        }
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

    open_.store(true, std::memory_order_release);
    return 0;
}

template <typename IO>
void UDepot<IO>::close() {
    // Operations check open_ inside their RCU read section, so once the
    // grace period ends none is still running and none can start.
    open_.store(false, std::memory_order_release);
    rcu_.synchronize();

    if (scm_) {
        scm_->exit_threads();

        // Persist any dirty segment metadata before shutdown.
        if (seg_md_dirty_) {
            for (uint64_t i = 0; i < num_segments_; ++i) {
                if (seg_md_dirty_[i].load(std::memory_order_acquire)) {
                    uint64_t seg_base = i * get_seg_size();
                    uint64_t md_grain = seg_base + get_seg_size() -
                                        seg_md_grains_;
                    persist_seg_md(md_grain,
                                   seg_timestamps_[i].load(
                                       std::memory_order_relaxed));
                }
            }
        }

        // Persist device metadata before shutdown so the next open can
        // recover.
        persist_dev_md();

        salsa::SalsaCtlr::shutdown();
        delete scm_;
        scm_ = nullptr;
    }

    seg_timestamps_.reset();
    seg_md_dirty_.reset();
    num_segments_ = 0;

    delete directory_;
    directory_ = nullptr;
    io_.close();
}

template <typename IO>
uint64_t UDepot<IO>::allocate_grains(uint64_t count) {
    // EAGAIN is transient: salsa's allocator thread has not staged the next
    // segment yet, which happens under CPU load. Legacy retries it without
    // bound; this is bounded because GC cannot yet reclaim segments here
    // (nothing calls release_grains), so a store out of free segments would
    // otherwise hang instead of failing.
    static constexpr int kMaxAttempts = 16;
    u64 grain_out = 0;
    for (int attempt = 0; attempt < kMaxAttempts; ++attempt) {
        int rc = salsa::SalsaCtlr::allocate_grains(
            static_cast<u64>(count), &grain_out);
        if (rc == 0) return grain_out;
        if (rc != EAGAIN) break;
        std::this_thread::yield();
    }
    return UINT64_MAX;
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

        {
            Rcu::ReadGuard guard(rcu_);
            directory_->remove(hash, grain);
        }

        grain += entry_grains;
    }

    return 0;
}

// Segment metadata callback — called when salsa allocates a new segment.
// inc_seg_alloc_nr() has already been called by salsa's wrapper before
// this runs, so get_seg_alloc_nr() returns the new timestamp.
//
// No I/O here — salsa may hold internal locks.  The persist is deferred
// to the coroutine-based put()/del() path via ensure_seg_md().
template <typename IO>
void UDepot<IO>::seg_md_callback(u64 grain_start, u64 /*grain_nr*/) {
    uint64_t ts = get_seg_alloc_nr();
    uint64_t seg_idx = scm_->grain_to_seg_idx(grain_start);
    if (seg_idx < num_segments_) {
        seg_timestamps_[seg_idx].store(ts, std::memory_order_release);
        seg_md_dirty_[seg_idx].store(true, std::memory_order_release);
    }
}

template <typename IO>
CoroTask<int> UDepot<IO>::ensure_seg_md(uint64_t grain) {
    uint64_t seg_idx = scm_->grain_to_seg_idx(grain);
    if (seg_idx >= num_segments_)
        co_return 0;
    bool expected = true;
    if (!seg_md_dirty_[seg_idx].compare_exchange_strong(
            expected, false, std::memory_order_acq_rel))
        co_return 0;

    uint64_t seg_size = get_seg_size();
    uint64_t seg_base = seg_idx * seg_size;
    uint64_t md_grain = seg_base + seg_size - seg_md_grains_;
    uint64_t ts = seg_timestamps_[seg_idx].load(std::memory_order_acquire);

    salsa::salsa_seg_md md{};
    md.segment_size = static_cast<u64>(seg_size);
    md.grain_size = grain_size_;
    md.timestamp = ts;
    md.seed = seed_;
    md.ctlr_type = get_ctlr_id();

    static constexpr size_t csum_off = offsetof(salsa::salsa_seg_md, csum);
    md.csum = compute_crc32(
        static_cast<uint32_t>(md.seed),
        reinterpret_cast<const uint8_t*>(&md), csum_off);
    md.write_nr = 0;
    md.reloc_nr = 0;

    size_t md_bytes = static_cast<size_t>(seg_md_grains_) * grain_size_;
    IoBuffer buf = io_.alloc_buffer(md_bytes);
    if (!buf.data) {
        seg_md_dirty_[seg_idx].store(true, std::memory_order_release);
        co_return -ENOMEM;
    }
    std::memset(buf.data, 0, md_bytes);
    std::memcpy(buf.data, &md, sizeof(md));

    ssize_t w = co_await io_.pwrite(buf.data, md_bytes,
                                     grain_to_offset(md_grain));
    if (w != static_cast<ssize_t>(md_bytes)) {
        seg_md_dirty_[seg_idx].store(true, std::memory_order_release);
        co_return -EIO;
    }
    co_return 0;
}

template <typename IO>
bool UDepot<IO>::newer_than(uint64_t new_pba, uint64_t old_pba) const {
    uint64_t old_seg = scm_->grain_to_seg_idx(old_pba);
    uint64_t new_seg = scm_->grain_to_seg_idx(new_pba);
    if (old_seg != new_seg)
        return seg_timestamps_[old_seg].load(std::memory_order_acquire) <
               seg_timestamps_[new_seg].load(std::memory_order_acquire);
    return old_pba < new_pba;
}

template <typename IO>
CoroTask<int> UDepot<IO>::probe_key(uint64_t hash,
                                    std::span<const uint8_t> key,
                                    KeyProbe& probe) {
    for (uint32_t start = 0; ; ) {
        HashEntry entry = directory_->lookup(hash, start);
        if (entry.empty()) break;
        start = entry.bucket_offset() + 1;
        if (probe.find(entry.pba())) continue;

        int vrc = co_await verify_key_at_pba(
            entry.pba(), entry.kv_size(), key, nullptr);
        if (vrc != 0 && vrc != -ENOENT) co_return vrc;
        // A neighborhood holds at most kHopRange entries; a full array
        // means some recorded pbas left it, so start the record over.
        if (probe.n == probe.seen.size()) probe.n = 0;
        probe.seen[probe.n++] = {entry.pba(), vrc == 0};
    }
    co_return 0;
}

template <typename IO>
bool UDepot<IO>::probe_settled(const HashTable& table, uint64_t hash,
                               const KeyProbe& probe, HashEntry* match) {
    *match = HashEntry{};
    for (uint32_t start = 0; ; ) {
        HashEntry entry = table.lookup(hash, start);
        if (entry.empty()) return true;
        start = entry.bucket_offset() + 1;
        const auto* seen = probe.find(entry.pba());
        if (!seen) return false;
        if (seen->match && match->empty()) *match = entry;
    }
}

template <typename IO>
int UDepot<IO>::commit_put(uint64_t hash, const KeyProbe& probe,
                           PutMode mode, uint64_t if_version,
                           uint16_t kv_grains, uint64_t pba,
                           HashEntry* replaced) {
    HashTable& table = directory_->table_for_hash(hash);
    auto lock = table.lock_for(hash);

    HashEntry match;
    if (!probe_settled(table, hash, probe, &match)) return kRetryProbe;

    if (match.empty()) {
        if (mode == PutMode::kReplace || if_version != kAnyVersion)
            return -ENOENT;
        return table.insert_locked(hash, kv_grains, pba) == 0 ? 0 : -ENOSPC;
    }
    if (mode == PutMode::kCreate) return -EEXIST;
    if (if_version != kAnyVersion && match.pba() != if_version)
        return -ESTALE;
    // Recovery keeps the newer of two copies of a key, so the copy the
    // directory ends up pointing to must be the newer one (legacy
    // is_pba_order_equal_to_total_order).
    *replaced = match;
    if (!newer_than(pba, match.pba())) return kRewrite;

    table.update_locked(hash, match.pba(), kv_grains, pba);
    return 0;
}

template <typename IO>
CoroTask<int> UDepot<IO>::put(std::span<const uint8_t> key,
                              std::span<const uint8_t> val,
                              PutMode mode, uint64_t if_version) {
    if (key.empty() || key.size() > UINT16_MAX)
        co_return -EINVAL;
    if (val.size() > UINT32_MAX)
        co_return -EINVAL;

    uint64_t hash = hash_key(key);
    uint64_t grains_needed = kv_total_grains(key.size(), val.size());

    if (grains_needed > HashEntry::kKvSizeMask)
        co_return -EINVAL;

    Rcu::ReadGuard guard(rcu_);
    if (!open_.load(std::memory_order_acquire)) co_return -ESHUTDOWN;

    // Build the on-disk entry.  Must happen before any co_await so that
    // key/val data is copied while the caller's buffers are still alive.
    size_t total = grains_needed * grain_size_;
    IoBuffer buf = io_.alloc_buffer(total);
    if (!buf.data) co_return -ENOMEM;

    auto* p = static_cast<uint8_t*>(buf.data);
    KvHeader hdr;
    hdr.key_size = static_cast<uint16_t>(key.size());
    hdr.val_size = static_cast<uint32_t>(val.size());
    std::memcpy(p + sizeof(hdr), key.data(), key.size());
    std::memcpy(p + sizeof(hdr) + key.size(), val.data(), val.size());

    // Zero any padding between the suffix and the end of the grain-aligned
    // region.
    size_t used = kv_total_bytes(key.size(), val.size());
    if (total > used)
        std::memset(p + used, 0, total - used);

    buf.length = total;

    KeyProbe probe;
    for (;;) {
        uint64_t grain = allocate_grains(grains_needed);
        if (grain == UINT64_MAX) co_return -ENOSPC;

        hdr.timestamp = seg_timestamps_[scm_->grain_to_seg_idx(grain)].load(
            std::memory_order_acquire);
        std::memcpy(p, &hdr, sizeof(hdr));
        KvSuffix suffix;
        suffix.crc16 = compute_crc16(hdr);
        std::memcpy(p + sizeof(hdr) + key.size() + val.size(),
                    &suffix, sizeof(suffix));

        // Persist segment metadata if this is the first write to a new
        // segment.
        co_await ensure_seg_md(grain);

        ssize_t written = co_await io_.pwrite(buf.data, total,
                                              grain_to_offset(grain));
        if (written != static_cast<ssize_t>(total)) {
            invalidate_grains(grain, grains_needed);
            co_return (written < 0) ? static_cast<int>(written) : -EIO;
        }

        int rc;
        HashEntry replaced;
        do {
            rc = co_await probe_key(hash, key, probe);
            if (rc == 0)
                rc = commit_put(hash, probe, mode, if_version,
                                static_cast<uint16_t>(grains_needed), grain,
                                &replaced);
        } while (rc == kRetryProbe);

        if (rc == 0) {
            if (!replaced.empty())
                invalidate_grains(replaced.pba(), replaced.kv_size());
            co_return 0;
        }

        invalidate_grains(grain, grains_needed);
        if (rc != kRewrite) co_return rc;

        // A concurrent put of this key was allocated after us but committed
        // first. Write again at a newer location; if it sits in another
        // segment, close ours so the next allocation is newer still.
        if (scm_->grain_to_seg_idx(grain) !=
            scm_->grain_to_seg_idx(replaced.pba()))
            salsa::SalsaCtlr::drain_remaining_grains();
    }
}

template <typename IO>
CoroTask<int> UDepot<IO>::verify_key_at_pba(
    uint64_t pba, uint16_t kv_grains,
    std::span<const uint8_t> key, KvHeader* hdr_out) {

    size_t read_size = sizeof(KvHeader) + key.size();
    // An entry too small to hold this key cannot be it.
    if (kv_total_bytes(key.size(), 0) >
        static_cast<size_t>(kv_grains) * grain_size_)
        co_return -ENOENT;
    // Round up to grain boundary for the read.
    size_t aligned_size = ((read_size + grain_size_ - 1) / grain_size_) *
                          grain_size_;

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
                              size_t* val_size_out, uint64_t* version_out) {
    if (key.empty()) co_return -EINVAL;

    uint64_t hash = hash_key(key);
    Rcu::ReadGuard guard(rcu_);
    if (!open_.load(std::memory_order_acquire)) co_return -ESHUTDOWN;

    // Iterate through all tag-matching entries to handle collisions.
    for (uint32_t start = 0; ; ) {
        HashEntry entry = directory_->lookup(hash, start);
        if (entry.empty()) break;

        start = entry.bucket_offset() + 1;

        uint64_t pba = entry.pba();
        uint16_t kv_grains = entry.kv_size();

        size_t read_bytes = static_cast<size_t>(kv_grains) * grain_size_;
        IoBuffer buf = io_.alloc_buffer(read_bytes);
        if (!buf.data) co_return -ENOMEM;

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
            if (suffix.crc16 != expected) co_return -EIO;
        }
#endif

        if (val_size_out) *val_size_out = hdr.val_size;
        if (version_out) *version_out = pba;
        if (val_out && val_buf_size > 0) {
            size_t to_copy = std::min(val_buf_size,
                                      static_cast<size_t>(hdr.val_size));
            std::memcpy(val_out, p + sizeof(hdr) + hdr.key_size, to_copy);
        }

        co_return 0;
    }

    co_return -ENOENT;
}

template <typename IO>
CoroTask<int> UDepot<IO>::write_tombstone(std::span<const uint8_t> key,
                                          uint64_t* tomb_out) {
    uint64_t tomb_grains = kv_total_grains(key.size(), 0);
    uint64_t tomb_grain = allocate_grains(tomb_grains);
    if (tomb_grain == UINT64_MAX) co_return -ENOSPC;

    size_t tomb_total = tomb_grains * grain_size_;
    IoBuffer tomb_buf = io_.alloc_buffer(tomb_total);
    if (!tomb_buf.data) {
        invalidate_grains(tomb_grain, tomb_grains);
        co_return -ENOMEM;
    }
    auto* tp = static_cast<uint8_t*>(tomb_buf.data);
    KvHeader tomb_hdr;
    tomb_hdr.key_size = static_cast<uint16_t>(key.size());
    tomb_hdr.val_size = 0;
    tomb_hdr.timestamp =
        seg_timestamps_[scm_->grain_to_seg_idx(tomb_grain)].load(
            std::memory_order_acquire);
    std::memcpy(tp, &tomb_hdr, sizeof(tomb_hdr));
    std::memcpy(tp + sizeof(tomb_hdr), key.data(), key.size());
    KvSuffix suffix;
    suffix.crc16 = compute_crc16(tomb_hdr);
    std::memcpy(tp + sizeof(tomb_hdr) + key.size(), &suffix, sizeof(suffix));
    size_t used = kv_total_bytes(key.size(), 0);
    if (tomb_total > used)
        std::memset(tp + used, 0, tomb_total - used);
    tomb_buf.length = tomb_total;

    co_await ensure_seg_md(tomb_grain);
    ssize_t w = co_await io_.pwrite(tomb_buf.data, tomb_total,
                                    grain_to_offset(tomb_grain));
    if (w != static_cast<ssize_t>(tomb_total)) {
        invalidate_grains(tomb_grain, tomb_grains);
        co_return (w < 0) ? static_cast<int>(w) : -EIO;
    }
    *tomb_out = tomb_grain;
    co_return 0;
}

template <typename IO>
int UDepot<IO>::commit_del(uint64_t hash, const KeyProbe& probe,
                           uint64_t if_version, uint64_t tomb_pba,
                           HashEntry* removed) {
    HashTable& table = directory_->table_for_hash(hash);
    auto lock = table.lock_for(hash);

    HashEntry match;
    if (!probe_settled(table, hash, probe, &match)) return kRetryProbe;
    if (match.empty()) return -ENOENT;
    if (if_version != kAnyVersion && match.pba() != if_version)
        return -ESTALE;
    *removed = match;
    if (tomb_pba == UINT64_MAX) return kNeedTomb;
    // As in commit_put: recovery must see the tombstone as newer than the
    // value it deletes.
    if (!newer_than(tomb_pba, match.pba())) return kRewrite;

    table.remove_locked(hash, match.pba());
    return 0;
}

template <typename IO>
CoroTask<int> UDepot<IO>::del(std::span<const uint8_t> key,
                              uint64_t if_version) {
    if (key.empty() || key.size() > UINT16_MAX) co_return -EINVAL;

    uint64_t hash = hash_key(key);
    Rcu::ReadGuard guard(rcu_);
    if (!open_.load(std::memory_order_acquire)) co_return -ESHUTDOWN;

    const uint64_t tomb_grains = kv_total_grains(key.size(), 0);
    uint64_t tomb = UINT64_MAX;
    KeyProbe probe;
    for (;;) {
        HashEntry removed;
        int rc = co_await probe_key(hash, key, probe);
        if (rc == 0)
            rc = commit_del(hash, probe, if_version, tomb, &removed);

        if (rc == kRetryProbe) continue;
        if (rc == 0) {
            invalidate_grains(removed.pba(), removed.kv_size());
            co_return 0;
        }
        if (rc == kNeedTomb) {
            // Written unlocked; commit_del re-checks the entry it deletes.
            rc = co_await write_tombstone(key, &tomb);
            if (rc != 0) co_return rc;
            continue;
        }

        // The tombstone is not needed (key gone, version changed) or not
        // newer than the entry it would delete (kRewrite).
        if (tomb != UINT64_MAX) {
            invalidate_grains(tomb, tomb_grains);
            if (rc == kRewrite &&
                scm_->grain_to_seg_idx(tomb) !=
                    scm_->grain_to_seg_idx(removed.pba()))
                salsa::SalsaCtlr::drain_remaining_grains();
            tomb = UINT64_MAX;
        }
        if (rc != kRewrite) co_return rc;
    }
}

template <typename IO>
CoroTask<int> UDepot<IO>::exists(std::span<const uint8_t> key,
                                 size_t* val_size_out) {
    if (key.empty()) co_return -EINVAL;

    uint64_t hash = hash_key(key);
    Rcu::ReadGuard guard(rcu_);
    if (!open_.load(std::memory_order_acquire)) co_return -ESHUTDOWN;

    for (uint32_t start = 0; ; ) {
        HashEntry entry = directory_->lookup(hash, start);
        if (entry.empty()) break;

        start = entry.bucket_offset() + 1;

        uint64_t pba = entry.pba();
        uint16_t kv_grains = entry.kv_size();

        KvHeader hdr;
        int rc = co_await verify_key_at_pba(pba, kv_grains, key, &hdr);
        if (rc != 0) continue;

        if (val_size_out) *val_size_out = hdr.val_size;
        co_return 0;
    }

    co_return -ENOENT;
}

// Explicit instantiations.
template class UDepot<PosixIO>;
template class UDepot<AioIO>;
#ifdef UDEPOT_BUILD_URING
template class UDepot<UringIO>;
#endif
#ifdef UDEPOT_BUILD_SPDK
template class UDepot<SpdkIO>;
#endif

}  // namespace udepot

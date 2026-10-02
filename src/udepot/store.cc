// Copyright (c) 2024-2026 Nikolas Ioannou
// SPDX-License-Identifier: BSD-3-Clause

#include "udepot/store.h"

#include <array>
#include <cassert>
#include <chrono>
#include <cerrno>
#include <cstdio>
#include <cstdlib>
#include <algorithm>
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

    ssize_t w = io_.pwrite_sync(buf.data, md_bytes, grain_to_offset(grain_start));
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
int UDepot<IO>::recover_record(uint64_t hash, std::span<const uint8_t> key,
                               uint64_t grain, uint16_t kv_grains,
                               bool tombstone) {
    // Same lookup as a put or del (legacy try_restore_entry ran
    // lookup_mbuff_put): every tag-matching entry, live or deleted, is
    // verified against the key, so another key with the same tag earlier in
    // the neighborhood cannot hide this key's entry.
    KeyProbe probe;
    int rc = probe_key(hash, key, probe).run_sync();
    if (rc != 0) return rc;

    for (;;) {
        HashEntry match;
        uint16_t match_grains = 0;
        {
            // Recovery runs alone, before any other thread: no grow can
            // race it, and it holds no read section, so it may grow inline.
            auto locked = directory_->lock_for(hash);
            assert(!locked.frozen());
            bool settled = probe_settled(*locked.table, hash, probe, &match);
            assert(settled);
            (void)settled;
            uint16_t new_kv = tombstone ? 0 : kv_grains;
            if (match.empty()) {
                if (locked.table->insert_locked(hash, new_kv, grain) == 0)
                    return 0;
                uint64_t seen = locked.snapshot->generation;
                locked.lock = HashTable::WriteLock{};
                if (directory_->grow(seen) != 0) return -ENOSPC;
                continue;
            }
            // Two records of one key: the newer one wins, as at run time.
            if (!newer_than(grain, match.pba())) {
                match = HashEntry{};
            } else {
                match_grains = match.deleted()
                    ? static_cast<uint16_t>(kv_total_grains(key.size(), 0))
                    : match.kv_size();
                locked.table->update_locked(hash, match.pba(), new_kv, grain);
            }
        }
        if (match.empty())
            invalidate_grains(grain, kv_grains);
        else
            invalidate_grains(match.pba(), match_grains);
        return 0;
    }
}

template <typename IO>
int UDepot<IO>::read_segment(uint64_t grain, uint64_t grains, IoBuffer& buf) {
    size_t bytes = static_cast<size_t>(grains) * grain_size_;
    if (!buf.data || buf.capacity < bytes) {
        buf = io_.alloc_buffer(bytes);
        if (!buf.data) return -ENOMEM;
    }
    // Chunked so a backend that bounces through DMA memory needs only a
    // chunk of it at a time.
    constexpr size_t kChunk = size_t{4} << 20;
    auto* dst = static_cast<uint8_t*>(buf.data);
    for (size_t off = 0; off < bytes; off += kChunk) {
        size_t n = std::min(kChunk, bytes - off);
        ssize_t r = io_.pread(dst + off, n,
                              grain_to_offset(grain) + static_cast<off_t>(off))
                        .run_sync();
        if (r != static_cast<ssize_t>(n)) return r < 0 ? static_cast<int>(r) : -EIO;
    }
    return 0;
}

template <typename IO>
int UDepot<IO>::crash_recovery() {
    uint64_t seg_size = get_seg_size();
    uint64_t data_grains = seg_size - seg_md_grains_;
    uint64_t max_timestamp = 0;
    IoBuffer seg_buf;

    for (auto it = scm_->begin(); it != scm_->end(); ++it) {
        salsa::GrainRange range = *it;
        uint64_t seg_base = range.grain_start;
        uint64_t seg_idx = scm_->grain_to_seg_idx(seg_base);

        // Read segment metadata from tail.
        uint64_t md_grain = seg_base + seg_size - seg_md_grains_;
        size_t md_bytes = static_cast<size_t>(seg_md_grains_) * grain_size_;
        IoBuffer md_buf = io_.alloc_buffer(md_bytes);
        if (!md_buf.data) return -ENOMEM;

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
        seg_md_ok_[seg_idx].store(true, std::memory_order_relaxed);
        seg_live_[seg_idx].store(true, std::memory_order_relaxed);
        if (ts > max_timestamp)
            max_timestamp = ts;

        // Mark the segment's data grains in use, as uDepot's crash_recovery
        // does (the net size: the metadata grains at the tail hold no
        // records and must not count as valid); grains that hold no valid
        // record are invalidated as the walk passes them.
        scm_->restore_grain_range(seg_base, data_grains, get_ctlr_id());

        // As uDepot (which maps the segment): one read of the net segment,
        // then walk it in memory.
        int rrc = read_segment(seg_base, data_grains, seg_buf);
        if (rrc != 0) return rrc;
        const auto* seg = static_cast<const uint8_t*>(seg_buf.data);

        uint64_t grain = seg_base;
        auto skip = [&](uint64_t n) {
            invalidate_grains(grain, n);
            grain += n;
        };
        while (grain < seg_base + data_grains) {
            const uint8_t* ep = seg + (grain - seg_base) * grain_size_;
            KvHeader hdr;
            std::memcpy(&hdr, ep, sizeof(hdr));
            if (hdr.key_size == 0 || hdr.timestamp != ts) {
                skip(1);
                continue;
            }

            const size_t val_bytes = record_val_bytes(hdr);
            uint64_t entry_grains = kv_total_grains(hdr.key_size, val_bytes);
            if (entry_grains == 0 || entry_grains > HashEntry::kKvSizeMask ||
                grain + entry_grains > seg_base + data_grains) {
                skip(1);
                continue;
            }

            KvSuffix suffix;
            std::memcpy(&suffix,
                        ep + sizeof(KvHeader) + hdr.key_size + val_bytes,
                        sizeof(suffix));
            if (suffix.crc16 != compute_crc16(hdr)) {
                skip(1);
                continue;
            }

            std::span<const uint8_t> key(ep + sizeof(KvHeader), hdr.key_size);
            int rc = recover_record(hash_key(key), key, grain,
                                    static_cast<uint16_t>(entry_grains),
                                    is_tombstone(hdr));
            if (rc != 0) return rc;
            grain += entry_grains;
        }
    }

    // Restore seg_alloc_nr so future allocations get higher timestamps.
    if (max_timestamp > 0)
        restore_seg_alloc_nr(max_timestamp);

    return 0;
}

template <typename IO>
int UDepot<IO>::open(const StoreConfig& config) {
    grain_size_ = config.grain_size;

    int rc = io_.open(config.path, config.size);
    if (rc != 0) return rc;

    // As uDepot's register_local_region: the device size is whatever the
    // backend reports once open (config.size 0 means "the whole device").
    const uint64_t dev_size = io_.get_size();
    total_grains_ = dev_size / grain_size_;

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

    // Relocating GC with uDepot's non-memcache parameters
    // (uDepotSalsa::init: init_local(14, 2, 4)).
    static constexpr uint32_t kGcType = 14;
    static constexpr uint32_t kGcLowWm = 2;
    static constexpr uint32_t kGcHighWm = 4;

    // Halving retry loop (matches uDepot): if the segment size is too
    // large for the device, halve and try again.
    while (true) {
        char argv_buf[256];
        snprintf(argv_buf, sizeof(argv_buf),
                 "scm_dev= dev_size=%lu grain_size=%u"
                 " segment_size=%lu gc_type=%u gc_low_wm=%u gc_high_wm=%u"
                 " gc_thread_nr=1 simulation=1",
                 static_cast<unsigned long>(dev_size),
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
    seg_md_ok_ = std::make_unique<std::atomic<bool>[]>(num_segments_);
    seg_live_ = std::make_unique<std::atomic<bool>[]>(num_segments_);
    for (uint64_t i = 0; i < num_segments_; ++i) {
        seg_timestamps_[i].store(0, std::memory_order_relaxed);
        seg_md_ok_[i].store(false, std::memory_order_relaxed);
        seg_live_[i].store(false, std::memory_order_relaxed);
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

    {
        std::lock_guard<std::mutex> lock(space_mu_);
        space_stop_ = false;
    }
    space_waker_ = std::thread(&UDepot::space_waker_loop, this);
    return 0;
}

template <typename IO>
void UDepot<IO>::close() {
    // As uDepot's shutdown(): no operation may be in progress or start
    // (store.h), so none is waiting for space or a grow either.
    stop_space_waker();

    if (scm_) {
        scm_->exit_threads();

        // Persist device metadata before shutdown so the next open can
        // recover.
        persist_dev_md();

        salsa::SalsaCtlr::shutdown();
        delete scm_;
        scm_ = nullptr;
    }

    seg_timestamps_.reset();
    seg_md_ok_.reset();
    seg_live_.reset();
    num_segments_ = 0;

    delete directory_;
    directory_ = nullptr;
    io_.close();
}

template <typename IO>
int UDepot<IO>::try_allocate_grains(uint64_t count, uint64_t* grain) {
    u64 grain_out = 0;
    int rc = salsa::SalsaCtlr::allocate_grains_no_wait(
        static_cast<u64>(count), &grain_out);
    if (rc == EAGAIN) return -EAGAIN;
    if (rc != 0) return -ENOSPC;
    // A segment whose metadata could not be written would not be found by
    // crash recovery, so nothing may be acknowledged from it.
    if (!seg_md_ok_[scm_->grain_to_seg_idx(grain_out)].load(
            std::memory_order_acquire)) {
        invalidate_grains(grain_out, count);
        release_grains(grain_out, count);
        return -EIO;
    }
    *grain = grain_out;
    return 0;
}

template <typename IO>
CoroTask<int> UDepot<IO>::allocate_or_wait(
    uint64_t count, uint64_t* grain, std::optional<Rcu::ReadGuard>& guard,
    KeyProbe* probe) {
    for (;;) {
        int rc = try_allocate_grains(count, grain);
        if (rc != -EAGAIN) co_return rc;
        guard.reset();
        if (probe) probe->n = 0;
        co_await SpaceWait{this};
        guard.emplace(rcu_);
    }
}

template <typename IO>
bool UDepot<IO>::SpaceWait::await_suspend(std::coroutine_handle<> h) {
    std::lock_guard<std::mutex> lock(store->space_mu_);
    assert(!store->space_stop_);  // no operation may race close()
    if (grow != Directory::kAnyGeneration)
        store->grow_request_ =
            std::max(store->grow_request_.value_or(0), grow);
    store->space_waiters_.push_back(h);
    store->space_cv_.notify_one();
    return true;
}

template <typename IO>
CoroTask<int> UDepot<IO>::wait_for_grow(uint64_t gen, bool grow,
                                        std::optional<Rcu::ReadGuard>& guard,
                                        KeyProbe* probe) {
    // The grow waits for read sections, so leave ours first.
    guard.reset();
    if (probe) probe->n = 0;
    co_await SpaceWait{this, grow ? gen : Directory::kAnyGeneration};
    guard.emplace(rcu_);
    co_return 0;
}

// Resume every waiter, with space_mu_ released. Called with it held.
template <typename IO>
void UDepot<IO>::resume_waiters(std::unique_lock<std::mutex>& lock) {
    auto waiters = std::move(space_waiters_);
    space_waiters_.clear();
    lock.unlock();
    for (auto h : waiters) h.resume();
    // Resumed operations may have submitted I/O from this thread; on a
    // backend that completes on the submitting thread, see it through.
    while (poll_this_thread()) {}
    lock.lock();
}

template <typename IO>
void UDepot<IO>::space_waker_loop() {
    std::unique_lock<std::mutex> lock(space_mu_);
    while (!space_stop_) {
        if (grow_request_) {
            // Grows run here, never on an operation's thread: they wait for
            // a grace period, which an I/O poller could be holding up. This
            // thread holds no read section and no I/O is pending on it.
            uint64_t gen = *grow_request_;
            grow_request_.reset();
            lock.unlock();
            directory_->grow(gen);  // -ENOSPC: commit_put reports it
            lock.lock();
            resume_waiters(lock);
            continue;
        }
        if (space_waiters_.empty()) {
            space_cv_.wait(lock);
            continue;
        }
        // Give salsa's allocator thread and GC a moment, then let every
        // waiter retry; those that still find no space wait again.
        lock.unlock();
        std::this_thread::sleep_for(std::chrono::microseconds(200));
        lock.lock();
        resume_waiters(lock);
    }
    // No operation may race close(), so none is left waiting.
    assert(space_waiters_.empty());
    grow_request_.reset();
}

template <typename IO>
void UDepot<IO>::stop_space_waker() {
    {
        std::lock_guard<std::mutex> lock(space_mu_);
        space_stop_ = true;
        space_cv_.notify_one();
    }
    if (space_waker_.joinable()) space_waker_.join();
}

template <typename IO>
void UDepot<IO>::invalidate_grains(uint64_t grain, uint64_t count,
                                   bool reloc) {
    salsa::SalsaCtlr::invalidate_grains(
        static_cast<u64>(grain), static_cast<u64>(count), reloc);
}

template <typename IO>
void UDepot<IO>::release_grains(uint64_t grain, uint64_t count, bool reloc) {
    salsa::SalsaCtlr::release_grains(static_cast<u64>(grain),
                                     static_cast<u64>(count), 0, reloc);
}

template <typename IO>
int UDepot<IO>::gc_record(uint64_t grain, uint64_t entry_grains,
                          const uint8_t* rec, bool drop_tombstones) {
    KvHeader hdr;
    std::memcpy(&hdr, rec, sizeof(hdr));
    std::span<const uint8_t> key(rec + sizeof(KvHeader), hdr.key_size);
    const uint64_t hash = hash_key(key);
    const size_t bytes = static_cast<size_t>(entry_grains) * grain_size_;

    // As uDepot's local_gc_callback: the key's stripes stay locked across
    // the whole relocation (check, copy, repoint), so no put or del of the
    // key can commit in between and leave the copy -- newer in recovery
    // order -- to win after a crash. Holding the lock across I/O is safe
    // only because the copy goes out with pwrite_sync, which needs no
    // poller: a writer queued on these stripes on a poller thread cannot
    // hold up the write it is waiting for.
    return with_table_blocking(hash, [&](HashTable& table) -> int {
        // Only records the directory still points at matter; anything else
        // in the segment was overwritten, deleted or never committed.
        const HashEntry entry = table.entry_at(hash, grain);
        if (entry.empty()) return 0;

        if (entry.deleted() && drop_tombstones) {
            // As uDepot purged deleted entries at GC; only when no older
            // segment remains, since an older copy of the key would
            // otherwise come back after a crash.
            table.remove_locked(hash, grain);
            invalidate_grains(grain, entry_grains, true);
            return 0;
        }

        // Relocate: copy to the relocation stream with the new segment's
        // timestamp, then repoint the entry.
        u64 dst = 0;
        int rc = salsa::SalsaCtlr::allocate_grains(entry_grains, &dst, 0, true);
        if (rc != 0) return rc;
        auto drop_dst = [&] {
            invalidate_grains(dst, entry_grains, true);
            release_grains(dst, entry_grains, true);
        };
        // As for user writes (try_allocate_grains): crash recovery would
        // not find a segment whose metadata write failed.
        if (!seg_md_ok_[scm_->grain_to_seg_idx(dst)].load(
                std::memory_order_acquire)) {
            drop_dst();
            return EIO;
        }

        IoBuffer buf = io_.alloc_buffer(bytes);
        if (!buf.data) {
            drop_dst();
            return ENOMEM;
        }
        std::memcpy(buf.data, rec, bytes);
        KvHeader moved_hdr = hdr;
        moved_hdr.timestamp = seg_timestamps_[scm_->grain_to_seg_idx(dst)].load(
            std::memory_order_acquire);
        std::memcpy(buf.data, &moved_hdr, sizeof(moved_hdr));
        KvSuffix suffix;
        suffix.crc16 = compute_crc16(moved_hdr);
        std::memcpy(static_cast<uint8_t*>(buf.data) + sizeof(KvHeader) +
                        hdr.key_size + record_val_bytes(hdr),
                    &suffix, sizeof(suffix));

        if (io_.pwrite_sync(buf.data, bytes, grain_to_offset(dst)) !=
            static_cast<ssize_t>(bytes)) {
            drop_dst();
            return EIO;
        }

        if (gc_relocation_test_hook) gc_relocation_test_hook(key);
        bool moved = table.update_locked(hash, grain, entry.kv_size(), dst);
        assert(moved);  // the stripes have been held since entry_at
        (void)moved;
        release_grains(dst, entry_grains, true);
        invalidate_grains(grain, entry_grains, true);
        return 0;
    });
}

// GC callback — called from salsa's GC thread when it reclaims a segment.
// Relocating GC, as uDepot's non-memcache runtimes used: every record the
// directory still references moves out, then the segment is freed.
template <typename IO>
int UDepot<IO>::gc_callback(u64 grain_start, u64 grain_nr) {
    uint64_t victim = scm_->grain_to_seg_idx(grain_start);
    uint64_t victim_ts = seg_timestamps_[victim].load(std::memory_order_acquire);

    // A tombstone only guards against older copies of its key, which can
    // live only in older segments.
    bool drop_tombstones = true;
    for (uint64_t i = 0; i < num_segments_ && drop_tombstones; ++i) {
        if (i != victim && seg_live_[i].load(std::memory_order_acquire) &&
            seg_timestamps_[i].load(std::memory_order_acquire) < victim_ts)
            drop_tombstones = false;
    }

    // Net segment excludes the per-segment metadata grains at the tail.
    // As uDepot (which maps the victim): one read, then walk it in memory.
    uint64_t end_grain = grain_start + grain_nr - seg_md_grains_;
    IoBuffer seg_buf;
    int rc = read_segment(grain_start, end_grain - grain_start, seg_buf);
    if (rc != 0) return -rc;
    const auto* seg = static_cast<const uint8_t*>(seg_buf.data);

    uint64_t grain = grain_start;
    while (grain < end_grain && rc == 0) {
        const uint8_t* rec = seg + (grain - grain_start) * grain_size_;
        KvHeader hdr;
        std::memcpy(&hdr, rec, sizeof(hdr));
        const size_t val_bytes = record_val_bytes(hdr);
        uint64_t entry_grains = kv_total_grains(hdr.key_size, val_bytes);
        // Grains not holding a record of this segment's life (never
        // written, or left over from its previous use) are skipped.
        if (hdr.key_size == 0 || hdr.timestamp != victim_ts ||
            entry_grains > HashEntry::kKvSizeMask ||
            grain + entry_grains > end_grain) {
            ++grain;
            continue;
        }

        KvSuffix suffix;
        std::memcpy(&suffix,
                    rec + sizeof(KvHeader) + hdr.key_size + val_bytes,
                    sizeof(suffix));
        if (suffix.crc16 != compute_crc16(hdr)) {
            ++grain;
            continue;
        }

        rc = gc_record(grain, entry_grains, rec, drop_tombstones);
        grain += entry_grains;
    }
    if (rc != 0) return rc;

    // Readers that looked up an entry before it moved may still be reading
    // this segment; it is reused only after they are done.
    rcu_.synchronize();
    seg_live_[victim].store(false, std::memory_order_release);
    return 0;
}

template <typename IO>
void UDepot<IO>::seg_md_callback(u64 grain_start, u64 /*grain_nr*/) {
    uint64_t ts = get_seg_alloc_nr();
    uint64_t seg_idx = scm_->grain_to_seg_idx(grain_start);
    if (seg_idx >= num_segments_) return;
    seg_timestamps_[seg_idx].store(ts, std::memory_order_release);
    // As uDepot's persist_seg_md: written before salsa stages the segment,
    // so it is on the device before any grain of it is handed out. This
    // may run on whichever thread found the stage queue empty, including an
    // I/O poller, hence the blocking write.
    bool ok = persist_seg_md(grain_start, ts) == 0;
    seg_md_ok_[seg_idx].store(ok, std::memory_order_release);
    seg_live_[seg_idx].store(true, std::memory_order_release);
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
    const uint16_t tomb_grains =
        static_cast<uint16_t>(kv_total_grains(key.size(), 0));
    for (uint32_t start = 0; ; ) {
        HashEntry entry = directory_->lookup(hash, start,
                                             /*include_deleted=*/true);
        if (entry.empty()) break;
        start = entry.bucket_offset() + 1;
        if (probe.find(entry.pba())) continue;

        // A deleted entry points at a tombstone, which holds the key.
        int vrc = co_await verify_key_at_pba(
            entry.pba(), entry.deleted() ? tomb_grains : entry.kv_size(),
            key, nullptr);
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
        HashEntry entry = table.lookup(hash, start, /*include_deleted=*/true);
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
                           HashEntry* replaced, uint64_t* gen) {
    auto locked = directory_->lock_for(hash);
    if (locked.frozen()) {
        *gen = locked.snapshot->generation;
        return kFrozen;
    }
    HashTable& table = *locked.table;

    HashEntry match;
    if (!probe_settled(table, hash, probe, &match)) return kRetryProbe;

    // The key's own deleted entry means the key is absent, but its
    // tombstone still orders this write.
    bool exists = !match.empty() && !match.deleted();
    if (!exists) {
        if (mode == PutMode::kReplace || if_version != kAnyVersion)
            return -ENOENT;
    } else {
        if (mode == PutMode::kCreate) return -EEXIST;
        if (if_version != kAnyVersion && match.pba() != if_version)
            return -ESTALE;
    }

    if (match.empty()) {
        if (table.insert_locked(hash, kv_grains, pba) == 0) return 0;
        if (locked.snapshot->table_bits >= DirSnapshot::kMaxTableBits)
            return -ENOSPC;
        *gen = locked.snapshot->generation;
        return kTableFull;
    }
    // Recovery keeps the newer of two records of a key, tombstones
    // included, so the record the directory ends up pointing to must be
    // the newer one (legacy is_pba_order_equal_to_total_order).
    *replaced = match;
    if (!newer_than(pba, match.pba())) return kRewrite;

    table.update_locked(hash, match.pba(), kv_grains, pba);
    return 0;
}

template <typename IO>
int UDepot<IO>::check_sizes(size_t key_size, size_t val_size) const {
    if (key_size == 0 || key_size > UINT16_MAX) return -EINVAL;
    if (val_size >= kTombstoneValSize) return -EINVAL;  // reserved
    if (kv_total_grains(key_size, val_size) > HashEntry::kKvSizeMask)
        return -EINVAL;
    return 0;
}

template <typename IO>
PutBuffer UDepot<IO>::alloc_put_buffer(size_t key_size, size_t val_size) {
    PutBuffer pb;
    if (check_sizes(key_size, val_size) != 0) return pb;
    size_t total = kv_total_grains(key_size, val_size) * grain_size_;
    pb.buf_ = io_.alloc_buffer(total);
    if (!pb.buf_.data) return pb;
    pb.buf_.length = total;
    pb.key_size_ = static_cast<uint16_t>(key_size);
    pb.val_size_ = static_cast<uint32_t>(val_size);
    // The grain padding past the record is written too; put() never
    // touches it, so zeroing it once covers every reuse of the buffer.
    size_t used = kv_total_bytes(key_size, val_size);
    std::memset(static_cast<uint8_t*>(pb.buf_.data) + used, 0, total - used);
    return pb;
}

// Not coroutines: both hand over to put_record(), so the copying put
// costs one coroutine frame, like the zero-copy one.
template <typename IO>
CoroTask<int> UDepot<IO>::put(std::span<const uint8_t> key,
                              std::span<const uint8_t> val,
                              PutMode mode, uint64_t if_version) {
    PutBuffer rec;
    int rc = check_sizes(key.size(), val.size());
    if (rc == 0) {
        rec = alloc_put_buffer(key.size(), val.size());
        if (rec.valid())
            std::memcpy(rec.value().data(), val.data(), val.size());
        else
            rc = -ENOMEM;
    }
    return put_record(key, nullptr, std::move(rec), rc, mode, if_version);
}

template <typename IO>
CoroTask<int> UDepot<IO>::put(std::span<const uint8_t> key, PutBuffer& val,
                              PutMode mode, uint64_t if_version) {
    int rc = (!val.valid() || key.size() != val.key_size_) ? -EINVAL : 0;
    return put_record(key, &val, PutBuffer{}, rc, mode, if_version);
}

template <typename IO>
CoroTask<int> UDepot<IO>::put_record(std::span<const uint8_t> key_in,
                                     PutBuffer* zc, PutBuffer owned,
                                     int prep_rc, PutMode mode,
                                     uint64_t if_version) {
    if (prep_rc != 0) co_return prep_rc;
    PutBuffer& rec = zc ? *zc : owned;
    IoBuffer& buf = rec.buf_;
    const size_t val_size = rec.val_size_;
    const size_t total = buf.length;
    const uint64_t grains_needed = kv_total_grains(key_in.size(), val_size);

    // The record already holds the value. Copy the key in before the first
    // suspension and use that copy from here on, so the caller's key need
    // not outlive it.
    auto* p = static_cast<uint8_t*>(buf.data);
    KvHeader hdr;
    hdr.key_size = static_cast<uint16_t>(key_in.size());
    hdr.val_size = static_cast<uint32_t>(val_size);
    std::memcpy(p + sizeof(hdr), key_in.data(), key_in.size());
    std::span<const uint8_t> key(p + sizeof(hdr), key_in.size());
    uint64_t hash = hash_key(key);

    std::optional<Rcu::ReadGuard> guard(std::in_place, rcu_);

    KeyProbe probe;
    for (;;) {
        uint64_t grain;
        int arc = co_await allocate_or_wait(grains_needed, &grain, guard,
                                            &probe);
        if (arc != 0) co_return arc;

        hdr.timestamp = seg_timestamps_[scm_->grain_to_seg_idx(grain)].load(
            std::memory_order_acquire);
        std::memcpy(p, &hdr, sizeof(hdr));
        KvSuffix suffix;
        suffix.crc16 = compute_crc16(hdr);
        std::memcpy(p + sizeof(hdr) + key.size() + val_size,
                    &suffix, sizeof(suffix));

        ssize_t written = co_await io_.pwrite(buf.data, total,
                                              grain_to_offset(grain));
        if (written != static_cast<ssize_t>(total)) {
            invalidate_grains(grain, grains_needed);
            release_grains(grain, grains_needed);
            co_return (written < 0) ? static_cast<int>(written) : -EIO;
        }

        int rc;
        HashEntry replaced;
        for (;;) {
            rc = co_await probe_key(hash, key, probe);
            if (rc != 0) break;
            uint64_t gen = 0;
            rc = commit_put(hash, probe, mode, if_version,
                            static_cast<uint16_t>(grains_needed), grain,
                            &replaced, &gen);
            if (rc == kRetryProbe) continue;
            if (rc != kTableFull && rc != kFrozen) break;
            // As in uDepot: a full table grows the directory, and a write
            // that meets a grow waits for it; either way it then retries.
            rc = co_await wait_for_grow(gen, rc == kTableFull, guard, &probe);
            if (rc != 0) break;
        }

        if (rc == 0) {
            if (!replaced.empty())
                invalidate_grains(replaced.pba(),
                                  replaced.deleted()
                                      ? kv_total_grains(key.size(), 0)
                                      : replaced.kv_size());
            release_grains(grain, grains_needed);
            co_return 0;
        }

        invalidate_grains(grain, grains_needed);
        release_grains(grain, grains_needed);
        if (rc != kRewrite) co_return rc;

        // A concurrent write of this key (or its delete) was allocated
        // after us but committed first. Write again at a newer location; if
        // it sits in another segment, close ours so the next allocation is
        // newer still.
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
    return get_record(key, nullptr, val_out, val_buf_size, val_size_out,
                      version_out);
}

template <typename IO>
CoroTask<int> UDepot<IO>::get(std::span<const uint8_t> key,
                              GetBuffer* val_out, uint64_t* version_out) {
    if (val_out) *val_out = GetBuffer{};
    return get_record(key, val_out, nullptr, 0, nullptr, version_out);
}

template <typename IO>
CoroTask<int> UDepot<IO>::get_record(std::span<const uint8_t> key,
                                     GetBuffer* zc, uint8_t* val_out,
                                     size_t val_buf_size,
                                     size_t* val_size_out,
                                     uint64_t* version_out) {
    if (key.empty()) co_return -EINVAL;

    uint64_t hash = hash_key(key);
    Rcu::ReadGuard guard(rcu_);

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
        if (zc) {
            // Zero copy: the caller gets the buffer the record was read into.
            zc->val_off_ = sizeof(hdr) + hdr.key_size;
            zc->val_size_ = hdr.val_size;
            zc->buf_ = std::move(buf);
        } else if (val_out && val_buf_size > 0) {
            size_t to_copy = std::min(val_buf_size,
                                      static_cast<size_t>(hdr.val_size));
            std::memcpy(val_out, p + sizeof(hdr) + hdr.key_size, to_copy);
        }

        co_return 0;
    }

    co_return -ENOENT;
}

template <typename IO>
CoroTask<int> UDepot<IO>::write_tombstone(
    std::span<const uint8_t> key, uint64_t* tomb_out,
    std::optional<Rcu::ReadGuard>& guard, KeyProbe* probe) {
    uint64_t tomb_grains = kv_total_grains(key.size(), 0);
    uint64_t tomb_grain;
    int arc = co_await allocate_or_wait(tomb_grains, &tomb_grain, guard,
                                        probe);
    if (arc != 0) co_return arc;

    size_t tomb_total = tomb_grains * grain_size_;
    IoBuffer tomb_buf = io_.alloc_buffer(tomb_total);
    if (!tomb_buf.data) {
        invalidate_grains(tomb_grain, tomb_grains);
        release_grains(tomb_grain, tomb_grains);
        co_return -ENOMEM;
    }
    auto* tp = static_cast<uint8_t*>(tomb_buf.data);
    KvHeader tomb_hdr;
    tomb_hdr.key_size = static_cast<uint16_t>(key.size());
    tomb_hdr.val_size = kTombstoneValSize;  // holds no value bytes
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

    ssize_t w = co_await io_.pwrite(tomb_buf.data, tomb_total,
                                    grain_to_offset(tomb_grain));
    if (w != static_cast<ssize_t>(tomb_total)) {
        invalidate_grains(tomb_grain, tomb_grains);
        release_grains(tomb_grain, tomb_grains);
        co_return (w < 0) ? static_cast<int>(w) : -EIO;
    }
    *tomb_out = tomb_grain;
    co_return 0;
}

template <typename IO>
int UDepot<IO>::commit_del(uint64_t hash, const KeyProbe& probe,
                           uint64_t if_version, uint64_t tomb_pba,
                           HashEntry* removed, uint64_t* gen) {
    auto locked = directory_->lock_for(hash);
    if (locked.frozen()) {
        *gen = locked.snapshot->generation;
        return kFrozen;
    }
    HashTable& table = *locked.table;

    HashEntry match;
    if (!probe_settled(table, hash, probe, &match)) return kRetryProbe;
    if (match.empty() || match.deleted()) return -ENOENT;
    if (if_version != kAnyVersion && match.pba() != if_version)
        return -ESTALE;
    *removed = match;
    if (tomb_pba == UINT64_MAX) return kNeedTomb;
    // As in commit_put: recovery must see the tombstone as newer than the
    // value it deletes.
    if (!newer_than(tomb_pba, match.pba())) return kRewrite;

    // As uDepot's map remove(): the entry stays, deleted, pointing at the
    // tombstone, so a later put of the key is ordered against it.
    table.update_locked(hash, match.pba(), 0, tomb_pba);
    return 0;
}

template <typename IO>
CoroTask<int> UDepot<IO>::del(std::span<const uint8_t> key,
                              uint64_t if_version) {
    if (key.empty() || key.size() > UINT16_MAX) co_return -EINVAL;

    uint64_t hash = hash_key(key);
    std::optional<Rcu::ReadGuard> guard(std::in_place, rcu_);

    const uint64_t tomb_grains = kv_total_grains(key.size(), 0);
    uint64_t tomb = UINT64_MAX;
    KeyProbe probe;
    for (;;) {
        HashEntry removed;
        uint64_t gen = 0;
        int rc = co_await probe_key(hash, key, probe);
        if (rc == 0)
            rc = commit_del(hash, probe, if_version, tomb, &removed, &gen);

        if (rc == kRetryProbe) continue;
        if (rc == kFrozen) {
            rc = co_await wait_for_grow(gen, false, guard, &probe);
            if (rc == 0) continue;
        }
        if (rc == 0) {
            invalidate_grains(removed.pba(), removed.kv_size());
            release_grains(tomb, tomb_grains);  // stays valid, referenced
            co_return 0;
        }
        if (rc == kNeedTomb) {
            // Written unlocked; commit_del re-checks the entry it deletes.
            rc = co_await write_tombstone(key, &tomb, guard, &probe);
            if (rc != 0) co_return rc;
            continue;
        }

        // The tombstone is not needed (key gone, version changed) or not
        // newer than the entry it would delete (kRewrite).
        if (tomb != UINT64_MAX) {
            invalidate_grains(tomb, tomb_grains);
            release_grains(tomb, tomb_grains);
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

// Copyright (c) 2024-2026 Nikolas Ioannou
// SPDX-License-Identifier: BSD-3-Clause

#pragma once

#include <array>
#include <atomic>
#include <cerrno>
#include <condition_variable>
#include <coroutine>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <memory>
#include <mutex>
#include <optional>
#include <span>
#include <string_view>
#include <thread>
#include <utility>
#include <vector>

#include "udepot/buffer.h"
#include "udepot/coro.h"
#include "udepot/directory.h"
#include "udepot/hash_entry.h"
#include "udepot/rcu.h"

#include "frontends/usalsa++/SalsaCtlr.hh"
#include "frontends/usalsa++/SalsaMD.hh"

#include "city.h"

namespace udepot {

// On-disk KV entry layout (unchanged from uDepot):
//   [u16 key_size][u32 val_size][u64 timestamp][key][value][u16 crc16]
// Total: 16 + key_size + val_size bytes.
struct __attribute__((packed)) KvHeader {
    uint16_t key_size;
    uint32_t val_size;
    uint64_t timestamp;
};

struct __attribute__((packed)) KvSuffix {
    uint16_t crc16;
};

static_assert(sizeof(KvHeader) == 14);
static_assert(sizeof(KvSuffix) == 2);

inline constexpr size_t kKvOverhead = sizeof(KvHeader) + sizeof(KvSuffix);

// A tombstone is a record whose val_size is kTombstoneValSize and that holds
// no value bytes. Values may be empty (the uDepot paper, section 4: "no
// minimum size"), so val_size 0 cannot mark a delete; the largest u32 can,
// since no value that long can be stored (check_sizes).
inline constexpr uint32_t kTombstoneValSize = UINT32_MAX;

inline bool is_tombstone(const KvHeader& hdr) noexcept {
    return hdr.val_size == kTombstoneValSize;
}

// Value bytes the record actually holds.
inline size_t record_val_bytes(const KvHeader& hdr) noexcept {
    return is_tombstone(hdr) ? 0 : hdr.val_size;
}

struct StoreConfig {
    const char* path = nullptr;
    size_t size = 0;
    uint32_t grain_size = 512;
    uint32_t initial_tables = 2;
    uint32_t index_bits = 10;
    // Segment size in grains. 0 = auto (uDepot default: (1<<29)/grain_size + 2,
    // halved until it fits the device).
    uint64_t segment_size = 0;
    // Overprovision in per-mille (200 = 20% reserved for GC).
    uint32_t overprovision = 200;
    // Destroy any existing data on the device and start fresh.
    bool force_destroy = false;
    // I/Os the caller expects to have in flight at once (per thread on
    // SPDK). It sizes the backend's queue (AIO context, io_uring ring,
    // SPDK request pool); uDepot allocates no data buffers for it, see
    // alloc_put_buffer() and alloc_get_buffer(). An operation whose I/O
    // the backend refuses because its queue is full fails with -EAGAIN.
    // 0 = the backend's default.
    unsigned queue_depth = 0;
};

// Condition a put() must satisfy, checked atomically with the write.
enum class PutMode {
    kUpsert,   // insert or overwrite
    kCreate,   // fail with -EEXIST if the key exists
    kReplace,  // fail with -ENOENT if the key does not exist
};

// A version identifies one stored value of a key; every successful put
// yields a new one, and none ever repeats. get() reports it, and put()/del()
// accept it to act only if the key still holds that value (-ESTALE
// otherwise). GC moving a value also changes its version: a conditional
// put/del after that fails with -ESTALE though the value is the same, a
// failure the caller handles by reading again, as for a real change.
inline constexpr uint64_t kAnyVersion = UINT64_MAX;

// Zero-copy values (uDepot's Mbuff interface). A PutBuffer, from
// UDepot::alloc_put_buffer(), is laid out as the on-disk record with room
// around the value: the caller writes the value in place and put() writes
// the buffer to storage as is, filling in header, key and checksum. A
// GetBuffer receives the buffer get() read the record into, and value()
// views the value inside it. Both are movable, not copyable.
class PutBuffer {
public:
    PutBuffer() = default;
    PutBuffer(PutBuffer&&) noexcept = default;
    PutBuffer& operator=(PutBuffer&&) noexcept = default;

    bool valid() const noexcept { return buf_.data != nullptr; }
    std::span<uint8_t> value() noexcept {
        return {static_cast<uint8_t*>(buf_.data) + sizeof(KvHeader) +
                    key_size_,
                val_size_};
    }
    size_t key_size() const noexcept { return key_size_; }

private:
    template <typename> friend class UDepot;
    IoBuffer buf_;
    uint16_t key_size_ = 0;
    uint32_t val_size_ = 0;
};

class GetBuffer {
public:
    GetBuffer() = default;
    GetBuffer(GetBuffer&&) noexcept = default;
    GetBuffer& operator=(GetBuffer&&) noexcept = default;

    bool valid() const noexcept { return buf_.data != nullptr; }
    std::span<const uint8_t> value() const noexcept {
        return {static_cast<const uint8_t*>(buf_.data) + val_off_, val_size_};
    }

private:
    template <typename> friend class UDepot;
    IoBuffer buf_;
    size_t val_off_ = 0;
    uint32_t val_size_ = 0;
};

// High-performance KV store, parameterized on the I/O backend.
//
// Uses RCU-protected directory for lock-free reads, salsa for grain
// allocation and GC, and the IoBackend for storage I/O.
template <typename IO>
class UDepot : private salsa::SalsaCtlr {
public:
    UDepot();
    ~UDepot();

    UDepot(const UDepot&) = delete;
    UDepot& operator=(const UDepot&) = delete;

    // As in uDepot, the caller orders open() and close() against
    // operations: none may start before open() returns, run concurrently
    // with close(), or follow it.
    //
    // Any operation fails with -EAGAIN if the backend cannot take another
    // I/O (its queue is full of the caller's operations); it may be retried
    // once some of those complete. See StoreConfig::queue_depth.
    int open(const StoreConfig& config);
    void close();

    // A non-default if_version implies kReplace: -ENOENT if the key is
    // missing, -ESTALE if it holds a different version.
    //
    // Copies the value into a record buffer; see the zero-copy overload.
    CoroTask<int> put(std::span<const uint8_t> key,
                      std::span<const uint8_t> val,
                      PutMode mode = PutMode::kUpsert,
                      uint64_t if_version = kAnyVersion);

    // Copies the value out of the record it reads; see the zero-copy
    // overload.
    CoroTask<int> get(std::span<const uint8_t> key,
                      uint8_t* val_out, size_t val_buf_size,
                      size_t* val_size_out,
                      uint64_t* version_out = nullptr);

    // Zero copy. A buffer for a key of key_size bytes and a value of
    // val_size bytes, to fill through value() and pass to put(); invalid
    // (!valid()) if the sizes cannot be stored or allocation fails.
    PutBuffer alloc_put_buffer(size_t key_size, size_t val_size);

    // Zero copy: writes `val` in place. key.size() must be the size the
    // buffer was allocated for (-EINVAL otherwise). The buffer stays the
    // caller's, and must stay alive and unmodified until the put completes;
    // it may be reused afterwards. The key is copied before the first
    // suspension.
    CoroTask<int> put(std::span<const uint8_t> key, PutBuffer& val,
                      PutMode mode = PutMode::kUpsert,
                      uint64_t if_version = kAnyVersion);

    // Zero copy, for gets: a buffer that holds a record of a key of
    // key_size bytes and a value of up to val_size bytes. Invalid
    // (!valid()) if the sizes cannot be stored or allocation fails. Use it
    // with the store that allocated it: on SPDK that is DMA memory, and
    // another buffer still works but is copied through one.
    GetBuffer alloc_get_buffer(size_t key_size, size_t val_size);

    // Zero copy: on success, *val_out holds the buffer the record was read
    // into, and val_out->value() the value. The record is read into
    // val_out's own buffer when it is large enough (from alloc_get_buffer()
    // or an earlier get), so a caller can reuse its buffers; otherwise
    // into a new one, which replaces it. On error value() is empty and the
    // buffer is kept. The buffer must stay alive until the get completes.
    CoroTask<int> get(std::span<const uint8_t> key, GetBuffer* val_out,
                      uint64_t* version_out = nullptr);

    CoroTask<int> del(std::span<const uint8_t> key,
                      uint64_t if_version = kAnyVersion);

    CoroTask<int> exists(std::span<const uint8_t> key,
                         size_t* val_size_out);

    // Convenience overloads for string keys/values.
    static std::span<const uint8_t> as_bytes(std::string_view s) {
        return {reinterpret_cast<const uint8_t*>(s.data()), s.size()};
    }

    CoroTask<int> put(std::string_view key, std::string_view val,
                      PutMode mode = PutMode::kUpsert,
                      uint64_t if_version = kAnyVersion) {
        return put(as_bytes(key), as_bytes(val), mode, if_version);
    }

    CoroTask<int> get(std::string_view key, uint8_t* val_out,
                      size_t val_buf_size, size_t* val_size_out,
                      uint64_t* version_out = nullptr) {
        return get(as_bytes(key), val_out, val_buf_size, val_size_out,
                   version_out);
    }

    CoroTask<int> del(std::string_view key,
                      uint64_t if_version = kAnyVersion) {
        return del(as_bytes(key), if_version);
    }

    CoroTask<int> exists(std::string_view key, size_t* val_size_out) {
        return exists(as_bytes(key), val_size_out);
    }

    uint64_t hash_key(std::span<const uint8_t> key) const {
        return CityHash64(reinterpret_cast<const char*>(key.data()),
                          key.size());
    }

    // Test seam, never set in production: GC calls it after writing a
    // relocated copy and before repointing the key's directory entry, with
    // the key's stripes held. Tests use it to widen the window a racing put
    // or del would fall into.
    // The hooks are atomic: tests set and clear them while the store's
    // threads may be reading them.
    using KeyHook = void (*)(std::span<const uint8_t> key);
    inline static std::atomic<KeyHook> gc_relocation_test_hook{nullptr};

    // Test seam, never set in production: a get calls it after looking up
    // a record and before reading it, inside its read-side section. Tests
    // use it to let writers recycle the record's segment meanwhile.
    inline static std::atomic<KeyHook> get_read_test_hook{nullptr};

    // Test seam, never set in production: GC calls it for every record it
    // relocates, with the timestamps of the victim segment and of the
    // segment the copy went to.
    using RelocationHook = void (*)(std::span<const uint8_t> key,
                                    uint64_t victim_ts, uint64_t dst_ts);
    inline static std::atomic<RelocationHook> gc_relocation_order_test_hook{
        nullptr};

    // Test seam, never set in production: GC calls it when it drops a
    // deleted key's tombstone, with the timestamp of the victim segment.
    // Test seams, never set in production: seg_md_enter_test_hook runs as
    // a new KV segment's metadata callback starts, before it takes a
    // timestamp; seg_md_test_hook is called with the timestamp it took.
    using SegEnterHook = void (*)();
    inline static std::atomic<SegEnterHook> seg_md_enter_test_hook{nullptr};
    using SegTimestampHook = void (*)(uint64_t ts);
    inline static std::atomic<SegTimestampHook> seg_md_test_hook{nullptr};

    using TombstoneDropHook = void (*)(std::span<const uint8_t> key,
                                       uint64_t victim_ts);
    inline static std::atomic<TombstoneDropHook> gc_tombstone_drop_test_hook{
        nullptr};

    // Test seam, never set in production: close() calls it before writing
    // each index footer; returning false stops the flush there, as a crash
    // would.
    using FooterHook = bool (*)();
    inline static std::atomic<FooterHook> index_footer_test_hook{nullptr};

    Directory& directory() { return *directory_; }
    const Directory& directory() const { return *directory_; }
    Rcu& rcu() { return rcu_; }
    IO& io() { return io_; }
    uint32_t grain_size() const { return grain_size_; }
    // The device seed: binds segment metadata and records to this store.
    uint64_t seed() const noexcept { return seed_; }

private:
    Rcu rcu_;
    IO io_;
    Directory* directory_ = nullptr;
    uint32_t grain_size_ = 512;
    uint64_t total_grains_ = 0;

    // Salsa segment allocator.
    salsa::Scm* scm_ = nullptr;
    uint64_t seg_md_grains_ = 0;

    // Crash recovery metadata.
    uint64_t seed_ = 0;
    uint64_t num_segments_ = 0;
    std::unique_ptr<std::atomic<uint64_t>[]> seg_timestamps_;
    // Whether the segment's metadata reached the device (written at
    // allocation, in seg_md_callback).
    std::unique_ptr<std::atomic<bool>[]> seg_md_ok_;
    // Segments whose records crash recovery could replay: allocated (or
    // restored) and not yet reused. A segment GC has reclaimed keeps its
    // metadata and old records on disk until it is reused. GC uses it to
    // tell whether a tombstone can still matter.
    std::unique_ptr<std::atomic<bool>[]> seg_live_;

    // Waiting for free space or for a directory grow. When salsa has no
    // segment staged, or the key's directory snapshot is being grown, an
    // operation suspends here instead of blocking its thread (which may be
    // an I/O poller that in-flight I/O needs), outside its read-side
    // section (GC needs a grace period to free a segment, and a grow to
    // copy the tables). The waker thread runs requested grows and resumes
    // waiters to retry, as uDepot's tasks yielded to the TRT scheduler.
    struct SpaceWait {
        UDepot* store;
        // A grow of this snapshot generation to run, if any.
        uint64_t grow = Directory::kAnyGeneration;
        bool await_ready() noexcept { return false; }
        bool await_suspend(std::coroutine_handle<> h);
        void await_resume() noexcept {}
    };
    void space_waker_loop();
    void stop_space_waker();
    void resume_waiters(std::unique_lock<std::mutex>& lock);
    std::mutex space_mu_;
    std::condition_variable space_cv_;
    std::vector<std::coroutine_handle<>> space_waiters_;  // space_mu_
    std::optional<uint64_t> grow_request_;                // space_mu_
    bool space_stop_ = false;                             // space_mu_
    std::thread space_waker_;

    // SalsaCtlr overrides — called from salsa's GC thread.
    int gc_callback(u64 grain_start, u64 grain_nr) override;
    // Salsa's defer_free_seg: frees a segment whose last grain was
    // invalidated only after a grace period.
    static void defer_free_seg(void* arg, struct segment* seg);
    void seg_md_callback(u64 grain_start, u64 grain_nr,
                         u64 alloc_nr) override;

    // Never blocks: 0, -EAGAIN (no segment staged yet), -ENOSPC, or -EIO
    // if the segment's metadata write failed.
    int try_allocate_grains(uint64_t count, uint64_t* grain);
    void invalidate_grains(uint64_t grain, uint64_t count, bool reloc = false);
    // As uDepot: every allocation is released once its write has been
    // committed or invalidated. Salsa seals a segment, making it a GC
    // candidate, only when all of its grains are released.
    void release_grains(uint64_t grain, uint64_t count, bool reloc = false);

    uint16_t compute_crc16(const KvHeader& hdr) const;
    static uint32_t compute_crc32(uint32_t seed, const uint8_t* data,
                                  size_t len);

    // Device metadata: sits at the tail past the last whole segment.
    uint64_t dev_md_grain_offset() const;
    int persist_dev_md();
    bool validate_dev_md(salsa::salsa_dev_md* md_out);

    // Segment metadata: written at the tail of each segment. ctlr_type is
    // the owning controller's id: KV segments are ours, index segments
    // index_ctlr_'s.
    int persist_seg_md(uint64_t grain_start, uint64_t timestamp,
                       uint8_t ctlr_type);
    bool validate_seg_md(const salsa::salsa_seg_md& md) const;
    // Reads and validates the metadata of the segment starting at seg_base.
    bool read_seg_md(uint64_t seg_base, salsa::salsa_seg_md* md);

    // uDepot's own synchronous I/O (recovery, index persistence, GC). It
    // shares the backend's queue with the caller's operations, so a
    // submission refused for a full queue (-EAGAIN) is retried once some
    // complete: only the caller's own operations report -EAGAIN.
    template <typename Start>
    static auto run_internal(Start&& start);
    ssize_t pread_internal(void* buf, size_t count, off_t offset) {
        return run_internal([&] { return io_.pread(buf, count, offset); });
    }
    ssize_t pwrite_internal(const void* buf, size_t count, off_t offset) {
        return run_internal([&] { return io_.pwrite(buf, count, offset); });
    }

    // ── Index segments (paper §4.4) ─────────────────────────────────────
    // close() flushes the hash tables to index segments, and an open after
    // a clean shutdown restores them instead of scanning the log; the log
    // stays the source of truth after a crash. As uDepot's
    // uDepotDirectoryMap, the index has a salsa controller of its own: a
    // net-segment allocation fills exactly one segment, and its segments'
    // metadata carry its type, so the log scan skips them.
    class IndexCtlr final : public salsa::SalsaCtlr {
    public:
        explicit IndexCtlr(UDepot* store) : store_(store) {}

    private:
        // Never holds anything GC could move (uDepot: "this should not
        // happen").
        int gc_callback(u64, u64) override { return ENOSYS; }
        void seg_md_callback(u64 grain_start, u64, u64) override {
            store_->index_seg_md_callback(grain_start);
        }
        UDepot* store_;
    };
    std::unique_ptr<IndexCtlr> index_ctlr_;
    // Newest index timestamp seen on the device; the next flush is newer.
    uint64_t index_ts_ = 0;
    void index_seg_md_callback(uint64_t md_grain);
    // Write every table to index segments, footers last. Returns 0 or
    // -errno; on failure no complete index is left, and the next open
    // scans the log.
    int flush_index();
    int allocate_index_segment(uint64_t net_grains, u64* grain);
    // Restore the newest complete index, if there is one (*restored), and
    // invalidate every valid index footer on the device either way, so a
    // later crash cannot bring back an index older than the log. Returns
    // -errno only if that invalidation failed.
    int restore_index(bool* restored);
    CoroTask<int> tombstone_grains(uint64_t pba, uint64_t seg_ts,
                                   uint64_t* grains);
    template <typename T>
    uint32_t index_md_csum(const T& md) const;

    // Read `grains` grains from `grain` into buf (reallocated if too
    // small). Returns 0 or -errno.
    int read_segment(uint64_t grain, uint64_t grains, IoBuffer& buf);

    // Full crash recovery: scan all segments, rebuild directory.
    int crash_recovery();
    int recover_record(uint64_t hash, std::span<const uint8_t> key,
                       uint64_t grain, uint16_t kv_grains, bool tombstone);

    size_t kv_total_bytes(size_t key_size, size_t val_size) const {
        return sizeof(KvHeader) + key_size + val_size + sizeof(KvSuffix);
    }

    uint64_t kv_total_grains(size_t key_size, size_t val_size) const {
        size_t bytes = kv_total_bytes(key_size, val_size);
        return (bytes + grain_size_ - 1) / grain_size_;
    }

    off_t grain_to_offset(uint64_t grain) const {
        return static_cast<off_t>(grain) * grain_size_;
    }

    // Read the on-disk header at a given PBA and verify the key matches.
    // Returns 0 if the key matches, ENOENT if it doesn't.
    CoroTask<int> verify_key_at_pba(uint64_t pba, uint16_t kv_grains,
                                    std::span<const uint8_t> key,
                                    KvHeader* hdr_out);

    // Lookup-before-write (legacy lookup_mbuff_put). Tags are a filter, so
    // finding a key's entry takes a key-verify read per tag match. That I/O
    // runs unlocked (probe_key), and the write then re-checks under the
    // table lock that every tag-matching entry is one already verified
    // (probe_settled); a new one sends it back to probe_key. The decision
    // and the directory write are therefore one critical section.
    struct KeyProbe {
        struct Seen {
            uint64_t pba;
            bool match;
        };
        std::array<Seen, HashEntry::kHopRange> seen{};
        uint32_t n = 0;

        const Seen* find(uint64_t pba) const noexcept {
            for (uint32_t i = 0; i < n; ++i)
                if (seen[i].pba == pba) return &seen[i];
            return nullptr;
        }
    };

    CoroTask<int> probe_key(uint64_t hash, std::span<const uint8_t> key,
                            KeyProbe& probe);
    static bool probe_settled(const HashTable& table, uint64_t hash,
                              const KeyProbe& probe, HashEntry* match);

    // Validates sizes; 0 or -EINVAL.
    int check_sizes(size_t key_size, size_t val_size) const;
    // The one put path: writes `*zc` if non-null (zero copy), else
    // `owned` (the copying put's record, moved into this frame).
    CoroTask<int> put_record(std::span<const uint8_t> key, PutBuffer* zc,
                             PutBuffer owned, int prep_rc, PutMode mode,
                             uint64_t if_version);
    // The one get path: hands the record buffer to *zc if non-null, else
    // copies the value to val_out.
    CoroTask<int> get_record(std::span<const uint8_t> key, GetBuffer* zc,
                             uint8_t* val_out, size_t val_buf_size,
                             size_t* val_size_out, uint64_t* version_out);

    // Results of commit_put/commit_del besides 0 and -errno.
    static constexpr int kRetryProbe = 1;  // unverified entry appeared
    static constexpr int kRewrite = 2;     // our write is not newer than
                                           // the entry (reported out)
    static constexpr int kNeedTomb = 3;    // del: write the tombstone
    static constexpr int kTableFull = 4;   // put: grow the directory
    static constexpr int kFrozen = 5;      // a grow is copying the tables

    // On kTableFull or kFrozen, *gen is the snapshot's generation.
    int commit_put(uint64_t hash, const KeyProbe& probe, PutMode mode,
                   uint64_t if_version, uint16_t kv_grains, uint64_t pba,
                   HashEntry* replaced, uint64_t* gen);
    int commit_del(uint64_t hash, const KeyProbe& probe, uint64_t if_version,
                   uint64_t tomb_pba, HashEntry* removed, uint64_t* gen);

    // Wait, outside the read section, until the snapshot of `gen` has been
    // replaced, asking the waker to grow it if `grow`. As in
    // allocate_or_wait, `guard` is released and `probe` cleared meanwhile.
    CoroTask<int> wait_for_grow(uint64_t gen, bool grow,
                                std::optional<Rcu::ReadGuard>& guard,
                                KeyProbe* probe);

    // For threads that may block (GC): fn(table) on the key's table with
    // its stripes held, inside a read section, waiting out a grow first if
    // one has frozen the snapshot.
    template <typename F>
    auto with_table_blocking(uint64_t hash, F&& fn) {
        for (;;) {
            uint64_t frozen;
            {
                Rcu::ReadGuard guard(rcu_);
                auto locked = directory_->lock_for(hash);
                if (!locked.frozen()) return fn(*locked.table);
                frozen = locked.snapshot->generation;
            }
            directory_->wait_for_grow(frozen);
        }
    }

    // Whether data at new_pba is newer than data at old_pba in the order
    // crash recovery uses (segment timestamp, then grain).
    bool newer_than(uint64_t new_pba, uint64_t old_pba) const;
    // The version of the record at pba (see kAnyVersion).
    uint64_t version_of(uint64_t pba) const;

    // Allocate, waiting for space if needed (see SpaceWait). While
    // waiting, `guard` is released and `probe` is cleared: an unprotected
    // probe's classifications could go stale through pba reuse.
    CoroTask<int> allocate_or_wait(uint64_t count, uint64_t* grain,
                                   std::optional<Rcu::ReadGuard>& guard,
                                   KeyProbe* probe);

    CoroTask<int> write_tombstone(std::span<const uint8_t> key,
                                  uint64_t* tomb_out,
                                  std::optional<Rcu::ReadGuard>& guard,
                                  KeyProbe* probe);

    // GC: move one record still referenced by the directory to a new
    // location, or drop it if it is a tombstone that can no longer matter.
    int gc_record(uint64_t grain, uint64_t entry_grains, const uint8_t* rec,
                  bool drop_tombstones);
};

}  // namespace udepot

// Copyright (c) 2024-2026 Nikolas Ioannou
// SPDX-License-Identifier: BSD-3-Clause

#pragma once

#include <array>
#include <atomic>
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
};

// Condition a put() must satisfy, checked atomically with the write.
enum class PutMode {
    kUpsert,   // insert or overwrite
    kCreate,   // fail with -EEXIST if the key exists
    kReplace,  // fail with -ENOENT if the key does not exist
};

// A version identifies one stored value of a key; every successful put
// yields a new one. get() reports it, and put()/del() accept it to act only
// if the key still holds that value (-ESTALE otherwise).
inline constexpr uint64_t kAnyVersion = UINT64_MAX;

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

    // open() and close() must not run concurrently with each other.
    // Operations may race close(): it waits for the ones in progress, and
    // any that start after it fail with -ESHUTDOWN, as do operations on a
    // store that was never opened. The object must outlive every caller.
    int open(const StoreConfig& config);
    void close();

    // A non-default if_version implies kReplace: -ENOENT if the key is
    // missing, -ESTALE if it holds a different version.
    CoroTask<int> put(std::span<const uint8_t> key,
                      std::span<const uint8_t> val,
                      PutMode mode = PutMode::kUpsert,
                      uint64_t if_version = kAnyVersion);

    CoroTask<int> get(std::span<const uint8_t> key,
                      uint8_t* val_out, size_t val_buf_size,
                      size_t* val_size_out,
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

    Directory& directory() { return *directory_; }
    const Directory& directory() const { return *directory_; }
    Rcu& rcu() { return rcu_; }
    IO& io() { return io_; }
    uint32_t grain_size() const { return grain_size_; }

private:
    Rcu rcu_;
    IO io_;
    // Checked by every operation inside its RCU read section; close()
    // clears it and then waits a grace period. Own line: read on every op.
    alignas(64) std::atomic<bool> open_{false};
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
    // Segments holding data: allocated (or restored) and not yet reclaimed
    // by GC. GC uses it to tell whether a tombstone can still matter.
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
    void seg_md_callback(u64 grain_start, u64 grain_nr) override;

    // Never blocks: 0, -EAGAIN (no segment staged yet), -ENOSPC, or -EIO
    // if the segment's metadata write failed.
    int try_allocate_grains(uint64_t count, uint64_t* grain);
    void invalidate_grains(uint64_t grain, uint64_t count, bool reloc = false);
    // As uDepot: every allocation is released once its write has been
    // committed or invalidated. Salsa seals a segment, making it a GC
    // candidate, only when all of its grains are released.
    void release_grains(uint64_t grain, uint64_t count, bool reloc = false);

    static uint16_t compute_crc16(const KvHeader& hdr);
    static uint32_t compute_crc32(uint32_t seed, const uint8_t* data,
                                  size_t len);

    // Device metadata: sits at the tail past the last whole segment.
    uint64_t dev_md_grain_offset() const;
    int persist_dev_md();
    bool validate_dev_md(salsa::salsa_dev_md* md_out);

    // Segment metadata: written at the tail of each segment.
    int persist_seg_md(uint64_t grain_start, uint64_t timestamp);
    bool validate_seg_md(const salsa::salsa_seg_md& md) const;

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

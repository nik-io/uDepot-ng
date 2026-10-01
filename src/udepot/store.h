// Copyright (c) 2024-2026 Nikolas Ioannou
// SPDX-License-Identifier: BSD-3-Clause

#pragma once

#include <array>
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <memory>
#include <span>
#include <string_view>
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
    Directory* directory_ = nullptr;
    uint32_t grain_size_ = 512;
    uint64_t total_grains_ = 0;

    // Salsa segment allocator.
    salsa::Scm* scm_ = nullptr;
    uint64_t seg_md_grains_ = 0;
    Rcu::Token gc_rcu_token_{};

    // Crash recovery metadata.
    uint64_t seed_ = 0;
    uint64_t num_segments_ = 0;
    std::unique_ptr<std::atomic<uint64_t>[]> seg_timestamps_;
    std::unique_ptr<std::atomic<bool>[]> seg_md_dirty_;

    // SalsaCtlr overrides — called from salsa's GC thread.
    int gc_callback(u64 grain_start, u64 grain_nr) override;
    void seg_md_callback(u64 grain_start, u64 grain_nr) override;

    uint64_t allocate_grains(uint64_t count);
    void invalidate_grains(uint64_t grain, uint64_t count);

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

    // Full crash recovery: scan all segments, rebuild directory.
    int crash_recovery();

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

    // Per-thread RCU token.  Each calling thread lazily registers with
    // the RCU subsystem on first use.  Tokens are cleaned up when the
    // thread exits (thread_local destructor).  Safe only when coroutines
    // are driven by run_sync() on the calling thread — a coroutine that
    // migrates threads would need a different scheme.
    Rcu::Token thread_token();

    // Persist segment metadata for the segment containing `grain` if it
    // has not been written yet.  Called from coroutine context (put/del).
    CoroTask<int> ensure_seg_md(uint64_t grain);

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

    int commit_put(uint64_t hash, const KeyProbe& probe, PutMode mode,
                   uint64_t if_version, uint16_t kv_grains, uint64_t pba,
                   HashEntry* replaced);
    int commit_del(uint64_t hash, const KeyProbe& probe, uint64_t if_version,
                   uint64_t tomb_pba, HashEntry* removed);

    // Whether data at new_pba is newer than data at old_pba in the order
    // crash recovery uses (segment timestamp, then grain).
    bool newer_than(uint64_t new_pba, uint64_t old_pba) const;

    CoroTask<int> write_tombstone(std::span<const uint8_t> key,
                                  uint64_t* tomb_out);
};

}  // namespace udepot

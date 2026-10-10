// Copyright (c) 2024-2026 Nikolas Ioannou
// SPDX-License-Identifier: BSD-3-Clause

#pragma once

#include <atomic>
#include <cassert>
#include <condition_variable>
#include <cstdint>
#include <memory>
#include <mutex>
#include <vector>

#include "udepot/hash_table.h"
#include "udepot/rcu.h"

namespace udepot {

// How a full table grows the directory.
enum class ResizeMode {
    // Incremental, per lock region (paper §4.3). The default.
    kIncremental,
    // Kept to compare against: freeze writers, wait a grace period, copy
    // every table, publish. Readers carry on; writers wait out the copy.
    kFreeze,
};

// A resize in progress (paper §4.3): the previous geometry, half as many
// tables, being migrated into the snapshot's tables one lock region
// (stripe) at a time. Old table t splits into new tables 2t and 2t + 1 by
// the next tag bit; buckets do not change, so stripe s of t migrates into
// stripe s of both, slot for slot (paper Figure 4).
struct Resize {
    std::vector<std::shared_ptr<HashTable>> old;
    uint64_t stripes;  // per table, the same in the old and the new tables
    // Per (old table, stripe): migrated. Set, under the stripe's lock,
    // after its slots are copied; never cleared.
    std::unique_ptr<std::atomic<bool>[]> migrated;
    // Stripes still to migrate (the paper's "resize" counter).
    std::atomic<uint64_t> remaining;

    bool is_migrated(uint32_t old_table, uint64_t stripe) const noexcept {
        return migrated[old_table * stripes + stripe].load(
            std::memory_order_acquire);
    }
};

// The lock handover after a resize ends. Writers still on the resizing
// snapshot hold the old tables' stripe locks; for one grace period after
// the final snapshot is published, its writers take them too, before the
// new tables' own.
struct Handover {
    std::vector<HashTable*> old;  // freed a grace period after !active
    std::atomic<bool> active{true};
};

// A snapshot of the directory: 2^table_bits hash tables. Published once,
// replaced whole: stable -> resizing -> stable (with a handover) -> ...
//
// As in uDepot's uDepotDirectoryMap::hash_to_map, a key's table is chosen by
// the top bits of its tag, which every entry stores; the bucket comes from
// the low bits. Those must be different bits, or each table would only ever
// use a fraction of its buckets and growing would add no capacity.
struct DirSnapshot {
    static constexpr uint32_t kMaxTableBits = 8;  // the tag's width

    std::vector<std::shared_ptr<HashTable>> tables;
    uint32_t table_bits;
    // Counts snapshots. Identifies one after it is freed, when its address
    // may already belong to a newer one.
    uint64_t generation;
    // Set while the tables are being migrated into from `resize->old`.
    std::unique_ptr<Resize> resize;
    // Set on the snapshot a resize ends with.
    std::shared_ptr<Handover> handover;
    // Set before a resize replaces this (stable) snapshot. Its writers
    // check it under their stripe locks, the ones the resize migrates
    // under, and retry on the new snapshot.
    std::atomic<bool> superseded{false};
    // ResizeMode::kFreeze: set before the tables are copied. Writers check
    // it under their stripe locks and back off until the copy is published.
    std::atomic<bool> frozen{false};

    // Fresh, empty tables; or, for a resize (`lazy`), tables whose
    // stripes are set up as they migrate (HashTable::Uninitialized).
    DirSnapshot(uint32_t table_bits, uint32_t index_bits, uint64_t generation,
                bool lazy = false)
        : table_bits(table_bits), generation(generation) {
        tables.reserve(1u << table_bits);
        for (uint32_t i = 0; i < (1u << table_bits); ++i)
            tables.push_back(
                lazy ? std::make_shared<HashTable>(
                           index_bits, HashTable::Uninitialized{})
                     : std::make_shared<HashTable>(index_bits));
    }
    // Another snapshot's tables.
    DirSnapshot(std::vector<std::shared_ptr<HashTable>> tables,
                uint32_t table_bits, uint64_t generation)
        : tables(std::move(tables)),
          table_bits(table_bits),
          generation(generation) {}

    uint32_t size() const noexcept {
        return static_cast<uint32_t>(tables.size());
    }

    uint32_t table_index(uint64_t hash) const noexcept {
        return table_bits == 0 ? 0 : hash_to_tag(hash) >> (8 - table_bits);
    }

    HashTable& table_for_hash(uint64_t hash) noexcept {
        return *tables[table_index(hash)];
    }

    const HashTable& table_for_hash(uint64_t hash) const noexcept {
        return *tables[table_index(hash)];
    }
};

// RCU-protected directory of hash tables, resized incrementally (paper
// §4.3).
//
// Readers load the snapshot pointer inside an RCU read-side section and
// read without any lock. While a resize is in progress, each neighborhood
// slot is read from the new table if its stripe has migrated, else from the
// old one.
//
// Writers go through lock_for(), which returns the key's table in the
// current snapshot with the stripes covering the key's writes held. While a
// resize is in progress those are the old table's stripes, and lock_for()
// first migrates any of them not yet migrated; the last migration publishes
// the final snapshot. A full table asks the space waker for resize(): start
// one, or, if one is in progress, finish it on demand and start the next.
// Nothing stalls writers while a resize runs.
class Directory {
public:
    // initial_tables is rounded up to a power of two.
    Directory(Rcu& rcu, uint32_t initial_tables, uint32_t index_bits,
              ResizeMode mode = ResizeMode::kIncremental);
    ~Directory();

    Directory(const Directory&) = delete;
    Directory& operator=(const Directory&) = delete;

    // Lock-free lookup. Caller must hold an RCU read lock.
    HashEntry lookup(uint64_t hash, uint32_t start_offset = 0,
                     bool include_deleted = false) const noexcept;

    // Lock-free: the entry (live or deleted) for (hash, pba), or empty.
    // Caller must hold an RCU read lock.
    HashEntry entry_at(uint64_t hash, uint64_t pba) const noexcept;

    // The key's table in the current snapshot, with the stripes covering
    // the key's writes held: the old table's while a resize is in progress
    // (old_lock, after migrating them), and also the old table's during a
    // handover. Caller must hold an RCU read lock. Only in
    // ResizeMode::kFreeze can it come back frozen() (no table, no lock):
    // the caller leaves its read section, wait_for_grow(generation)s and
    // retries.
    struct Locked {
        HashTable* table;
        const DirSnapshot* snapshot;
        HashTable::WriteLock old_lock;
        HashTable::WriteLock lock;

        bool frozen() const noexcept { return table == nullptr; }
    };
    Locked lock_for(uint64_t hash);

    // ResizeMode::kFreeze: block until the snapshot of `generation` is no
    // longer current. Never inside a read-side section.
    void wait_for_grow(uint64_t generation);

    // Single-step writes. Caller must hold an RCU read lock. insert()
    // returns -ENOSPC if the table is full; the caller resizes, outside its
    // section.
    int insert(uint64_t hash, uint16_t kv_size, uint64_t pba);
    bool update(uint64_t hash, uint64_t old_pba,
                uint16_t new_kv_size, uint64_t new_pba);
    bool remove(uint64_t hash, uint64_t pba);

    static constexpr uint64_t kAnyGeneration = UINT64_MAX;

    // A table of snapshot `seen` is full. Unless that snapshot is no longer
    // current: finish its resize if one is in progress, migrating every
    // stripe still pending, and start the next, doubling the tables.
    // Returns 0, or -ENOSPC at the maximum size. Allocates the new tables
    // and may wait for a grace period (ending a handover), so it runs on
    // the space waker, never inside a read section or on an I/O poller.
    // In ResizeMode::kFreeze: the whole copy, as described there.
    int resize(uint64_t seen = kAnyGeneration);

    // resize(), then finish it: the tables doubled when it returns.
    // Recovery and tests.
    int grow(uint64_t seen = kAnyGeneration);

    // Finish a resize in progress and its handover. For close, before the
    // index is persisted: no operation may be in flight.
    void complete();

    bool resizing() const noexcept;
    ResizeMode mode() const noexcept { return mode_; }
    uint32_t num_tables() const noexcept;
    uint32_t index_bits() const noexcept;

    // The current snapshot, for callers no resize or write can race:
    // persisting the index at close() (after complete()) and restoring it
    // at open().
    DirSnapshot& snapshot() noexcept {
        DirSnapshot* snap = current_.load(std::memory_order_acquire);
        assert(!snap->resize);
        return *snap;
    }

private:
    // Copy stripe s of old table `old_table` into the new tables, with its
    // lock held. The last one publishes the final snapshot.
    void migrate_stripe(DirSnapshot& snap, uint32_t old_table,
                        uint64_t stripe);
    void publish_final(DirSnapshot* resizing);
    // With grow_mutex_ held and inside a read section: migrate every
    // stripe still pending in `snap`.
    void finish_migration(DirSnapshot* snap);
    // With grow_mutex_ held, outside any read section: end the current
    // snapshot's handover, waiting a grace period if it is still active.
    void end_handover();
    int grow_frozen(uint64_t seen);

    Rcu& rcu_;
    uint32_t index_bits_;
    ResizeMode mode_;
    alignas(64) std::atomic<DirSnapshot*> current_;
    // One resize() or complete() at a time; writers never take it.
    alignas(64) std::mutex grow_mutex_;
    std::mutex grown_mu_;               // kFreeze
    std::condition_variable grown_cv_;  // a frozen grow published
};

}  // namespace udepot

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

// Where a directory's tables come from and go. The store backs each table
// with an index segment of its own for the table's life, as uDepot's
// uDepotDirectoryMap::grow() allocates one per table; tests use memory.
class TableSource {
public:
    virtual ~TableSource() = default;
    // A new, cleared table, or nullptr if there is no space for one.
    virtual std::unique_ptr<HashTable> new_table() = 0;
    // `table` has left the directory (or never entered it): no writer
    // reaches it, though readers may until a grace period ends. Release
    // what backs it on the device; its memory goes with the table.
    virtual void retire_table(HashTable& table) = 0;
};

// A snapshot of the directory: 2^table_bits hash tables. Immutable once
// published; grow() creates a new one.
//
// As in uDepot's uDepotDirectoryMap::hash_to_map, a key's table is chosen by
// the top bits of its tag, which every entry stores; the bucket comes from
// the low bits. Those must be different bits, or each table would only ever
// use a fraction of its buckets and growing would add no capacity.
struct DirSnapshot {
    static constexpr uint32_t kMaxTableBits = 8;  // the tag's width

    std::vector<std::unique_ptr<HashTable>> tables;
    uint32_t table_bits;
    // Counts grows. Identifies a snapshot after it is freed, when its
    // address may already belong to a newer one.
    uint64_t generation;
    // Set by grow() before it copies this snapshot. Writers check it under
    // their stripe lock and back off; readers ignore it.
    std::atomic<bool> frozen{false};

    // `tables` holds a power of two of them, at most 2^kMaxTableBits.
    DirSnapshot(std::vector<std::unique_ptr<HashTable>> tables,
                uint64_t generation);

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

// RCU-protected directory of hash tables.
//
// Readers load the snapshot pointer inside an RCU read-side section and
// access tables without any lock, including while a grow copies them.
//
// Writers go through lock_for(), which returns the key's table in the
// current snapshot with its stripes locked, or reports the snapshot frozen.
// grow() takes the role of uDepot's rwpflock write side, for writers only:
// it freezes the snapshot, waits a grace period (every writer that missed
// the flag has finished and its write is in the old tables), copies with no
// locks held, publishes the new snapshot and hands the old one to
// Rcu::call() to be freed once no reader can still reach it. A writer that
// finds the snapshot frozen leaves its read section, waits for the grow
// (wait_for_grow) and retries on the new snapshot.
class Directory {
public:
    // Starts with `tables` (a power of two of them; uDepot starts with
    // one), and takes grown ones from `source`.
    Directory(Rcu& rcu, TableSource& source,
              std::vector<std::unique_ptr<HashTable>> tables);
    ~Directory();

    Directory(const Directory&) = delete;
    Directory& operator=(const Directory&) = delete;

    // Lock-free lookup. Caller must hold an RCU read lock.
    HashEntry lookup(uint64_t hash, uint32_t start_offset = 0,
                     bool include_deleted = false) const noexcept;

    // Lock-free: the entry (live or deleted) for (hash, pba), or empty.
    // Caller must hold an RCU read lock.
    HashEntry entry_at(uint64_t hash, uint64_t pba) const noexcept {
        return current_.load(std::memory_order_acquire)
            ->table_for_hash(hash).entry_at(hash, pba);
    }

    // The key's table in the current snapshot, with the stripes covering
    // the key's writes held; or, if a grow has frozen the snapshot, no
    // table and no lock (frozen()), and the caller must leave its read
    // section and wait_for_grow(snapshot->generation) before retrying. Caller must hold
    // an RCU read lock.
    struct Locked {
        HashTable* table;
        const DirSnapshot* snapshot;
        HashTable::WriteLock lock;

        bool frozen() const noexcept { return table == nullptr; }
    };
    Locked lock_for(uint64_t hash);

    // Block until the snapshot of `generation` is no longer current. Must
    // not be called inside a read-side section (the grow waits for those).
    void wait_for_grow(uint64_t generation);

    // Single-step writes for callers that never race grow() (tests,
    // recovery). Caller must hold an RCU read lock. insert() returns
    // -ENOSPC if the table is full; the caller grows, outside its section.
    int insert(uint64_t hash, uint16_t kv_size, uint64_t pba);
    bool update(uint64_t hash, uint64_t old_pba,
                uint16_t new_kv_size, uint64_t new_pba);
    bool remove(uint64_t hash, uint64_t pba);

    static constexpr uint64_t kAnyGeneration = UINT64_MAX;

    // Double the number of tables, unless the current snapshot is no longer
    // the one of generation `seen` (another grow did it). Returns 0, or
    // -ENOSPC at the maximum size or if the source has no space for the
    // new tables. Waits for a grace period, so it must not be called inside
    // a read-side section, nor on a thread whose progress in-flight
    // operations depend on (an I/O poller).
    int grow(uint64_t seen = kAnyGeneration);

    uint32_t num_tables() const noexcept;

    // The current snapshot, for callers no grow or write can race:
    // persisting the index at close() and restoring it at open().
    DirSnapshot& snapshot() noexcept {
        return *current_.load(std::memory_order_acquire);
    }

private:
    Rcu& rcu_;
    TableSource& source_;
    alignas(64) std::atomic<DirSnapshot*> current_;
    alignas(64) std::mutex grow_mutex_;  // one grow at a time
    std::mutex grown_mu_;
    std::condition_variable grown_cv_;   // a grow published a snapshot
};

}  // namespace udepot

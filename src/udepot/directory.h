// Copyright (c) 2024-2026 Nikolas Ioannou
// SPDX-License-Identifier: BSD-3-Clause

#pragma once

#include <atomic>
#include <cassert>
#include <cstdint>
#include <memory>
#include <mutex>
#include <vector>

#include "udepot/hash_table.h"
#include "udepot/rcu.h"

namespace udepot {

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

    DirSnapshot(uint32_t table_bits, uint32_t index_bits)
        : table_bits(table_bits) {
        tables.reserve(1u << table_bits);
        for (uint32_t i = 0; i < (1u << table_bits); ++i)
            tables.push_back(std::make_unique<HashTable>(index_bits));
    }

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
// access tables without any lock. Writers go through lock_for(), which
// returns the key's table in the current snapshot with its stripes locked.
// grow() locks every stripe of the old snapshot, copies it, publishes the
// new one and marks the old tables retired before unlocking, so a writer
// that was waiting on an old stripe sees it retired and retries on the new
// snapshot: no write is lost to a grow.
class Directory {
public:
    // initial_tables is rounded up to a power of two.
    Directory(Rcu& rcu, uint32_t initial_tables, uint32_t index_bits);
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
    // the key's writes held. Caller must hold an RCU read lock.
    struct Locked {
        HashTable* table;
        const DirSnapshot* snapshot;
        HashTable::WriteLock lock;
    };
    Locked lock_for(uint64_t hash);

    // Single-step writes, each under lock_for(). Caller must hold an RCU
    // read lock. insert() returns -ENOSPC if the table is full.
    int insert(uint64_t hash, uint16_t kv_size, uint64_t pba);
    bool update(uint64_t hash, uint64_t old_pba,
                uint16_t new_kv_size, uint64_t new_pba);
    bool remove(uint64_t hash, uint64_t pba);

    // Double the number of tables, unless the directory already changed
    // since `seen` (another writer grew it). Returns 0, or -ENOSPC at the
    // maximum size. Never waits for readers; retired snapshots are freed
    // with the directory.
    int grow(const DirSnapshot* seen = nullptr);

    uint32_t num_tables() const noexcept;
    uint32_t index_bits() const noexcept;

private:
    Rcu& rcu_;
    uint32_t index_bits_;
    alignas(64) std::atomic<DirSnapshot*> current_;
    alignas(64) std::mutex grow_mutex_;
    std::vector<std::unique_ptr<DirSnapshot>> retired_;  // grow_mutex_
};

}  // namespace udepot

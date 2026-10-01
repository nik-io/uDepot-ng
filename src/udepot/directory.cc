// Copyright (c) 2024-2026 Nikolas Ioannou
// SPDX-License-Identifier: BSD-3-Clause

#include "udepot/directory.h"

#include <bit>
#include <cerrno>
#include <thread>

namespace udepot {

namespace {

uint32_t table_bits_for(uint32_t tables) {
    uint32_t bits = 0;
    while ((1u << bits) < tables && bits < DirSnapshot::kMaxTableBits) ++bits;
    return bits;
}

}  // namespace

Directory::Directory(Rcu& rcu, uint32_t initial_tables, uint32_t index_bits)
    : rcu_(rcu),
      index_bits_(index_bits),
      current_(new DirSnapshot(table_bits_for(initial_tables), index_bits)) {}

Directory::~Directory() { delete current_.load(std::memory_order_relaxed); }

HashEntry Directory::lookup(uint64_t hash, uint32_t start_offset,
                            bool include_deleted) const noexcept {
    DirSnapshot* snap = current_.load(std::memory_order_acquire);
    return snap->table_for_hash(hash).lookup(hash, start_offset,
                                             include_deleted);
}

Directory::Locked Directory::lock_for(uint64_t hash) {
    for (;;) {
        DirSnapshot* snap = current_.load(std::memory_order_acquire);
        HashTable& table = snap->table_for_hash(hash);
        auto lock = table.lock_for(hash);
        // grow() marks a table retired, after publishing its replacement,
        // while holding all of its stripes; seeing it unretired here means
        // no grow has copied it yet, and none can until we unlock.
        if (!table.retired()) return Locked{&table, snap, std::move(lock)};
    }
}

int Directory::insert(uint64_t hash, uint16_t kv_size, uint64_t pba) {
    for (;;) {
        auto locked = lock_for(hash);
        if (locked.table->insert_locked(hash, kv_size, pba) == 0) return 0;
        const DirSnapshot* seen = locked.snapshot;
        locked.lock = HashTable::WriteLock{};
        if (grow(seen) != 0) return -ENOSPC;
    }
}

bool Directory::update(uint64_t hash, uint64_t old_pba,
                       uint16_t new_kv_size, uint64_t new_pba) {
    auto locked = lock_for(hash);
    return locked.table->update_locked(hash, old_pba, new_kv_size, new_pba);
}

bool Directory::remove(uint64_t hash, uint64_t pba) {
    auto locked = lock_for(hash);
    return locked.table->remove_locked(hash, pba);
}

int Directory::grow(const DirSnapshot* seen) {
    std::lock_guard<std::mutex> lock(grow_mutex_);

    DirSnapshot* old_snap = current_.load(std::memory_order_acquire);
    if (seen && seen != old_snap) return 0;  // someone else grew it
    if (old_snap->table_bits >= DirSnapshot::kMaxTableBits) return -ENOSPC;

    auto* new_snap = new DirSnapshot(old_snap->table_bits + 1, index_bits_);

    // Hold every stripe of the old snapshot until it is replaced and
    // retired: writes wait, then retry on the new snapshot.
    std::vector<std::vector<std::unique_lock<std::mutex>>> held;
    held.reserve(old_snap->size());
    for (auto& t : old_snap->tables) held.push_back(t->lock_all());

    for (auto& old_table : old_snap->tables) {
        for (uint64_t s = 0; s < old_table->total_slots(); ++s) {
            HashEntry entry = old_table->load_slot(s);
            if (entry.empty()) continue;
            // An entry holds its tag and home bucket: enough for both the
            // new table index (tag bits) and the bucket.
            uint64_t home_bucket = s - entry.bucket_offset();
            uint64_t hash = (static_cast<uint64_t>(entry.key_tag()) << 56) |
                            home_bucket;
            // Splitting a table halves its load, so the copy always fits.
            int rc = new_snap->table_for_hash(hash).insert_locked(
                hash, entry.kv_size(), entry.pba());
            assert(rc == 0);
            (void)rc;
        }
    }

    current_.store(new_snap, std::memory_order_release);
    for (auto& t : old_snap->tables) t->retire();
    held.clear();

    // Readers may still be in the old snapshot. This can run inside a
    // caller's own read-side section (put grows on a full table), so it
    // cannot wait for a grace period; the old snapshot lives as long as the
    // directory. Their sizes halve going back, so together they never
    // exceed the current one.
    retired_.emplace_back(old_snap);
    return 0;
}

uint32_t Directory::num_tables() const noexcept {
    return current_.load(std::memory_order_acquire)->size();
}

uint32_t Directory::index_bits() const noexcept { return index_bits_; }

}  // namespace udepot

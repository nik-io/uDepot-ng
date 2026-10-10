// Copyright (c) 2024-2026 Nikolas Ioannou
// SPDX-License-Identifier: BSD-3-Clause

#include "udepot/directory.h"

#include <bit>
#include <cerrno>
#include <thread>

namespace udepot {

DirSnapshot::DirSnapshot(std::vector<std::unique_ptr<HashTable>> tables,
                         uint64_t generation)
    : tables(std::move(tables)),
      table_bits(static_cast<uint32_t>(std::countr_zero(this->tables.size()))),
      generation(generation) {
    assert(std::has_single_bit(this->tables.size()) &&
           table_bits <= kMaxTableBits);
}

Directory::Directory(Rcu& rcu, TableSource& source,
                     std::vector<std::unique_ptr<HashTable>> tables)
    : rcu_(rcu),
      source_(source),
      current_(new DirSnapshot(std::move(tables), 0)) {}

Directory::~Directory() {
    // Snapshots retired by grow() are freed by RCU callbacks.
    rcu_.barrier();
    delete current_.load(std::memory_order_relaxed);
}

HashEntry Directory::lookup(uint64_t hash, uint32_t start_offset,
                            bool include_deleted) const noexcept {
    DirSnapshot* snap = current_.load(std::memory_order_acquire);
    return snap->table_for_hash(hash).lookup(hash, start_offset,
                                             include_deleted);
}

Directory::Locked Directory::lock_for(uint64_t hash) {
    DirSnapshot* snap = current_.load(std::memory_order_acquire);
    HashTable& table = snap->table_for_hash(hash);
    auto lock = table.lock_for(hash);
    // Checked inside the caller's read section: a writer that sees false
    // here is one grow()'s synchronize() waits for, so its write is in the
    // tables before they are copied.
    if (snap->frozen.load(std::memory_order_acquire))
        return Locked{nullptr, snap, {}};
    return Locked{&table, snap, std::move(lock)};
}

void Directory::wait_for_grow(uint64_t generation) {
    std::unique_lock<std::mutex> lock(grown_mu_);
    grown_cv_.wait(lock, [&] {
        // A snapshot is freed once replaced; read it inside a section.
        Rcu::ReadGuard guard(rcu_);
        return current_.load(std::memory_order_acquire)->generation !=
               generation;
    });
}

int Directory::insert(uint64_t hash, uint16_t kv_size, uint64_t pba) {
    auto locked = lock_for(hash);
    assert(!locked.frozen());
    return locked.table->insert_locked(hash, kv_size, pba) == 0 ? 0 : -ENOSPC;
}

bool Directory::update(uint64_t hash, uint64_t old_pba,
                       uint16_t new_kv_size, uint64_t new_pba) {
    auto locked = lock_for(hash);
    assert(!locked.frozen());
    return locked.table->update_locked(hash, old_pba, new_kv_size, new_pba);
}

bool Directory::remove(uint64_t hash, uint64_t pba) {
    auto locked = lock_for(hash);
    assert(!locked.frozen());
    return locked.table->remove_locked(hash, pba);
}

int Directory::grow(uint64_t seen) {
    std::lock_guard<std::mutex> lock(grow_mutex_);

    // Only grow() replaces current_, and it holds grow_mutex_: the snapshot
    // cannot be freed under us.
    DirSnapshot* old_snap = current_.load(std::memory_order_acquire);
    if (seen != kAnyGeneration && seen != old_snap->generation)
        return 0;  // someone else grew it
    if (old_snap->table_bits >= DirSnapshot::kMaxTableBits) return -ENOSPC;

    // Allocated before freezing: writers stall only for the copy. As
    // uDepot's grow(), each new table gets a segment of its own.
    std::vector<std::unique_ptr<HashTable>> tables;
    tables.reserve(2 * old_snap->size());
    for (uint32_t i = 0; i < 2 * old_snap->size(); ++i) {
        auto table = source_.new_table();
        if (!table) {
            for (auto& t : tables) source_.retire_table(*t);
            grow_failures_.fetch_add(1, std::memory_order_acq_rel);
            return -ENOSPC;
        }
        tables.push_back(std::move(table));
    }
    auto* new_snap = new DirSnapshot(std::move(tables),
                                     old_snap->generation + 1);

    // As uDepot's rwpflock write_enter + write_wait_readers, for writers
    // only: new writers see the flag and back off, and the grace period
    // waits out every one that did not. Readers carry on.
    old_snap->frozen.store(true, std::memory_order_seq_cst);
    rcu_.synchronize();

    // Nothing writes the old tables now; the new ones are unpublished.
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
    {
        std::lock_guard<std::mutex> g(grown_mu_);
    }
    grown_cv_.notify_all();

    // The old tables' segments are free once no writer can reach them, as
    // uDepot's grow() invalidates them; readers still use their memory.
    for (auto& t : old_snap->tables) source_.retire_table(*t);
    // Readers may still be in the old snapshot: free it after a grace
    // period, off this path (call_rcu).
    rcu_.call([old_snap] { delete old_snap; });
    return 0;
}

uint32_t Directory::num_tables() const noexcept {
    Rcu::ReadGuard guard(rcu_);
    return current_.load(std::memory_order_acquire)->size();
}

}  // namespace udepot

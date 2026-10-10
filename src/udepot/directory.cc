// Copyright (c) 2024-2026 Nikolas Ioannou
// SPDX-License-Identifier: BSD-3-Clause

#include "udepot/directory.h"

#include <cerrno>

namespace udepot {

namespace {

uint32_t table_bits_for(uint32_t tables) {
    uint32_t bits = 0;
    while ((1u << bits) < tables && bits < DirSnapshot::kMaxTableBits) ++bits;
    return bits;
}

// Reads a neighborhood slot from the new table if its stripe has migrated,
// else from the old one. The stripe's flag is read (acquire) when the scan
// enters the stripe, before its slots: a migration happens before any write
// to the stripe's new slots, so a slot that a write emptied while moving an
// entry up into the next stripe is followed by that stripe reading as
// migrated, and the entry is found there (entries only move up, written
// before cleared, as in a single table).
class ResizingSlots {
public:
    ResizingSlots(const DirSnapshot& snap, uint32_t new_table)
        : resize_(*snap.resize),
          new_(*snap.tables[new_table]),
          old_(*resize_.old[new_table >> 1]),
          old_table_(new_table >> 1) {}

    HashEntry operator()(uint64_t idx) {
        const uint64_t stripe = new_.stripe_of_slot(idx);
        if (stripe != stripe_) {
            stripe_ = stripe;
            src_ = resize_.is_migrated(old_table_, stripe) ? &new_ : &old_;
        }
        return src_->load_slot(idx, std::memory_order_acquire);
    }

private:
    const Resize& resize_;
    const HashTable& new_;
    const HashTable& old_;
    const uint32_t old_table_;
    uint64_t stripe_ = UINT64_MAX;
    const HashTable* src_ = nullptr;
};

}  // namespace

Directory::Directory(Rcu& rcu, uint32_t initial_tables, uint32_t index_bits,
                     ResizeMode mode)
    : rcu_(rcu),
      index_bits_(index_bits),
      mode_(mode),
      current_(new DirSnapshot(table_bits_for(initial_tables), index_bits,
                               0)) {}

Directory::~Directory() {
    complete();
    // Replaced snapshots and tables are freed by RCU callbacks, the old
    // tables of a resize by one that the handover's queues: twice.
    rcu_.barrier();
    rcu_.barrier();
    delete current_.load(std::memory_order_relaxed);
}

HashEntry Directory::lookup(uint64_t hash, uint32_t start_offset,
                            bool include_deleted) const noexcept {
    const DirSnapshot* snap = current_.load(std::memory_order_acquire);
    const uint32_t t = snap->table_index(hash);
    const HashTable& table = *snap->tables[t];
    if (!snap->resize) return table.lookup(hash, start_offset, include_deleted);
    return HashTable::scan(table.bucket_of(hash), hash_to_tag(hash),
                           start_offset, include_deleted,
                           ResizingSlots(*snap, t));
}

HashEntry Directory::entry_at(uint64_t hash, uint64_t pba) const noexcept {
    const DirSnapshot* snap = current_.load(std::memory_order_acquire);
    const uint32_t t = snap->table_index(hash);
    const HashTable& table = *snap->tables[t];
    if (!snap->resize) return table.entry_at(hash, pba);
    return HashTable::scan_pba(table.bucket_of(hash), hash_to_tag(hash), pba,
                               ResizingSlots(*snap, t));
}

Directory::Locked Directory::lock_for(uint64_t hash) {
    for (;;) {
        DirSnapshot* snap = current_.load(std::memory_order_acquire);
        const uint32_t t = snap->table_index(hash);
        HashTable& table = *snap->tables[t];
        const auto [first, last] = table.stripes_for(hash);

        if (snap->resize) {
            // The old table's stripes guard both its slots and the new
            // tables' (paper §4.3): migrate them, then write the new table.
            Resize& rs = *snap->resize;
            const uint32_t old_t = t >> 1;
            Locked locked{&table, snap,
                          rs.old[old_t]->lock_stripes(first, last), {}};
            for (uint64_t s = first; s <= last; ++s)
                if (!rs.migrated[old_t * rs.stripes + s].load(
                        std::memory_order_relaxed))
                    migrate_stripe(*snap, old_t, s);
            return locked;
        }

        HashTable::WriteLock old_lock;
        if (snap->handover &&
            snap->handover->active.load(std::memory_order_acquire))
            old_lock = snap->handover->old[t >> 1]->lock_stripes(first, last);
        HashTable::WriteLock lock = table.lock_stripes(first, last);
        // kFreeze: a writer that sees false here is one the grow's grace
        // period waits for, so its write is in the tables before the copy.
        if (snap->frozen.load(std::memory_order_acquire))
            return Locked{nullptr, snap, {}, {}};
        // A resize started on this snapshot: its migrations take these
        // stripes, so a writer that sees false here writes before the
        // stripe is copied, and one that sees true retries on the
        // resizing snapshot, already published.
        if (snap->superseded.load(std::memory_order_acquire)) continue;
        return Locked{&table, snap, std::move(old_lock), std::move(lock)};
    }
}

void Directory::migrate_stripe(DirSnapshot& snap, uint32_t old_table,
                               uint64_t stripe) {
    Resize& rs = *snap.resize;
    const HashTable& old = *rs.old[old_table];
    // An entry's new table is its old one plus the next tag bit; its slot
    // does not change. Each old slot goes to exactly one new table, and no
    // write reaches a stripe's new slots before it migrates: the copy
    // lands in empty slots and needs no displacement.
    for (uint64_t idx = old.stripe_begin(stripe); idx < old.stripe_end(stripe);
         ++idx) {
        const HashEntry e = old.load_slot(idx);
        if (e.empty()) continue;
        HashTable& dst = *snap.tables[e.key_tag() >> (8 - snap.table_bits)];
        assert(dst.load_slot(idx).empty());
        dst.restore_slot(idx, e.raw());
    }
    rs.migrated[old_table * rs.stripes + stripe].store(
        true, std::memory_order_release);
    if (rs.remaining.fetch_sub(1, std::memory_order_acq_rel) == 1)
        publish_final(&snap);
}

void Directory::publish_final(DirSnapshot* resizing) {
    auto* final_snap = new DirSnapshot(resizing->tables, resizing->table_bits,
                                       resizing->generation + 1);
    auto handover = std::make_shared<Handover>();
    std::vector<std::shared_ptr<HashTable>> old = resizing->resize->old;
    for (auto& t : old) handover->old.push_back(t.get());
    final_snap->handover = handover;

    DirSnapshot* expected = resizing;
    const bool swapped = current_.compare_exchange_strong(
        expected, final_snap, std::memory_order_acq_rel);
    assert(swapped);  // only the last migration publishes
    (void)swapped;

    // After a grace period no writer is left on the resizing snapshot: end
    // the handover. Writers that saw it active may still hold the old
    // stripes, so free the old tables a grace period later.
    Rcu* rcu = &rcu_;
    rcu_.call([rcu, resizing, handover, old = std::move(old)]() mutable {
        handover->active.store(false, std::memory_order_release);
        delete resizing;
        rcu->call([old = std::move(old)] {});
    });
}

void Directory::finish_migration(DirSnapshot* snap) {
    Resize& rs = *snap->resize;
    for (uint32_t t = 0; t < rs.old.size(); ++t) {
        for (uint64_t s = 0; s < rs.stripes; ++s) {
            auto lock = rs.old[t]->lock_stripes(s, s);
            if (!rs.migrated[t * rs.stripes + s].load(
                    std::memory_order_relaxed))
                migrate_stripe(*snap, t, s);
        }
    }
}

void Directory::end_handover() {
    // Stable and current: only grow_mutex_'s holder replaces it.
    DirSnapshot* snap = current_.load(std::memory_order_acquire);
    assert(!snap->resize);
    if (!snap->handover ||
        !snap->handover->active.load(std::memory_order_acquire))
        return;
    rcu_.synchronize();  // every writer on the resizing snapshot is done
    snap->handover->active.store(false, std::memory_order_release);
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

int Directory::grow_frozen(uint64_t seen) {
    std::lock_guard<std::mutex> lock(grow_mutex_);
    // Only this replaces current_ in kFreeze, under grow_mutex_: the
    // snapshot cannot be freed under us.
    DirSnapshot* old_snap = current_.load(std::memory_order_acquire);
    if (seen != kAnyGeneration && seen != old_snap->generation) return 0;
    if (old_snap->table_bits >= DirSnapshot::kMaxTableBits) return -ENOSPC;

    // Allocated before freezing: writers stall only for the copy.
    auto* new_snap = new DirSnapshot(old_snap->table_bits + 1, index_bits_,
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
    rcu_.call([old_snap] { delete old_snap; });
    return 0;
}

int Directory::resize(uint64_t seen) {
    if (mode_ == ResizeMode::kFreeze) return grow_frozen(seen);
    std::lock_guard<std::mutex> lock(grow_mutex_);
    {
        // A resizing snapshot is freed once its final one is published,
        // which a writer can do at any time: read it inside a section.
        Rcu::ReadGuard guard(rcu_);
        DirSnapshot* snap = current_.load(std::memory_order_acquire);
        if (seen != kAnyGeneration && seen != snap->generation)
            return 0;  // it moved on; the caller retries
        // A table filled up mid-resize: finish it on demand, so writers to
        // regions nothing else would migrate are not left waiting.
        if (snap->resize) finish_migration(snap);
    }

    end_handover();
    DirSnapshot* old_snap = current_.load(std::memory_order_acquire);
    if (old_snap->table_bits >= DirSnapshot::kMaxTableBits) return -ENOSPC;

    // Allocated here, on the space waker, never on a writer (paper: "Hash
    // tables are pre-allocated during the resize operation in a separate
    // thread to avoid delays").
    auto* snap = new DirSnapshot(old_snap->table_bits + 1, index_bits_,
                                 old_snap->generation + 1);
    auto rs = std::make_unique<Resize>();
    rs->old = old_snap->tables;
    rs->stripes = rs->old[0]->num_stripes();
    assert(rs->stripes == snap->tables[0]->num_stripes());
    const uint64_t n = rs->old.size() * rs->stripes;
    rs->migrated = std::make_unique<std::atomic<bool>[]>(n);
    for (uint64_t i = 0; i < n; ++i)
        rs->migrated[i].store(false, std::memory_order_relaxed);
    rs->remaining.store(n, std::memory_order_relaxed);
    snap->resize = std::move(rs);

    old_snap->superseded.store(true, std::memory_order_seq_cst);
    current_.store(snap, std::memory_order_release);
    // The old snapshot's tables live on in the resize.
    rcu_.call([old_snap] { delete old_snap; });
    return 0;
}

int Directory::grow(uint64_t seen) {
    const int rc = resize(seen);
    if (rc != 0) return rc;
    complete();
    return 0;
}

void Directory::complete() {
    std::lock_guard<std::mutex> lock(grow_mutex_);
    {
        Rcu::ReadGuard guard(rcu_);
        DirSnapshot* snap = current_.load(std::memory_order_acquire);
        if (snap->resize) finish_migration(snap);
    }
    end_handover();
}

int Directory::insert(uint64_t hash, uint16_t kv_size, uint64_t pba) {
    auto locked = lock_for(hash);
    return locked.table->insert_locked(hash, kv_size, pba) == 0 ? 0 : -ENOSPC;
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

bool Directory::resizing() const noexcept {
    Rcu::ReadGuard guard(rcu_);
    return current_.load(std::memory_order_acquire)->resize != nullptr;
}

uint32_t Directory::num_tables() const noexcept {
    Rcu::ReadGuard guard(rcu_);
    return current_.load(std::memory_order_acquire)->size();
}

uint32_t Directory::index_bits() const noexcept { return index_bits_; }

}  // namespace udepot

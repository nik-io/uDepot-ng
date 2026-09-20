#include "udepot/directory.h"

#include <cassert>

namespace udepot {

Directory::Directory(Rcu& rcu, uint32_t initial_tables,
                     uint32_t index_bits)
    : rcu_(rcu),
      index_bits_(index_bits),
      current_(new DirSnapshot(initial_tables, index_bits)) {}

Directory::~Directory() { delete current_.load(std::memory_order_relaxed); }

HashEntry Directory::lookup(uint64_t hash) const noexcept {
    DirSnapshot* snap = current_.load(std::memory_order_acquire);
    return snap->table_for_hash(hash).lookup(hash);
}

int Directory::insert(uint64_t hash, uint16_t kv_size, uint64_t pba) {
    DirSnapshot* snap = current_.load(std::memory_order_acquire);
    return snap->table_for_hash(hash).insert(hash, kv_size, pba);
}

bool Directory::remove(uint64_t hash, uint64_t pba) {
    DirSnapshot* snap = current_.load(std::memory_order_acquire);
    return snap->table_for_hash(hash).remove(hash, pba);
}

int Directory::grow() {
    std::lock_guard<std::mutex> lock(grow_mutex_);

    DirSnapshot* old_snap = current_.load(std::memory_order_acquire);
    uint32_t old_count = old_snap->size();
    uint32_t new_count = old_count * 2;

    auto* new_snap = new DirSnapshot(new_count, index_bits_);

    // Rehash: for each entry in each old table, insert into the
    // appropriate new table. We walk the raw slots.
    for (uint32_t t = 0; t < old_count; ++t) {
        HashTable& old_table = *old_snap->tables[t];

        for (uint64_t s = 0; s < old_table.total_slots(); ++s) {
            HashEntry entry = old_table.load_slot(s);
            if (entry.empty()) continue;

            // Reconstruct the hash from the entry's position and tag.
            uint64_t home_bucket = s - entry.bucket_offset();
            uint64_t hash = (static_cast<uint64_t>(entry.key_tag()) << 56) |
                            home_bucket;

            // The old table index was: hash % old_count == t
            // The new table index is: hash % new_count
            // This is either t or t + old_count.
            int rc = new_snap->table_for_hash(hash).insert(
                hash, entry.kv_size(), entry.pba());
            if (rc != 0) {
                delete new_snap;
                return -1;
            }
        }
    }

    // Publish the new directory. Readers in progress still see old_snap
    // via their loaded pointer — RCU guarantees it remains valid.
    current_.store(new_snap, std::memory_order_release);

    // Wait for all pre-existing readers to finish, then reclaim.
    rcu_.synchronize();
    delete old_snap;

    return 0;
}

uint32_t Directory::num_tables() const noexcept {
    return current_.load(std::memory_order_acquire)->size();
}

uint32_t Directory::index_bits() const noexcept { return index_bits_; }

}  // namespace udepot

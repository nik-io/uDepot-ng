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

// A snapshot of the directory: a sized array of hash tables.
// Immutable once published — grow() creates a new one.
struct DirSnapshot {
    std::vector<std::unique_ptr<HashTable>> tables;

    explicit DirSnapshot(uint32_t num_tables, uint32_t index_bits) {
        tables.reserve(num_tables);
        for (uint32_t i = 0; i < num_tables; ++i) {
            tables.push_back(std::make_unique<HashTable>(index_bits));
        }
    }

    uint32_t size() const noexcept {
        return static_cast<uint32_t>(tables.size());
    }

    HashTable& table_for_hash(uint64_t hash) noexcept {
        return *tables[hash % tables.size()];
    }

    const HashTable& table_for_hash(uint64_t hash) const noexcept {
        return *tables[hash % tables.size()];
    }
};

// RCU-protected directory of hash tables.
//
// Readers load the directory pointer under rcu_read_lock and access
// tables without any lock. Writers (grow) allocate a new directory,
// copy entries, publish the pointer, then wait for a grace period
// before reclaiming the old one.
class Directory {
public:
    // Construct with initial_tables hash tables, each with
    // 2^index_bits buckets.
    Directory(Rcu& rcu, uint32_t initial_tables, uint32_t index_bits);
    ~Directory();

    Directory(const Directory&) = delete;
    Directory& operator=(const Directory&) = delete;

    // Lock-free lookup. Caller must hold an RCU read lock.
    // Pass start_offset > 0 to resume past a previous tag-matching entry.
    HashEntry lookup(uint64_t hash, uint32_t start_offset = 0) const noexcept;

    // Insert under stripe lock. Caller must hold an RCU read lock.
    int insert(uint64_t hash, uint16_t kv_size, uint64_t pba);

    // Remove under stripe lock. Caller must hold an RCU read lock.
    bool remove(uint64_t hash, uint64_t pba);

    // Double the directory by splitting each table into two.
    // Single-writer (serialized by grow_mutex_). Waits for an RCU
    // grace period before freeing the old directory.
    // Returns 0 on success.
    int grow();

    uint32_t num_tables() const noexcept;
    uint32_t index_bits() const noexcept;

private:
    Rcu& rcu_;
    uint32_t index_bits_;
    std::atomic<DirSnapshot*> current_;
    std::mutex grow_mutex_;
};

}  // namespace udepot

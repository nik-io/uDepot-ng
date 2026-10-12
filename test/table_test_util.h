// Copyright (c) 2024-2026 Nikolas Ioannou
// SPDX-License-Identifier: BSD-3-Clause

#pragma once

#include <cerrno>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <vector>

#include "udepot/directory.h"
#include "udepot/hash_table.h"
#include "udepot/table_region.h"

namespace udepot::test {

// Net bytes of a segment whose table holds 2^bits buckets: as in the
// store, a table's size follows from its segment's.
inline size_t table_net_bytes(uint32_t bits) {
    return 2 * TableRegion::kMdBytes +
           ((size_t{1} << bits) + HashEntry::kHopRange) * sizeof(uint64_t);
}

// A cleared table in a mapping of its own, as if of such a segment.
inline std::unique_ptr<HashTable> make_table(uint32_t bits) {
    const size_t net = table_net_bytes(bits);
    TableRegion region = TableRegion::map((net + 4095) / 4096 * 4096, net,
                                          TableRegion::kNoSegment);
    if (!region.valid()) return nullptr;
    auto table = std::make_unique<HashTable>(std::move(region));
    table->clear();
    return table;
}

// Tables of 2^bits buckets in memory, at most `budget` of them out at once
// (a store with that many free index segments: then -ENOSPC).
// again(n, after) lets `after` more requests through, then fails the n
// next with -EAGAIN (none free yet).
class MemTables final : public TableSource {
public:
    explicit MemTables(uint32_t bits, size_t budget = SIZE_MAX)
        : bits_(bits), budget_(budget) {}

    int new_table(std::unique_ptr<HashTable>* table) override {
        ++requests_;
        if (pass_ > 0) {
            --pass_;
        } else if (again_ > 0) {
            --again_;
            return -EAGAIN;
        }
        if (live_ >= budget_) return -ENOSPC;
        *table = make_table(bits_);
        if (!*table) return -ENOMEM;
        ++live_;
        ++made_;
        return 0;
    }
    void retire_table(HashTable&) override {
        --live_;
        ++retired_;
    }

    // n new tables, for a directory to start with.
    std::vector<std::unique_ptr<HashTable>> initial(uint32_t n) {
        std::vector<std::unique_ptr<HashTable>> tables(n);
        for (auto& t : tables) new_table(&t);
        return tables;
    }

    void again(int n, int after = 0) {
        again_ = n;
        pass_ = after;
    }
    size_t live() const { return live_; }
    size_t made() const { return made_; }
    size_t retired() const { return retired_; }
    size_t requests() const { return requests_; }

private:
    uint32_t bits_;
    size_t budget_;
    int again_ = 0;
    int pass_ = 0;
    size_t live_ = 0;
    size_t made_ = 0;
    size_t retired_ = 0;
    size_t requests_ = 0;
};

}  // namespace udepot::test

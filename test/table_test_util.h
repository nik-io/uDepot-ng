// Copyright (c) 2024-2026 Nikolas Ioannou
// SPDX-License-Identifier: BSD-3-Clause

#pragma once

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
// (a store with that many free index segments).
class MemTables final : public TableSource {
public:
    explicit MemTables(uint32_t bits, size_t budget = SIZE_MAX)
        : bits_(bits), budget_(budget) {}

    std::unique_ptr<HashTable> new_table() override {
        if (live_ >= budget_) return nullptr;
        ++live_;
        return make_table(bits_);
    }
    void retire_table(HashTable&) override {
        --live_;
        ++retired_;
    }

    // n new tables, for a directory to start with.
    std::vector<std::unique_ptr<HashTable>> initial(uint32_t n) {
        std::vector<std::unique_ptr<HashTable>> tables;
        for (uint32_t i = 0; i < n; ++i) tables.push_back(new_table());
        return tables;
    }

    size_t live() const { return live_; }
    size_t retired() const { return retired_; }

private:
    uint32_t bits_;
    size_t budget_;
    size_t live_ = 0;
    size_t retired_ = 0;
};

}  // namespace udepot::test

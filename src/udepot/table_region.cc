// Copyright (c) 2024-2026 Nikolas Ioannou
// SPDX-License-Identifier: BSD-3-Clause

#include "udepot/table_region.h"

#include <sys/mman.h>

#include <cassert>

namespace udepot {

TableRegion::~TableRegion() { unmap(); }

TableRegion& TableRegion::operator=(TableRegion&& other) noexcept {
    if (this != &other) {
        unmap();
        base_ = std::exchange(other.base_, nullptr);
        map_bytes_ = std::exchange(other.map_bytes_, 0);
        net_bytes_ = std::exchange(other.net_bytes_, 0);
        grain_ = std::exchange(other.grain_, 0);
        huge_ = std::exchange(other.huge_, false);
    }
    return *this;
}

bool TableRegion::wants_huge_pages(size_t seg_bytes) noexcept {
    if (seg_bytes < kHugePage) return false;
    const size_t map_bytes = huge_map_bytes(seg_bytes);
    return (map_bytes - seg_bytes) * kMaxHugeWaste <= map_bytes;
}

TableRegion TableRegion::map(size_t seg_bytes, size_t net_bytes,
                             uint64_t grain) {
    assert(net_bytes <= seg_bytes && net_bytes >= 2 * kMdBytes);
    constexpr int kProt = PROT_READ | PROT_WRITE;
    constexpr int kFlags = MAP_PRIVATE | MAP_ANONYMOUS;
    void* p = MAP_FAILED;
    size_t map_bytes = seg_bytes;
    bool huge = false;
    if (wants_huge_pages(seg_bytes)) {
        const size_t huge_bytes = huge_map_bytes(seg_bytes);
        p = ::mmap(nullptr, huge_bytes, kProt, kFlags | MAP_HUGETLB, -1, 0);
        huge = p != MAP_FAILED;
        if (huge) map_bytes = huge_bytes;
    }
    if (!huge) p = ::mmap(nullptr, seg_bytes, kProt, kFlags, -1, 0);
    if (p == MAP_FAILED) return {};
    return TableRegion(static_cast<uint8_t*>(p), map_bytes, net_bytes, grain,
                       huge);
}

void TableRegion::unmap() noexcept {
    if (!base_) return;
    int rc = ::munmap(base_, map_bytes_);
    assert(rc == 0);
    (void)rc;
    base_ = nullptr;
}

}  // namespace udepot

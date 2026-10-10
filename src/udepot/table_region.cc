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

TableRegion TableRegion::map(size_t seg_bytes, size_t net_bytes,
                             uint64_t grain) {
    assert(net_bytes <= seg_bytes && net_bytes >= 2 * kMdBytes);
    constexpr int kProt = PROT_READ | PROT_WRITE;
    constexpr int kFlags = MAP_PRIVATE | MAP_ANONYMOUS;
    void* p = MAP_FAILED;
    bool huge = false;
    if (seg_bytes % kHugePage == 0) {
        p = ::mmap(nullptr, seg_bytes, kProt, kFlags | MAP_HUGETLB, -1, 0);
        huge = p != MAP_FAILED;
    }
    if (!huge) p = ::mmap(nullptr, seg_bytes, kProt, kFlags, -1, 0);
    if (p == MAP_FAILED) return {};
    return TableRegion(static_cast<uint8_t*>(p), seg_bytes, net_bytes, grain,
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

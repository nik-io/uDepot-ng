// Copyright (c) 2024-2026 Nikolas Ioannou
// SPDX-License-Identifier: BSD-3-Clause

#pragma once

#include <cstddef>
#include <cstdint>
#include <utility>

namespace udepot {

// The memory a directory table lives in: a mapping of its whole index
// segment, as uDepot's uDepotDirectoryMap maps a directory segment. The
// table uses the net region, laid out as
//
//   [ header, 512 B | slots, 8 B each | ... | footer, 512 B ]
//
// with the footer at the very end of the net region; the segment's salsa
// metadata sits in the tail past it, inside the mapping but never written
// from it.
//
// As uDepot (0e0b0a0, "map the full segment on huge pages"): the mapping
// always spans the whole segment, on huge pages when the segment's size is
// a 2 MiB multiple (segments tile the device from offset 0, so such a
// segment is also 2 MiB-aligned there), falling back to 4 KiB pages when
// no huge page is available; any other segment size maps on 4 KiB pages.
// The mapping is anonymous, as uDepot's on its AIO and SPDK backends: the
// table is read from its segment on restore and written back at close.
class TableRegion {
public:
    static constexpr size_t kMdBytes = 512;  // uDepot's dirmap_hdr/_ftr
    static constexpr size_t kHugePage = size_t{2} << 20;
    // grain() of a table not (yet) given a segment.
    static constexpr uint64_t kNoSegment = ~uint64_t{0};

    TableRegion() noexcept = default;
    ~TableRegion();

    TableRegion(TableRegion&& other) noexcept { *this = std::move(other); }
    TableRegion& operator=(TableRegion&& other) noexcept;
    TableRegion(const TableRegion&) = delete;
    TableRegion& operator=(const TableRegion&) = delete;

    // Map seg_bytes for a table using the first net_bytes, backed by the
    // segment starting at `grain` (kNoSegment for none yet). The memory is
    // zero. Returns an invalid region if the mapping fails.
    static TableRegion map(size_t seg_bytes, size_t net_bytes,
                           uint64_t grain);

    bool valid() const noexcept { return base_ != nullptr; }
    uint8_t* base() const noexcept { return base_; }
    size_t net_bytes() const noexcept { return net_bytes_; }
    size_t map_bytes() const noexcept { return map_bytes_; }
    uint64_t grain() const noexcept { return grain_; }
    bool huge() const noexcept { return huge_; }

    // Give a region mapped with kNoSegment its segment.
    void set_grain(uint64_t grain) noexcept { grain_ = grain; }

    // Offset of the footer in the net region.
    size_t footer_offset() const noexcept { return net_bytes_ - kMdBytes; }

private:
    TableRegion(uint8_t* base, size_t map_bytes, size_t net_bytes,
                uint64_t grain, bool huge) noexcept
        : base_(base), map_bytes_(map_bytes), net_bytes_(net_bytes),
          grain_(grain), huge_(huge) {}

    void unmap() noexcept;

    uint8_t* base_ = nullptr;
    size_t map_bytes_ = 0;
    size_t net_bytes_ = 0;
    uint64_t grain_ = 0;
    bool huge_ = false;
};

}  // namespace udepot

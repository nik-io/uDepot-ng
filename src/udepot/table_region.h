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
// As uDepot (0e0b0a0, "map the full segment on huge pages"), the mapping
// always spans the whole segment. It is on huge pages, rounded up to a
// 2 MiB multiple, when wants_huge_pages(): the mapping is anonymous, as
// uDepot's on its AIO and SPDK backends (the table is read from its
// segment on restore and written back at close, never mapped from the
// device), so it need not match the segment's size or where it sits on the
// device. uDepot took huge pages only for a segment that is a 2 MiB
// multiple, and every such segment's table filled half of it (see
// fitted_segment_size). Otherwise, or when no huge page is available, 4 KiB
// pages map the segment exactly; a grow then faults each new table in 4 KiB
// at a time, and lookups miss the TLB more.
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

    // Whether a segment of seg_bytes maps on huge pages: it is 2 MiB or
    // more, and rounding it up to a 2 MiB multiple leaves at most
    // 1/kMaxHugeWaste of the mapping unused. A fitted segment is a power of
    // two plus a page or two, so its rounding wastes nearly 2 MiB: under
    // 1/8 from a 16 MiB table up (the default's is 512 MiB), half of the
    // bindings' 2 MiB one, which would take 4 MiB of huge pages each, from
    // the pool SPDK's DMA buffers come from.
    static constexpr size_t kMaxHugeWaste = 8;
    static bool wants_huge_pages(size_t seg_bytes) noexcept;
    // Bytes a huge-page mapping of seg_bytes takes: rounded up to 2 MiB.
    static size_t huge_map_bytes(size_t seg_bytes) noexcept {
        return (seg_bytes + kHugePage - 1) / kHugePage * kHugePage;
    }

    // Map seg_bytes (rounded up to 2 MiB on huge pages) for a table using
    // the first net_bytes, backed by the segment starting at `grain`
    // (kNoSegment for none yet). The memory is zero. Returns an invalid
    // region if the mapping fails.
    static TableRegion map(size_t seg_bytes, size_t net_bytes,
                           uint64_t grain);

    bool valid() const noexcept { return base_ != nullptr; }
    uint8_t* base() const noexcept { return base_; }
    size_t net_bytes() const noexcept { return net_bytes_; }
    size_t map_bytes() const noexcept { return map_bytes_; }
    uint64_t grain() const noexcept { return grain_; }
    bool huge() const noexcept { return huge_; }
    // The errno of a huge-page mapping that wants_huge_pages() asked for
    // and the system refused (no huge page free: ENOMEM), else 0.
    int huge_error() const noexcept { return huge_error_; }

    // Give a region mapped with kNoSegment its segment.
    void set_grain(uint64_t grain) noexcept { grain_ = grain; }

    // Offset of the footer in the net region.
    size_t footer_offset() const noexcept { return net_bytes_ - kMdBytes; }

private:
    TableRegion(uint8_t* base, size_t map_bytes, size_t net_bytes,
                uint64_t grain, bool huge, int huge_error) noexcept
        : base_(base), map_bytes_(map_bytes), net_bytes_(net_bytes),
          grain_(grain), huge_(huge), huge_error_(huge_error) {}

    void unmap() noexcept;

    uint8_t* base_ = nullptr;
    size_t map_bytes_ = 0;
    size_t net_bytes_ = 0;
    uint64_t grain_ = 0;
    bool huge_ = false;
    int huge_error_ = 0;
};

}  // namespace udepot

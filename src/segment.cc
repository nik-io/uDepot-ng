#include "udepot/segment.h"

#include <algorithm>
#include <cassert>

namespace udepot {

static uint64_t align_up(uint64_t val, uint64_t align) {
    return (val + align - 1) / align * align;
}

SegmentGeometry::SegmentGeometry(uint64_t device_bytes, uint64_t grain_bytes,
                                 uint64_t segment_grains)
    : device_bytes_(device_bytes),
      grain_bytes_(grain_bytes),
      segment_grains_(segment_grains) {
    assert(grain_bytes > 0);
    assert(segment_grains > 0);

    // Per-segment metadata size in grains (rounded up).
    md_grains_ = align_up(sizeof(SegmentMetadata), grain_bytes) / grain_bytes;
    assert(segment_grains_ > md_grains_);

    uint64_t seg_bytes = segment_grains_ * grain_bytes_;
    // Number of whole segments that fit, leaving a tail for device metadata.
    num_segments_ = static_cast<uint32_t>(device_bytes_ / seg_bytes);

    // The device must have a tail past the last segment for device metadata.
    // If it's an exact multiple, drop one segment.
    if (num_segments_ > 0 &&
        static_cast<uint64_t>(num_segments_) * seg_bytes == device_bytes_) {
        --num_segments_;
    }
}

SegmentAllocator::SegmentAllocator(const SegmentGeometry& geom,
                                   uint32_t reserved_segments)
    : geom_(geom), total_(geom.num_segments()) {
    assert(reserved_segments < total_);
    // Segments [reserved_segments, total_) are initially free.
    for (uint32_t i = reserved_segments; i < total_; ++i) {
        free_list_.push_back(i);
    }
}

int32_t SegmentAllocator::allocate() {
    std::lock_guard<std::mutex> lock(mutex_);
    if (free_list_.empty()) return -1;
    uint32_t seg = free_list_.back();
    free_list_.pop_back();
    return static_cast<int32_t>(seg);
}

void SegmentAllocator::release(uint32_t seg_idx) {
    std::lock_guard<std::mutex> lock(mutex_);
    free_list_.push_back(seg_idx);
}

uint32_t SegmentAllocator::free_count() const noexcept {
    return static_cast<uint32_t>(free_list_.size());
}

GrainAllocator::GrainAllocator(uint32_t seg_idx,
                                const SegmentGeometry& geom)
    : seg_idx_(seg_idx),
      base_grain_(static_cast<uint64_t>(seg_idx) * geom.segment_grains()),
      capacity_(geom.net_grains()),
      next_grain_(0) {}

uint64_t GrainAllocator::allocate(uint64_t len) {
    if (next_grain_ + len > capacity_) return UINT64_MAX;
    uint64_t grain = base_grain_ + next_grain_;
    next_grain_ += len;
    return grain;
}

}  // namespace udepot

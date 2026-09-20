#pragma once

#include <cassert>
#include <cstdint>
#include <mutex>
#include <vector>

namespace udepot {

// On-disk device metadata — written once in the tail past the last segment.
struct __attribute__((packed)) DeviceMetadata {
    uint64_t physical_size;
    uint64_t logical_size;
    uint64_t segment_size;
    uint64_t grain_size;
    uint64_t seed;
    uint32_t csum;
};

// On-disk per-segment metadata — last grain(s) of each segment.
struct __attribute__((packed)) SegmentMetadata {
    uint64_t segment_size;
    uint64_t grain_size;
    uint64_t timestamp;
    uint64_t seed;
    uint8_t ctlr_type;
    uint32_t csum;
    uint64_t write_nr;
    uint64_t reloc_nr;
};

// Segment type — data segments hold KV pairs, directory segments hold
// hash tables.
enum class SegmentType : uint8_t {
    kData = 0,
    kDirectory = 1,
};

// Segment geometry: fixed layout derived from device and grain sizes.
//
// A device is divided into segments of segment_size grains. Each segment
// reserves its last md_grains for per-segment metadata. The net region
// (segment_size - md_grains) holds data or a directory table. Device-level
// metadata lives in the tail past the last whole segment.
class SegmentGeometry {
public:
    SegmentGeometry(uint64_t device_bytes, uint64_t grain_bytes,
                    uint64_t segment_grains);

    uint64_t grain_bytes() const noexcept { return grain_bytes_; }
    uint64_t segment_grains() const noexcept { return segment_grains_; }
    uint64_t segment_bytes() const noexcept {
        return segment_grains_ * grain_bytes_;
    }
    uint64_t md_grains() const noexcept { return md_grains_; }
    uint64_t net_grains() const noexcept {
        return segment_grains_ - md_grains_;
    }
    uint64_t net_bytes() const noexcept { return net_grains() * grain_bytes_; }
    uint32_t num_segments() const noexcept { return num_segments_; }
    uint64_t device_bytes() const noexcept { return device_bytes_; }

    // Segment N starts at this byte offset on the device.
    uint64_t segment_offset(uint32_t seg_idx) const noexcept {
        return static_cast<uint64_t>(seg_idx) * segment_bytes();
    }

    // Byte offset of the per-segment metadata within segment seg_idx.
    uint64_t seg_md_offset(uint32_t seg_idx) const noexcept {
        return segment_offset(seg_idx) + net_grains() * grain_bytes_;
    }

    // Byte offset of the device-level metadata (tail past last segment).
    uint64_t dev_md_offset() const noexcept {
        return static_cast<uint64_t>(num_segments_) * segment_bytes();
    }

    // Convert a grain address to a byte offset.
    uint64_t grain_to_byte(uint64_t grain) const noexcept {
        return grain * grain_bytes_;
    }

    // Which segment contains this grain.
    uint32_t grain_to_segment(uint64_t grain) const noexcept {
        return static_cast<uint32_t>(grain / segment_grains_);
    }

private:
    uint64_t device_bytes_;
    uint64_t grain_bytes_;
    uint64_t segment_grains_;
    uint64_t md_grains_;
    uint32_t num_segments_;
};

// Simple segment allocator. Tracks which segments are free and hands
// them out. GC is a future addition — for now this is a bump allocator
// with free-list return.
class SegmentAllocator {
public:
    explicit SegmentAllocator(const SegmentGeometry& geom,
                              uint32_t reserved_segments = 0);

    // Allocate a segment. Returns segment index, or -1 if none free.
    int32_t allocate();

    // Return a segment to the free pool.
    void release(uint32_t seg_idx);

    uint32_t free_count() const noexcept;
    uint32_t total_count() const noexcept { return total_; }

    const SegmentGeometry& geometry() const noexcept { return geom_; }

private:
    const SegmentGeometry& geom_;
    uint32_t total_;
    std::mutex mutex_;
    std::vector<uint32_t> free_list_;
};

// Within a segment, a grain allocator for KV entries. Each data segment
// appends entries sequentially (log-structured). Thread-safe via mutex.
class GrainAllocator {
public:
    GrainAllocator(uint32_t seg_idx, const SegmentGeometry& geom);

    // Allocate len grains from this segment. Returns the grain offset
    // within the device, or UINT64_MAX if the segment is full.
    uint64_t allocate(uint64_t len);

    uint64_t used_grains() const noexcept { return next_grain_; }
    uint64_t free_grains() const noexcept {
        return capacity_ - next_grain_;
    }
    bool full() const noexcept { return next_grain_ >= capacity_; }

private:
    uint32_t seg_idx_;
    uint64_t base_grain_;
    uint64_t capacity_;
    uint64_t next_grain_;
};

}  // namespace udepot

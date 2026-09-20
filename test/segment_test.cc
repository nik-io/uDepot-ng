#include "udepot/segment.h"

#include <cstdint>
#include <set>

#include <gtest/gtest.h>

using udepot::DeviceMetadata;
using udepot::GrainAllocator;
using udepot::SegmentAllocator;
using udepot::SegmentGeometry;
using udepot::SegmentMetadata;

TEST(SegmentGeometry, BasicLayout) {
    // 513 MiB device, 512-byte grains, 32K grains per segment (16 MiB).
    uint64_t dev_bytes = 513ULL * 1024 * 1024;
    SegmentGeometry geom(dev_bytes, 512, 32768);

    EXPECT_EQ(geom.grain_bytes(), 512u);
    EXPECT_EQ(geom.segment_grains(), 32768u);
    EXPECT_EQ(geom.segment_bytes(), 16ULL * 1024 * 1024);

    // 513 MiB / 16 MiB = 32 segments with 1 MiB tail.
    EXPECT_EQ(geom.num_segments(), 32u);

    // Metadata grains: sizeof(SegmentMetadata) rounded up to grain size.
    EXPECT_GE(geom.md_grains(), 1u);
    EXPECT_EQ(geom.net_grains(), geom.segment_grains() - geom.md_grains());

    // First segment at offset 0.
    EXPECT_EQ(geom.segment_offset(0), 0u);
    EXPECT_EQ(geom.segment_offset(1), geom.segment_bytes());

    // Device metadata offset is past all segments.
    EXPECT_EQ(geom.dev_md_offset(), 32ULL * 16 * 1024 * 1024);
}

TEST(SegmentGeometry, ExactMultipleDropsSegment) {
    // 512 MiB device, 16 MiB segments — exact multiple, must drop one.
    uint64_t dev_bytes = 512ULL * 1024 * 1024;
    SegmentGeometry geom(dev_bytes, 512, 32768);

    // 512 / 16 = 32, but exact multiple so we get 31.
    EXPECT_EQ(geom.num_segments(), 31u);
    EXPECT_LT(geom.dev_md_offset(), dev_bytes);
}

TEST(SegmentGeometry, GrainConversions) {
    SegmentGeometry geom(100ULL * 1024 * 1024, 4096, 1024);

    EXPECT_EQ(geom.grain_to_byte(0), 0u);
    EXPECT_EQ(geom.grain_to_byte(1), 4096u);
    EXPECT_EQ(geom.grain_to_byte(1024), 1024u * 4096);

    EXPECT_EQ(geom.grain_to_segment(0), 0u);
    EXPECT_EQ(geom.grain_to_segment(1023), 0u);
    EXPECT_EQ(geom.grain_to_segment(1024), 1u);
}

TEST(SegmentAllocator, AllocateAndRelease) {
    SegmentGeometry geom(513ULL * 1024 * 1024, 512, 32768);
    SegmentAllocator alloc(geom, 2);  // Reserve segments 0 and 1.

    uint32_t expected_free = geom.num_segments() - 2;
    EXPECT_EQ(alloc.free_count(), expected_free);

    // Allocate all free segments.
    std::set<int32_t> allocated;
    for (uint32_t i = 0; i < expected_free; ++i) {
        int32_t seg = alloc.allocate();
        EXPECT_GE(seg, 2);  // Reserved segments should not appear.
        allocated.insert(seg);
    }
    EXPECT_EQ(allocated.size(), expected_free);
    EXPECT_EQ(alloc.free_count(), 0u);

    // Next allocate should fail.
    EXPECT_EQ(alloc.allocate(), -1);

    // Release one and re-allocate.
    alloc.release(5);
    EXPECT_EQ(alloc.free_count(), 1u);
    EXPECT_EQ(alloc.allocate(), 5);
    EXPECT_EQ(alloc.free_count(), 0u);
}

TEST(GrainAllocator, SequentialAlloc) {
    SegmentGeometry geom(100ULL * 1024 * 1024, 4096, 1024);
    GrainAllocator ga(0, geom);

    uint64_t net = geom.net_grains();
    EXPECT_EQ(ga.free_grains(), net);
    EXPECT_FALSE(ga.full());

    // Allocate 10 grains.
    uint64_t g1 = ga.allocate(10);
    EXPECT_EQ(g1, 0u);
    EXPECT_EQ(ga.used_grains(), 10u);
    EXPECT_EQ(ga.free_grains(), net - 10);

    // Allocate more.
    uint64_t g2 = ga.allocate(5);
    EXPECT_EQ(g2, 10u);

    // Allocate too much.
    uint64_t g3 = ga.allocate(net);
    EXPECT_EQ(g3, UINT64_MAX);
}

TEST(GrainAllocator, SegmentBaseOffset) {
    SegmentGeometry geom(100ULL * 1024 * 1024, 4096, 1024);
    GrainAllocator ga(3, geom);

    // Segment 3 starts at grain 3*1024 = 3072.
    uint64_t g = ga.allocate(1);
    EXPECT_EQ(g, 3072u);
}

TEST(MetadataStructs, PackedSizes) {
    // Ensure packed structs match original salsa on-disk format.
    EXPECT_EQ(sizeof(DeviceMetadata), 44u);
    EXPECT_EQ(sizeof(SegmentMetadata), 53u);
}

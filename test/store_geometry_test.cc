// Copyright (c) 2024-2026 Nikolas Ioannou
// SPDX-License-Identifier: BSD-3-Clause

// The largest record the store holds: the smaller of a hash entry's 11-bit
// size and a segment's data grains. Every geometry puts a value of exactly
// max_value_bytes() and one byte more, which must be refused with -EINVAL
// at once (not -ENOSPC from salsa, not a wait for space).

#include "udepot/store.h"
#include "udepot/io/posix.h"

#include <algorithm>
#include <chrono>
#include <cerrno>
#include <cstdint>
#include <filesystem>
#include <string>
#include <vector>

#include <gtest/gtest.h>

using udepot::PosixIO;
using udepot::StoreConfig;
using udepot::uDepot;

namespace {

struct Geometry {
    const char* name;
    size_t device;
    uint32_t grain;
    uint64_t segment;       // StoreConfig::segment_size, grains (0 = auto)
    bool entry_limit_binds;  // the segment's data grains exceed 2047
};

// The segment's metadata (salsa_seg_md, 53 bytes) takes one grain at every
// grain size tested.
constexpr uint64_t kMdGrains = 1;
constexpr uint64_t kEntryLimit = 2047;  // HashEntry::kKvSizeMask

const Geometry kGeometries[] = {
    // 4 MiB device: the default segment shrinks to 264 grains.
    {"small_device_512", 4u << 20, 512, 0, false},
    // 32 MiB: segment_size asked for 512 grains, shrunk to fit.
    {"asked_segment_512", 32u << 20, 512, 512, false},
    // 64 MiB: the segment (4104 grains) is far above 2047 data grains.
    {"entry_limit_512", 64u << 20, 512, 0, true},
    {"small_device_4096", 16u << 20, 4096, 0, false},
    {"asked_segment_4096", 64u << 20, 4096, 600, false},
    {"entry_limit_4096", 256u << 20, 4096, 2100, true},
};

class GeometryTest : public ::testing::TestWithParam<Geometry> {
protected:
    void SetUp() override {
        path_ = std::filesystem::temp_directory_path() /
                ("udepot_geometry_test_" + std::string(GetParam().name));
        config_.path = path_.c_str();
        config_.size = GetParam().device;
        config_.grain_size = GetParam().grain;
        config_.segment_size = GetParam().segment;
        config_.force_destroy = true;
    }
    void TearDown() override {
        store_.close();
        std::filesystem::remove(path_);
    }

    std::filesystem::path path_;
    StoreConfig config_;
    uDepot<PosixIO> store_;
};

TEST_P(GeometryTest, LargestValueFitsAndOneByteMoreIsRefused) {
    testing::internal::CaptureStderr();
    ASSERT_EQ(store_.open(config_), 0);
    const std::string said = testing::internal::GetCapturedStderr();

    const uint64_t grain = GetParam().grain;
    const uint64_t seg = store_.segment_grains();
    ASSERT_GT(seg, kMdGrains);
    const uint64_t net = seg - kMdGrains;
    EXPECT_EQ(net > kEntryLimit, GetParam().entry_limit_binds)
        << "the geometry no longer exercises the limit it is named for: " << said;
    const uint64_t want_grains = std::min(net, kEntryLimit);
    EXPECT_EQ(store_.max_record_grains(), want_grains);
    EXPECT_EQ(store_.max_record_bytes(), want_grains * grain);

    // The open says what it ended up with.
    EXPECT_NE(said.find("the largest record is " +
                        std::to_string(want_grains) + " grains, " +
                        std::to_string(want_grains * grain) + " bytes"),
              std::string::npos)
        << said;
    EXPECT_NE(said.find(GetParam().entry_limit_binds
                            ? "Limited by a hash entry's 11-bit"
                            : "Limited by the segment"),
              std::string::npos)
        << said;

    for (size_t key_size : {size_t{1}, size_t{8}, size_t{100}}) {
        SCOPED_TRACE("key_size=" + std::to_string(key_size));
        // The record arithmetic: 14-byte header, key, value, 2-byte CRC.
        const size_t max_val = store_.max_value_bytes(key_size);
        ASSERT_EQ(max_val, want_grains * grain - (14 + key_size + 2));

        const std::string key(key_size, 'k');
        std::string val(max_val, 'v');
        val[0] = 'a';
        val[max_val - 1] = 'z';
        ASSERT_EQ(store_.put(key, val).run_sync(), 0);
        std::string out(max_val, '\0');
        size_t n = 0;
        ASSERT_EQ(store_.get(key, reinterpret_cast<uint8_t*>(out.data()),
                             out.size(), &n)
                      .run_sync(),
                  0);
        EXPECT_EQ(n, max_val);
        EXPECT_EQ(out, val);

        // One byte more is one more grain: refused up front with EINVAL,
        // not salsa's ENOSPC. (The time bound is a sanity check only; a
        // put that waited forever is caught by ctest's TIMEOUT.)
        const std::string key2 = key + "2";
        std::string too_big(store_.max_value_bytes(key2.size()) + 1, 'x');
        const auto t0 = std::chrono::steady_clock::now();
        EXPECT_EQ(store_.put(key2, too_big).run_sync(), -EINVAL);
        EXPECT_LT(std::chrono::steady_clock::now() - t0,
                  std::chrono::seconds(5));
        EXPECT_EQ(store_.exists(key2, &n).run_sync(), -ENOENT);

        // Far beyond the segment but within 2047 grains, where the
        // segment is the limit.
        if (!GetParam().entry_limit_binds) {
            std::string far((kEntryLimit - 1) * grain, 'x');
            EXPECT_EQ(store_.put(key2, far).run_sync(), -EINVAL);
        }

        // The buffer API applies the same limit.
        EXPECT_TRUE(store_.alloc_put_buffer(key_size, max_val).valid());
        // A get buffer is sized as the record it reads, so one for a
        // record no put could write is refused too (on a store whose
        // segment is under 2047 data grains, before this it was handed out).
        EXPECT_TRUE(store_.alloc_get_buffer(key_size, max_val).valid());
        EXPECT_FALSE(store_.alloc_get_buffer(key_size, max_val + 1).valid());
    }
    EXPECT_FALSE(
        store_.alloc_put_buffer(8, store_.max_value_bytes(8) + 1).valid());
}

INSTANTIATE_TEST_SUITE_P(
    Geometries, GeometryTest, ::testing::ValuesIn(kGeometries),
    [](const ::testing::TestParamInfo<Geometry>& i) { return i.param.name; });

// The geometry comes from the device metadata on a reopen, not from the
// config: the limit and the message must be the ones the store had.
TEST(Geometry, ReopenKeepsTheLimitAndSaysTheSame) {
    const auto path = std::filesystem::temp_directory_path() /
                      "udepot_geometry_test_reopen";
    StoreConfig config;
    config.path = path.c_str();
    config.size = 4u << 20;  // small: the segment is shrunk, net < 2047
    config.force_destroy = true;
    auto geometry_line = [](const std::string& err) {
        const size_t at = err.find("udepot: ");
        // the line that carries the largest record
        for (size_t pos = at; pos != std::string::npos;
             pos = err.find("udepot: ", pos + 1)) {
            const size_t end = err.find('\n', pos);
            const std::string line = err.substr(pos, end - pos);
            if (line.find("the largest record is") != std::string::npos)
                return line;
        }
        return std::string();
    };

    uint64_t grains = 0;
    size_t value8 = 0;
    std::string before;
    {
        uDepot<PosixIO> store;
        testing::internal::CaptureStderr();
        ASSERT_EQ(store.open(config), 0);
        before = geometry_line(testing::internal::GetCapturedStderr());
        ASSERT_FALSE(before.empty());
        grains = store.max_record_grains();
        value8 = store.max_value_bytes(8);
        ASSERT_LT(grains, kEntryLimit);
        ASSERT_EQ(store.put("12345678", std::string(value8, 'v')).run_sync(),
                  0);
        store.close();
    }
    config.force_destroy = false;
    uDepot<PosixIO> store;
    testing::internal::CaptureStderr();
    ASSERT_EQ(store.open(config), 0);
    const std::string after = geometry_line(testing::internal::GetCapturedStderr());
    EXPECT_TRUE(store.index_restored());
    EXPECT_EQ(store.max_record_grains(), grains);
    EXPECT_EQ(store.max_value_bytes(8), value8);
    EXPECT_EQ(after, before);
    EXPECT_EQ(store.put("12345679", std::string(value8 + 1, 'v')).run_sync(),
              -EINVAL);
    store.close();
    std::filesystem::remove(path);
}

TEST(Geometry, InvalidKeySizesAndUnopenedStoreHoldNothing) {
    uDepot<PosixIO> store;
    EXPECT_EQ(store.max_record_grains(), 0u);
    EXPECT_EQ(store.max_value_bytes(8), 0u);
    EXPECT_EQ(store.segment_grains(), 0u);

    const auto path = std::filesystem::temp_directory_path() /
                      "udepot_geometry_test_keys";
    StoreConfig config;
    config.path = path.c_str();
    config.size = 4u << 20;
    config.force_destroy = true;
    ASSERT_EQ(store.open(config), 0);
    EXPECT_EQ(store.max_value_bytes(0), 0u);
    EXPECT_EQ(store.max_value_bytes(65536), 0u);
    EXPECT_GT(store.max_value_bytes(65535), 0u);
    store.close();
    std::filesystem::remove(path);
}

}  // namespace

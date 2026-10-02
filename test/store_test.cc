// Copyright (c) 2024-2026 Nikolas Ioannou
// SPDX-License-Identifier: BSD-3-Clause

#include "udepot/store.h"
#include "udepot/io/posix.h"

#include <cerrno>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <memory>
#include <string>
#include <string_view>
#include <vector>

#include "city.h"

#include <gtest/gtest.h>

using udepot::PosixIO;
using udepot::StoreConfig;
using udepot::UDepot;

static constexpr size_t kStoreSize = 4 * 1024 * 1024;  // 4 MiB

class StoreTest : public ::testing::Test {
protected:
    void SetUp() override {
        path_ = std::filesystem::temp_directory_path() / "udepot_store_test";
        config_.path = path_.c_str();
        config_.size = kStoreSize;
        config_.grain_size = 512;
        config_.initial_tables = 2;
        config_.index_bits = 10;
        config_.force_destroy = true;

        ASSERT_EQ(store_.open(config_), 0);
    }

    void TearDown() override {
        store_.close();
        std::filesystem::remove(path_);
    }

    std::filesystem::path path_;
    StoreConfig config_;
    UDepot<PosixIO> store_;
};

// --- Put and Get ---

TEST_F(StoreTest, PutThenGetReturnsValue) {
    int rc = store_.put("hello", "world").run_sync();
    ASSERT_EQ(rc, 0);

    uint8_t val[64];
    size_t val_size = 0;
    rc = store_.get("hello", val, sizeof(val), &val_size).run_sync();
    ASSERT_EQ(rc, 0);
    EXPECT_EQ(val_size, 5u);
    EXPECT_EQ(std::string_view(reinterpret_cast<char*>(val), val_size),
              "world");
}

TEST_F(StoreTest, GetNonexistentKeyReturnsNotFound) {
    uint8_t val[64];
    size_t val_size = 0;
    int rc = store_.get("nosuchkey", val, sizeof(val), &val_size).run_sync();
    EXPECT_NE(rc, 0);
}

TEST_F(StoreTest, PutMultipleKeysGetEach) {
    ASSERT_EQ(store_.put("k1", "val1").run_sync(), 0);
    ASSERT_EQ(store_.put("k2", "val2").run_sync(), 0);
    ASSERT_EQ(store_.put("k3", "val3").run_sync(), 0);

    uint8_t val[64];
    size_t val_size = 0;

    ASSERT_EQ(store_.get("k1", val, sizeof(val), &val_size).run_sync(), 0);
    EXPECT_EQ(std::string_view(reinterpret_cast<char*>(val), val_size),
              "val1");

    ASSERT_EQ(store_.get("k2", val, sizeof(val), &val_size).run_sync(), 0);
    EXPECT_EQ(std::string_view(reinterpret_cast<char*>(val), val_size),
              "val2");

    ASSERT_EQ(store_.get("k3", val, sizeof(val), &val_size).run_sync(), 0);
    EXPECT_EQ(std::string_view(reinterpret_cast<char*>(val), val_size),
              "val3");
}

TEST_F(StoreTest, LargeValueRoundTrips) {
    std::vector<uint8_t> large_val(4096, 0xAB);
    auto key = std::span<const uint8_t>(
        reinterpret_cast<const uint8_t*>("bigkey"), 6);
    auto val = std::span<const uint8_t>(large_val.data(), large_val.size());

    ASSERT_EQ(store_.put(key, val).run_sync(), 0);

    std::vector<uint8_t> result(4096);
    size_t val_size = 0;
    ASSERT_EQ(store_.get(key, result.data(), result.size(),
                         &val_size).run_sync(), 0);
    EXPECT_EQ(val_size, 4096u);
    EXPECT_EQ(result, large_val);
}

TEST_F(StoreTest, EmptyValueRoundTrips) {
    ASSERT_EQ(store_.put("empty", "").run_sync(), 0);

    uint8_t val[64];
    size_t val_size = 99;
    int rc = store_.get("empty", val, sizeof(val), &val_size).run_sync();
    ASSERT_EQ(rc, 0);
    EXPECT_EQ(val_size, 0u);
}

TEST_F(StoreTest, ValueTruncatedToBufferSize) {
    ASSERT_EQ(store_.put("key", "long_value_here").run_sync(), 0);

    uint8_t val[4];
    size_t val_size = 0;
    int rc = store_.get("key", val, sizeof(val), &val_size).run_sync();
    ASSERT_EQ(rc, 0);
    EXPECT_EQ(val_size, 15u);  // full size reported
    EXPECT_EQ(std::string_view(reinterpret_cast<char*>(val), 4), "long");
}

// --- Delete ---

TEST_F(StoreTest, DeleteRemovesKey) {
    ASSERT_EQ(store_.put("to_delete", "val").run_sync(), 0);
    ASSERT_EQ(store_.del("to_delete").run_sync(), 0);

    uint8_t val[64];
    size_t val_size = 0;
    EXPECT_NE(store_.get("to_delete", val, sizeof(val),
                         &val_size).run_sync(), 0);
}

TEST_F(StoreTest, DeleteNonexistentKeyReturnsNotFound) {
    int rc = store_.del("nosuchkey").run_sync();
    EXPECT_NE(rc, 0);
}

TEST_F(StoreTest, DeleteDoesNotAffectOtherKeys) {
    ASSERT_EQ(store_.put("keep", "keeper").run_sync(), 0);
    ASSERT_EQ(store_.put("drop", "dropper").run_sync(), 0);

    ASSERT_EQ(store_.del("drop").run_sync(), 0);

    uint8_t val[64];
    size_t val_size = 0;
    ASSERT_EQ(store_.get("keep", val, sizeof(val), &val_size).run_sync(), 0);
    EXPECT_EQ(std::string_view(reinterpret_cast<char*>(val), val_size),
              "keeper");
}

// --- Exists ---

TEST_F(StoreTest, ExistsReturnsSizeForPresentKey) {
    ASSERT_EQ(store_.put("present", "12345").run_sync(), 0);

    size_t val_size = 0;
    int rc = store_.exists("present", &val_size).run_sync();
    ASSERT_EQ(rc, 0);
    EXPECT_EQ(val_size, 5u);
}

TEST_F(StoreTest, ExistsReturnsNotFoundForMissingKey) {
    size_t val_size = 0;
    int rc = store_.exists("missing", &val_size).run_sync();
    EXPECT_NE(rc, 0);
}

TEST_F(StoreTest, ExistsReturnsNotFoundAfterDelete) {
    ASSERT_EQ(store_.put("temp", "data").run_sync(), 0);
    ASSERT_EQ(store_.del("temp").run_sync(), 0);

    size_t val_size = 0;
    EXPECT_NE(store_.exists("temp", &val_size).run_sync(), 0);
}

// --- Edge cases ---

TEST_F(StoreTest, EmptyKeyIsRejected) {
    int rc = store_.put("", "val").run_sync();
    EXPECT_NE(rc, 0);
}

TEST_F(StoreTest, ManyKeysRoundTrip) {
    constexpr int kCount = 200;
    for (int i = 0; i < kCount; ++i) {
        std::string key = "key_" + std::to_string(i);
        std::string val = "val_" + std::to_string(i);
        ASSERT_EQ(store_.put(key, val).run_sync(), 0) << "put i=" << i;
    }

    for (int i = 0; i < kCount; ++i) {
        std::string key = "key_" + std::to_string(i);
        std::string expected = "val_" + std::to_string(i);
        uint8_t val[64];
        size_t val_size = 0;
        ASSERT_EQ(store_.get(key, val, sizeof(val), &val_size).run_sync(), 0)
            << "get i=" << i;
        EXPECT_EQ(std::string_view(reinterpret_cast<char*>(val), val_size),
                  expected)
            << "i=" << i;
    }
}

TEST_F(StoreTest, DataSurvivesDirectoryGrow) {
    // Insert enough entries to need a grow, then verify all are readable.
    constexpr int kCount = 100;
    for (int i = 0; i < kCount; ++i) {
        std::string key = "grow_key_" + std::to_string(i);
        std::string val = "grow_val_" + std::to_string(i);
        ASSERT_EQ(store_.put(key, val).run_sync(), 0) << "put i=" << i;
    }

    ASSERT_EQ(store_.directory().grow(), 0);

    for (int i = 0; i < kCount; ++i) {
        std::string key = "grow_key_" + std::to_string(i);
        std::string expected = "grow_val_" + std::to_string(i);
        uint8_t val[64];
        size_t val_size = 0;
        ASSERT_EQ(store_.get(key, val, sizeof(val), &val_size).run_sync(), 0)
            << "get after grow i=" << i;
        EXPECT_EQ(std::string_view(reinterpret_cast<char*>(val), val_size),
                  expected)
            << "i=" << i;
    }
}

// --- CRC integrity ---
// CRC verification on get is debug-only (matching uDepot's
// _UDEPOT_DATA_DEBUG_VERIFY), so this test only runs in debug builds.

#ifndef NDEBUG
TEST_F(StoreTest, CorruptedDataDetectedOnGet) {
    ASSERT_EQ(store_.put("crc_key", "crc_val").run_sync(), 0);

    // Look up the PBA from the directory (salsa allocates grains, so the
    // first put does not necessarily land at grain 0).
    uint64_t hash = store_.hash_key(std::span<const uint8_t>(
        reinterpret_cast<const uint8_t*>("crc_key"), 7));
    uint32_t rcu_idx = store_.rcu().read_lock();
    udepot::HashEntry entry = store_.directory().lookup(hash);
    store_.rcu().read_unlock(rcu_idx);
    ASSERT_FALSE(entry.empty());

    off_t offset = static_cast<off_t>(entry.pba()) * store_.grain_size();

    // Corrupt the on-disk header metadata so the CRC (which covers only
    // the header, matching uDepot) won't match.
    uint8_t grain[512];
    ssize_t nread = store_.io().pread(grain, sizeof(grain), offset)
                        .run_sync();
    ASSERT_EQ(nread, 512);

    // Flip the timestamp field in the header (bytes 6..13) while keeping
    // key_size and val_size intact so lookup still finds the entry.
    udepot::KvHeader hdr;
    std::memcpy(&hdr, grain, sizeof(hdr));
    hdr.timestamp ^= 0xDEADBEEF;
    std::memcpy(grain, &hdr, sizeof(hdr));

    ssize_t written = store_.io().pwrite(grain, sizeof(grain), offset)
                          .run_sync();
    ASSERT_EQ(written, 512);

    uint8_t val[64];
    size_t val_size = 0;
    int rc = store_.get("crc_key", val, sizeof(val), &val_size).run_sync();
    EXPECT_NE(rc, 0);  // Should fail due to CRC mismatch.
}
#endif

// --- Tag collision tests ---
// Two keys that hash to the same bucket with the same 8-bit tag but are
// different keys.  The store must find each one via disk verification,
// not stop at the first tag match.

// Brute-force search for a pair of short keys whose CityHash64 values
// share the same tag (top 8 bits) and bucket (low index_bits bits).
static std::pair<std::string, std::string> find_colliding_keys(
    uint32_t index_bits) {
    uint64_t bucket_mask = (1ULL << index_bits) - 1;
    // Try sequential integer keys; CityHash spreads them well, so two
    // that collide on both tag and bucket take a little searching.
    for (int a = 0; a < 100000; ++a) {
        std::string ka = "col_a_" + std::to_string(a);
        uint64_t ha = CityHash64(ka.data(), ka.size());
        uint8_t tag_a = static_cast<uint8_t>(ha >> 56);
        uint64_t bucket_a = ha & bucket_mask;

        for (int b = a + 1; b < a + 200; ++b) {
            std::string kb = "col_b_" + std::to_string(b);
            uint64_t hb = CityHash64(kb.data(), kb.size());
            uint8_t tag_b = static_cast<uint8_t>(hb >> 56);
            uint64_t bucket_b = hb & bucket_mask;

            if (tag_a == tag_b && bucket_a == bucket_b)
                return {ka, kb};
        }
    }
    // Collision must be found — 10-bit bucket × 8-bit tag = 18 bits, so
    // any window of ~500k pairs will contain dozens.
    ADD_FAILURE() << "no colliding pair found";
    return {"", ""};
}

TEST_F(StoreTest, GetWithTagCollisionReturnsCorrectValue) {
    auto [key_a, key_b] = find_colliding_keys(config_.index_bits);
    ASSERT_FALSE(key_a.empty());

    ASSERT_EQ(store_.put(key_a, "val_a").run_sync(), 0);
    ASSERT_EQ(store_.put(key_b, "val_b").run_sync(), 0);

    uint8_t val[64];
    size_t val_size = 0;

    ASSERT_EQ(store_.get(key_a, val, sizeof(val), &val_size).run_sync(), 0);
    EXPECT_EQ(std::string_view(reinterpret_cast<char*>(val), val_size),
              "val_a");

    ASSERT_EQ(store_.get(key_b, val, sizeof(val), &val_size).run_sync(), 0);
    EXPECT_EQ(std::string_view(reinterpret_cast<char*>(val), val_size),
              "val_b");
}

TEST_F(StoreTest, DeleteWithTagCollisionRemovesCorrectKey) {
    auto [key_a, key_b] = find_colliding_keys(config_.index_bits);
    ASSERT_FALSE(key_a.empty());

    ASSERT_EQ(store_.put(key_a, "val_a").run_sync(), 0);
    ASSERT_EQ(store_.put(key_b, "val_b").run_sync(), 0);

    // Delete key_a; key_b must survive.
    ASSERT_EQ(store_.del(key_a).run_sync(), 0);

    uint8_t val[64];
    size_t val_size = 0;
    EXPECT_NE(store_.get(key_a, val, sizeof(val), &val_size).run_sync(), 0);

    ASSERT_EQ(store_.get(key_b, val, sizeof(val), &val_size).run_sync(), 0);
    EXPECT_EQ(std::string_view(reinterpret_cast<char*>(val), val_size),
              "val_b");
}

TEST_F(StoreTest, ExistsWithTagCollisionFindsCorrectKey) {
    auto [key_a, key_b] = find_colliding_keys(config_.index_bits);
    ASSERT_FALSE(key_a.empty());

    ASSERT_EQ(store_.put(key_a, "aaa").run_sync(), 0);
    ASSERT_EQ(store_.put(key_b, "bbbbb").run_sync(), 0);

    size_t val_size = 0;
    ASSERT_EQ(store_.exists(key_a, &val_size).run_sync(), 0);
    EXPECT_EQ(val_size, 3u);

    ASSERT_EQ(store_.exists(key_b, &val_size).run_sync(), 0);
    EXPECT_EQ(val_size, 5u);
}

// --- CRC table-based vs bit-by-bit equivalence ---

// zlib's crc32(crc, buf, len), bit by bit, independent of the store's table.
static uint32_t crc32_bitwise(uint32_t crc, const uint8_t* data, size_t len) {
    crc = ~crc;
    for (size_t i = 0; i < len; ++i) {
        crc ^= data[i];
        for (int j = 0; j < 8; ++j)
            crc = (crc >> 1) ^ (0xEDB88320u & (0u - (crc & 1u)));
    }
    return ~crc;
}

// --- Upsert (overwrite) ---

TEST_F(StoreTest, PutOverwriteUpdatesValue) {
    ASSERT_EQ(store_.put("key", "old_val").run_sync(), 0);
    ASSERT_EQ(store_.put("key", "new_val").run_sync(), 0);

    uint8_t val[64];
    size_t val_size = 0;
    ASSERT_EQ(store_.get("key", val, sizeof(val), &val_size).run_sync(), 0);
    EXPECT_EQ(val_size, 7u);
    EXPECT_EQ(std::string_view(reinterpret_cast<char*>(val), val_size),
              "new_val");
}

TEST_F(StoreTest, PutOverwriteDoesNotLeaveOldValue) {
    ASSERT_EQ(store_.put("dup", "first").run_sync(), 0);
    ASSERT_EQ(store_.put("dup", "second").run_sync(), 0);

    // After overwrite, only one directory entry should exist for "dup".
    uint64_t hash = store_.hash_key(std::span<const uint8_t>(
        reinterpret_cast<const uint8_t*>("dup"), 3));

    uint32_t rcu_idx = store_.rcu().read_lock();

    int count = 0;
    for (uint32_t start = 0; ; ) {
        udepot::HashEntry entry = store_.directory().lookup(hash, start);
        if (entry.empty()) break;
        start = entry.bucket_offset() + 1;
        ++count;
    }

    store_.rcu().read_unlock(rcu_idx);

    EXPECT_EQ(count, 1) << "overwrite must not create duplicate entries";
}

TEST_F(StoreTest, PutOverwriteWithDifferentSize) {
    ASSERT_EQ(store_.put("resize", "short").run_sync(), 0);

    std::vector<uint8_t> big_val(2048, 0xCC);
    auto key = std::span<const uint8_t>(
        reinterpret_cast<const uint8_t*>("resize"), 6);
    auto val = std::span<const uint8_t>(big_val.data(), big_val.size());
    ASSERT_EQ(store_.put(key, val).run_sync(), 0);

    std::vector<uint8_t> out(2048);
    size_t val_size = 0;
    ASSERT_EQ(store_.get(key, out.data(), out.size(), &val_size).run_sync(), 0);
    EXPECT_EQ(val_size, 2048u);
    EXPECT_EQ(out, big_val);
}

TEST_F(StoreTest, PutOverwriteThenDelete) {
    ASSERT_EQ(store_.put("od", "v1").run_sync(), 0);
    ASSERT_EQ(store_.put("od", "v2").run_sync(), 0);
    ASSERT_EQ(store_.del("od").run_sync(), 0);

    uint8_t val[64];
    size_t val_size = 0;
    EXPECT_NE(store_.get("od", val, sizeof(val), &val_size).run_sync(), 0);
}

TEST_F(StoreTest, PutOverwriteMultipleTimes) {
    for (int i = 0; i < 10; ++i) {
        std::string val = "iteration_" + std::to_string(i);
        ASSERT_EQ(store_.put("multi", val).run_sync(), 0);
    }

    uint8_t val[64];
    size_t val_size = 0;
    ASSERT_EQ(store_.get("multi", val, sizeof(val), &val_size).run_sync(), 0);
    EXPECT_EQ(std::string_view(reinterpret_cast<char*>(val), val_size),
              "iteration_9");
}

TEST_F(StoreTest, PutOverwriteDoesNotAffectOtherKeys) {
    ASSERT_EQ(store_.put("a", "a_val").run_sync(), 0);
    ASSERT_EQ(store_.put("b", "b_val").run_sync(), 0);
    ASSERT_EQ(store_.put("a", "a_new").run_sync(), 0);

    uint8_t val[64];
    size_t val_size = 0;

    ASSERT_EQ(store_.get("a", val, sizeof(val), &val_size).run_sync(), 0);
    EXPECT_EQ(std::string_view(reinterpret_cast<char*>(val), val_size),
              "a_new");

    ASSERT_EQ(store_.get("b", val, sizeof(val), &val_size).run_sync(), 0);
    EXPECT_EQ(std::string_view(reinterpret_cast<char*>(val), val_size),
              "b_val");
}

TEST_F(StoreTest, PutOverwriteExistsReportsNewSize) {
    ASSERT_EQ(store_.put("ex", "ab").run_sync(), 0);
    ASSERT_EQ(store_.put("ex", "abcdef").run_sync(), 0);

    size_t val_size = 0;
    ASSERT_EQ(store_.exists("ex", &val_size).run_sync(), 0);
    EXPECT_EQ(val_size, 6u);
}

TEST_F(StoreTest, CrcTableMatchesBitwiseForKnownPatterns) {
    // Put a key/value pair; read it back via raw I/O and verify the
    // on-disk CRC matches the reference bitwise implementation.
    // As uDepot's checksum16(timestamp, md): a CRC32 seeded with the
    // segment timestamp, over the header, then over the device seed,
    // truncated to 16 bits.
    std::string key = "crc_check_key";
    std::vector<uint8_t> val(256);
    for (size_t i = 0; i < val.size(); ++i)
        val[i] = static_cast<uint8_t>(i);

    auto key_span = std::span<const uint8_t>(
        reinterpret_cast<const uint8_t*>(key.data()), key.size());
    auto val_span = std::span<const uint8_t>(val);

    ASSERT_EQ(store_.put(key_span, val_span).run_sync(), 0);

    // Look up the PBA from the directory.
    uint64_t hash = store_.hash_key(key_span);
    uint32_t rcu_idx = store_.rcu().read_lock();
    udepot::HashEntry entry = store_.directory().lookup(hash);
    store_.rcu().read_unlock(rcu_idx);
    ASSERT_FALSE(entry.empty());

    off_t offset = static_cast<off_t>(entry.pba()) * store_.grain_size();

    // Read the raw on-disk entry at the allocated PBA.
    size_t entry_bytes = sizeof(udepot::KvHeader) + key.size() + val.size() +
                         sizeof(udepot::KvSuffix);
    size_t grains = (entry_bytes + config_.grain_size - 1) / config_.grain_size;
    size_t read_size = grains * config_.grain_size;

    std::vector<uint8_t> buf(read_size);
    ssize_t nread = store_.io().pread(buf.data(), read_size, offset)
                        .run_sync();
    ASSERT_EQ(nread, static_cast<ssize_t>(read_size));

    udepot::KvHeader hdr;
    std::memcpy(&hdr, buf.data(), sizeof(hdr));
    const uint64_t seed = store_.seed();
    uint32_t crc = crc32_bitwise(static_cast<uint32_t>(hdr.timestamp),
                                 buf.data(), sizeof(udepot::KvHeader));
    crc = crc32_bitwise(crc, reinterpret_cast<const uint8_t*>(&seed),
                        sizeof(seed));
    uint16_t ref_crc = static_cast<uint16_t>(crc);

    // Read the stored CRC from the suffix.
    size_t suffix_offset = sizeof(udepot::KvHeader) + key.size() + val.size();
    udepot::KvSuffix suffix;
    std::memcpy(&suffix, buf.data() + suffix_offset, sizeof(suffix));

    EXPECT_EQ(suffix.crc16, ref_crc)
        << "On-disk CRC is uDepot's checksum16 over the header and seed";
}

TEST_F(StoreTest, PutCreateFailsIfKeyExists) {
    EXPECT_EQ(store_.put("k", "v1", udepot::PutMode::kCreate).run_sync(), 0);
    EXPECT_EQ(store_.put("k", "v2", udepot::PutMode::kCreate).run_sync(),
              -EEXIST);
    char val[8];
    size_t n = 0;
    ASSERT_EQ(store_.get("k", reinterpret_cast<uint8_t*>(val), sizeof(val),
                         &n).run_sync(), 0);
    EXPECT_EQ(std::string_view(val, n), "v1");
}

TEST_F(StoreTest, PutReplaceFailsIfKeyMissing) {
    EXPECT_EQ(store_.put("k", "v", udepot::PutMode::kReplace).run_sync(),
              -ENOENT);
    size_t n = 0;
    EXPECT_EQ(store_.get("k", nullptr, 0, &n).run_sync(), -ENOENT);
    ASSERT_EQ(store_.put("k", "v1").run_sync(), 0);
    EXPECT_EQ(store_.put("k", "v2", udepot::PutMode::kReplace).run_sync(), 0);
}

TEST_F(StoreTest, VersionChangesOnEveryPut) {
    uint64_t v1 = udepot::kAnyVersion, v2 = udepot::kAnyVersion;
    size_t n = 0;
    ASSERT_EQ(store_.put("k", "a").run_sync(), 0);
    ASSERT_EQ(store_.get("k", nullptr, 0, &n, &v1).run_sync(), 0);
    ASSERT_EQ(store_.put("k", "a").run_sync(), 0);
    ASSERT_EQ(store_.get("k", nullptr, 0, &n, &v2).run_sync(), 0);
    EXPECT_NE(v1, udepot::kAnyVersion);
    EXPECT_NE(v1, v2);
}

TEST_F(StoreTest, PutIfVersionRejectsStaleVersion) {
    uint64_t v1 = 0, v2 = 0;
    size_t n = 0;
    ASSERT_EQ(store_.put("k", "a").run_sync(), 0);
    ASSERT_EQ(store_.get("k", nullptr, 0, &n, &v1).run_sync(), 0);
    ASSERT_EQ(store_.put("k", "b", udepot::PutMode::kUpsert, v1).run_sync(), 0);
    // v1 is gone now; a second writer holding it must lose.
    EXPECT_EQ(store_.put("k", "c", udepot::PutMode::kUpsert, v1).run_sync(),
              -ESTALE);
    char val[8];
    ASSERT_EQ(store_.get("k", reinterpret_cast<uint8_t*>(val), sizeof(val),
                         &n, &v2).run_sync(), 0);
    EXPECT_EQ(std::string_view(val, n), "b");
    // A version check implies the key must exist.
    EXPECT_EQ(store_.put("missing", "x", udepot::PutMode::kUpsert, v2)
                  .run_sync(), -ENOENT);
}

TEST_F(StoreTest, DelIfVersion) {
    uint64_t v1 = 0, v2 = 0;
    size_t n = 0;
    ASSERT_EQ(store_.put("k", "a").run_sync(), 0);
    ASSERT_EQ(store_.get("k", nullptr, 0, &n, &v1).run_sync(), 0);
    ASSERT_EQ(store_.put("k", "b").run_sync(), 0);
    EXPECT_EQ(store_.del("k", v1).run_sync(), -ESTALE);
    ASSERT_EQ(store_.get("k", nullptr, 0, &n, &v2).run_sync(), 0);
    EXPECT_EQ(store_.del("k", v2).run_sync(), 0);
    EXPECT_EQ(store_.get("k", nullptr, 0, &n).run_sync(), -ENOENT);
}

// ── Zero-copy interface ─────────────────────────────────────────────────────

namespace {
std::span<const uint8_t> bytes(std::string_view s) {
    return {reinterpret_cast<const uint8_t*>(s.data()), s.size()};
}
std::string_view text(std::span<const uint8_t> s) {
    return {reinterpret_cast<const char*>(s.data()), s.size()};
}
}  // namespace

TEST_F(StoreTest, ZeroCopyPutThenZeroCopyGet) {
    const std::string val(3000, 'z');
    auto pb = store_.alloc_put_buffer(5, val.size());
    ASSERT_TRUE(pb.valid());
    ASSERT_EQ(pb.value().size(), val.size());
    std::memcpy(pb.value().data(), val.data(), val.size());
    ASSERT_EQ(store_.put(bytes("zckey"), pb).run_sync(), 0);

    udepot::GetBuffer gb;
    uint64_t version = 0;
    ASSERT_EQ(store_.get(bytes("zckey"), &gb, &version).run_sync(), 0);
    ASSERT_TRUE(gb.valid());
    EXPECT_EQ(text(gb.value()), val);
    EXPECT_NE(version, 0u);

    // The copying interface reads the same record.
    std::string out(val.size(), '\0');
    size_t n = 0;
    ASSERT_EQ(store_.get("zckey", reinterpret_cast<uint8_t*>(out.data()),
                         out.size(), &n).run_sync(), 0);
    EXPECT_EQ(out, val);
}

TEST_F(StoreTest, ZeroCopyGetSeesCopyingPut) {
    ASSERT_EQ(store_.put("plain", "value").run_sync(), 0);
    udepot::GetBuffer gb;
    ASSERT_EQ(store_.get(bytes("plain"), &gb).run_sync(), 0);
    EXPECT_EQ(text(gb.value()), "value");
}

// A PutBuffer stays the caller's: it can be refilled and put again, under
// the same key (overwrite) or another of the same size.
TEST_F(StoreTest, ZeroCopyPutBufferIsReusable) {
    auto pb = store_.alloc_put_buffer(4, 8);
    ASSERT_TRUE(pb.valid());
    std::memcpy(pb.value().data(), "aaaaaaaa", 8);
    ASSERT_EQ(store_.put(bytes("key1"), pb).run_sync(), 0);
    std::memcpy(pb.value().data(), "bbbbbbbb", 8);
    ASSERT_EQ(store_.put(bytes("key2"), pb).run_sync(), 0);
    std::memcpy(pb.value().data(), "cccccccc", 8);
    ASSERT_EQ(store_.put(bytes("key1"), pb).run_sync(), 0);

    udepot::GetBuffer gb;
    ASSERT_EQ(store_.get(bytes("key1"), &gb).run_sync(), 0);
    EXPECT_EQ(text(gb.value()), "cccccccc");
    ASSERT_EQ(store_.get(bytes("key2"), &gb).run_sync(), 0);
    EXPECT_EQ(text(gb.value()), "bbbbbbbb");
}

TEST_F(StoreTest, ZeroCopyPutHonoursModes) {
    auto pb = store_.alloc_put_buffer(3, 1);
    ASSERT_TRUE(pb.valid());
    pb.value()[0] = 'x';
    EXPECT_EQ(store_.put(bytes("abc"), pb, udepot::PutMode::kReplace)
                  .run_sync(), -ENOENT);
    EXPECT_EQ(store_.put(bytes("abc"), pb, udepot::PutMode::kCreate)
                  .run_sync(), 0);
    EXPECT_EQ(store_.put(bytes("abc"), pb, udepot::PutMode::kCreate)
                  .run_sync(), -EEXIST);
}

TEST_F(StoreTest, ZeroCopyErrors) {
    // Sizes the store cannot hold give no buffer.
    EXPECT_FALSE(store_.alloc_put_buffer(0, 10).valid());
    EXPECT_FALSE(store_.alloc_put_buffer(UINT16_MAX + 1, 10).valid());
    EXPECT_FALSE(store_.alloc_put_buffer(10, 2u << 20).valid());

    // A key of another size than the buffer was made for, or no buffer.
    auto pb = store_.alloc_put_buffer(4, 4);
    ASSERT_TRUE(pb.valid());
    EXPECT_EQ(store_.put(bytes("toolong"), pb).run_sync(), -EINVAL);
    udepot::PutBuffer empty;
    EXPECT_EQ(store_.put(bytes("k"), empty).run_sync(), -EINVAL);

    // A missing key leaves the GetBuffer empty, even if it held a value.
    ASSERT_EQ(store_.put("have", "it").run_sync(), 0);
    udepot::GetBuffer gb;
    ASSERT_EQ(store_.get(bytes("have"), &gb).run_sync(), 0);
    EXPECT_EQ(store_.get(bytes("missing"), &gb).run_sync(), -ENOENT);
    EXPECT_FALSE(gb.valid());
    EXPECT_EQ(store_.get(std::span<const uint8_t>{}, &gb).run_sync(), -EINVAL);
}

// Recovery reads zero-copy records like any other.
TEST_F(StoreTest, ZeroCopyPutSurvivesReopen) {
    auto pb = store_.alloc_put_buffer(7, 100);
    ASSERT_TRUE(pb.valid());
    std::memset(pb.value().data(), 'r', 100);
    ASSERT_EQ(store_.put(bytes("durable"), pb).run_sync(), 0);
    store_.close();
    config_.force_destroy = false;
    ASSERT_EQ(store_.open(config_), 0);
    udepot::GetBuffer gb;
    ASSERT_EQ(store_.get(bytes("durable"), &gb).run_sync(), 0);
    EXPECT_EQ(text(gb.value()), std::string(100, 'r'));
}

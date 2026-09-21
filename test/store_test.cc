#include "udepot/store.h"
#include "udepot/io/posix.h"

#include <cstdint>
#include <cstdio>
#include <cstring>
#include <filesystem>
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

TEST_F(StoreTest, CorruptedDataDetectedOnGet) {
    ASSERT_EQ(store_.put("crc_key", "crc_val").run_sync(), 0);

    // Corrupt the on-disk data by writing garbage at the stored location.
    // We know the first put goes to grain 0.
    uint8_t garbage[512];
    std::memset(garbage, 0xFF, sizeof(garbage));
    // Preserve the header so lookup finds it, but corrupt the value.
    udepot::KvHeader hdr;
    hdr.key_size = 7;  // "crc_key"
    hdr.val_size = 7;  // "crc_val"
    hdr.timestamp = 0;
    std::memcpy(garbage, &hdr, sizeof(hdr));
    std::memcpy(garbage + sizeof(hdr), "crc_key", 7);
    // Value is now 0xFF bytes, CRC won't match.

    ssize_t written = store_.io().pwrite(garbage, sizeof(garbage), 0)
                          .run_sync();
    ASSERT_EQ(written, 512);

    uint8_t val[64];
    size_t val_size = 0;
    int rc = store_.get("crc_key", val, sizeof(val), &val_size).run_sync();
    EXPECT_NE(rc, 0);  // Should fail due to CRC mismatch.
}

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

static uint16_t crc16_bitwise(const uint8_t* data, size_t len) {
    uint16_t crc = 0xFFFF;
    for (size_t i = 0; i < len; ++i) {
        crc ^= static_cast<uint16_t>(data[i]) << 8;
        for (int j = 0; j < 8; ++j)
            crc = (crc & 0x8000) ? (crc << 1) ^ 0x1021 : crc << 1;
    }
    return crc;
}

TEST_F(StoreTest, CrcTableMatchesBitwiseForKnownPatterns) {
    // Put a key/value pair; read it back via raw I/O and verify the
    // on-disk CRC matches the reference bitwise implementation.
    std::string key = "crc_check_key";
    std::vector<uint8_t> val(256);
    for (size_t i = 0; i < val.size(); ++i)
        val[i] = static_cast<uint8_t>(i);

    auto key_span = std::span<const uint8_t>(
        reinterpret_cast<const uint8_t*>(key.data()), key.size());
    auto val_span = std::span<const uint8_t>(val);

    ASSERT_EQ(store_.put(key_span, val_span).run_sync(), 0);

    // Read the raw on-disk entry from grain 0.
    size_t entry_bytes = sizeof(udepot::KvHeader) + key.size() + val.size() +
                         sizeof(udepot::KvSuffix);
    size_t grains = (entry_bytes + config_.grain_size - 1) / config_.grain_size;
    size_t read_size = grains * config_.grain_size;

    std::vector<uint8_t> buf(read_size);
    ssize_t nread = store_.io().pread(buf.data(), read_size, 0).run_sync();
    ASSERT_EQ(nread, static_cast<ssize_t>(read_size));

    // Compute reference CRC over header + key + value.
    uint16_t ref_crc = 0xFFFF;
    // Header
    ref_crc = crc16_bitwise(buf.data(), sizeof(udepot::KvHeader));
    // To chain properly, feed remaining data byte-by-byte into the same state.
    // Simpler: just compute over the whole prefix.
    size_t crc_input_len = sizeof(udepot::KvHeader) + key.size() + val.size();
    ref_crc = crc16_bitwise(buf.data(), crc_input_len);

    // Read the stored CRC from the suffix.
    udepot::KvSuffix suffix;
    std::memcpy(&suffix, buf.data() + crc_input_len, sizeof(suffix));

    EXPECT_EQ(suffix.crc16, ref_crc)
        << "On-disk CRC (from table-based compute_crc16) must match "
           "reference bitwise CRC-CCITT";
}

#include "udepot/store.h"
#include "udepot/io/posix.h"

#include <cstdint>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <string>
#include <string_view>
#include <vector>

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

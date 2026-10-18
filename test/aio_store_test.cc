// Copyright (c) 2024-2026 Nikolas Ioannou
// SPDX-License-Identifier: BSD-3-Clause

#include "udepot/store.h"
#include "udepot/io/aio.h"

#include <algorithm>
#include <chrono>
#include <atomic>
#include <cstdio>
#include <cstdint>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <memory>
#include <string>
#include <string_view>
#include <thread>
#include <vector>

#include <gtest/gtest.h>

using udepot::AioIO;
using udepot::StoreConfig;
using udepot::uDepot;

static constexpr size_t kStoreSize = 4 * 1024 * 1024;

class AioStoreTest : public ::testing::Test {
protected:
    void SetUp() override {
        path_ = std::filesystem::temp_directory_path() / "udepot_aio_store_test";
        config_.path = path_.c_str();
        config_.size = kStoreSize;
        config_.grain_size = 512;
        config_.force_destroy = true;

        ASSERT_EQ(store_.open(config_), 0);
    }

    void TearDown() override {
        store_.close();
        std::filesystem::remove(path_);
    }

    std::filesystem::path path_;
    StoreConfig config_;
    uDepot<AioIO> store_;
};

TEST_F(AioStoreTest, PutThenGetReturnsValue) {
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

TEST_F(AioStoreTest, GetNonexistentKeyReturnsNotFound) {
    uint8_t val[64];
    size_t val_size = 0;
    int rc = store_.get("nosuchkey", val, sizeof(val), &val_size).run_sync();
    EXPECT_NE(rc, 0);
}

TEST_F(AioStoreTest, PutMultipleKeysGetEach) {
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

TEST_F(AioStoreTest, LargeValueRoundTrips) {
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

TEST_F(AioStoreTest, DeleteRemovesKey) {
    ASSERT_EQ(store_.put("to_delete", "val").run_sync(), 0);
    ASSERT_EQ(store_.del("to_delete").run_sync(), 0);

    uint8_t val[64];
    size_t val_size = 0;
    EXPECT_NE(store_.get("to_delete", val, sizeof(val),
                         &val_size).run_sync(), 0);
}

TEST_F(AioStoreTest, DeleteNonexistentKeyReturnsNotFound) {
    int rc = store_.del("nosuchkey").run_sync();
    EXPECT_NE(rc, 0);
}

TEST_F(AioStoreTest, ExistsReturnsSizeForPresentKey) {
    ASSERT_EQ(store_.put("present", "12345").run_sync(), 0);

    size_t val_size = 0;
    int rc = store_.exists("present", &val_size).run_sync();
    ASSERT_EQ(rc, 0);
    EXPECT_EQ(val_size, 5u);
}

TEST_F(AioStoreTest, ExistsReturnsNotFoundForMissingKey) {
    size_t val_size = 0;
    int rc = store_.exists("missing", &val_size).run_sync();
    EXPECT_NE(rc, 0);
}

TEST_F(AioStoreTest, ManyKeysRoundTrip) {
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

// ── Zero-copy interface ─────────────────────────────────────────────────────

namespace {
std::span<const uint8_t> zc_bytes(std::string_view s) {
    return {reinterpret_cast<const uint8_t*>(s.data()), s.size()};
}
std::string_view zc_text(std::span<const uint8_t> s) {
    return {reinterpret_cast<const char*>(s.data()), s.size()};
}
}  // namespace

TEST_F(AioStoreTest, ZeroCopyRoundTrip) {
    const std::string val(5000, 'q');
    auto pb = store_.alloc_put_buffer(4, val.size());
    ASSERT_TRUE(pb.valid());
    std::memcpy(pb.value().data(), val.data(), val.size());
    ASSERT_EQ(store_.put(zc_bytes("zkey"), pb).run_sync(), 0);
    udepot::GetBuffer gb;
    ASSERT_EQ(store_.get(zc_bytes("zkey"), &gb).run_sync(), 0);
    EXPECT_EQ(zc_text(gb.value()), val);
}

// The key is copied into the record before the put first suspends (here,
// on the AIO data write), so the caller's key buffer may go away while the
// put is still in flight.
TEST_F(AioStoreTest, ZeroCopyPutDoesNotNeedTheKeyAfterStarting) {
    // An existing key: the put has to read and match it, after suspending.
    ASSERT_EQ(store_.put("keyabc", "old").run_sync(), 0);
    auto pb = store_.alloc_put_buffer(6, 3);
    ASSERT_TRUE(pb.valid());
    std::memcpy(pb.value().data(), "val", 3);
    auto key = std::make_unique<std::string>("keyabc");
    auto op = store_.put(zc_bytes(*key), pb);
    std::memset(key->data(), 'X', key->size());  // clobbered mid-flight
    key.reset();
    ASSERT_EQ(op.run_sync(), 0);
    udepot::GetBuffer gb;
    ASSERT_EQ(store_.get(zc_bytes("keyabc"), &gb).run_sync(), 0);
    EXPECT_EQ(zc_text(gb.value()), "val");
}

// The index goes to its segments and comes back through O_DIRECT, with a
// grain of its own for the footer (512 B) and with the footer sharing its
// grain with the table's last slots (4 KiB, the segment chosen for it).
// Opening clears only the footer: those slots stay on the device.
TEST_F(AioStoreTest, IndexSurvivesACleanReopen) {
    struct Case {
        uint32_t grain;
        uint64_t segment;  // grains
    };
    for (Case c : {Case{512, 64}, Case{4096, 10}}) {
        const auto path = std::filesystem::temp_directory_path() /
                          ("udepot_aio_index_" + std::to_string(c.grain));
        StoreConfig cfg = config_;
        cfg.path = path.c_str();
        cfg.size = 16 * 1024 * 1024 + 4096;
        cfg.grain_size = c.grain;
        cfg.segment_size = c.segment;
        cfg.force_destroy = true;
        {
            uDepot<AioIO> store;
            ASSERT_EQ(store.open(cfg), 0) << c.grain;
            for (int i = 0; i < 3000; ++i)
                ASSERT_EQ(store.put("ix" + std::to_string(i),
                                    "v" + std::to_string(i)).run_sync(), 0);
            store.close();
        }
        cfg.force_destroy = false;
        uDepot<AioIO> store;
        ASSERT_EQ(store.open(cfg), 0) << c.grain;
        EXPECT_TRUE(store.index_restored()) << c.grain;
        for (int i = 0; i < 3000; ++i) {
            uint8_t val[64];
            size_t n = 0;
            ASSERT_EQ(store.get("ix" + std::to_string(i), val, sizeof(val), &n)
                          .run_sync(), 0);
            ASSERT_EQ(std::string(reinterpret_cast<char*>(val), n),
                      "v" + std::to_string(i));
        }
        // The footer's grain as the device holds it: footer cleared, any
        // slots in it as the table has them.
        const auto& table = *store.directory().snapshot().tables[0];
        const auto& r = table.region();
        const size_t from = r.net_bytes() - c.grain;
        if (c.grain == 4096) {
            ASSERT_GT(table.used_bytes(), from) << "no slots share the grain";
        }
        std::string disk(c.grain, '\0');
        {
            std::ifstream f(path, std::ios::binary);
            f.seekg(static_cast<std::streamoff>(r.grain() * c.grain + from));
            ASSERT_TRUE(f.read(disk.data(), c.grain));
        }
        const size_t slots_end = std::min(table.used_bytes(), r.net_bytes());
        for (size_t b = from; b < r.net_bytes(); ++b) {
            const auto got = static_cast<uint8_t>(disk[b - from]);
            if (b >= r.footer_offset()) {
                ASSERT_EQ(got, 0) << "grain " << c.grain << " footer byte " << b;
            } else if (b < slots_end) {
                ASSERT_EQ(got, r.base()[b])
                    << "grain " << c.grain << " slot byte " << b;
            }
        }
        store.close();
        std::filesystem::remove(path);
    }
}

// A thread's first I/O creates its AIO context. When none can be had (the
// system's contexts hold fs.aio-max-nr requests; io_setup fails with
// EAGAIN, injected here), that I/O fails with -ENOMEM: -EAGAIN would mean a
// full queue that the thread's own completions clear, so a caller retrying
// it, and uDepot's own I/O (which does), would spin forever. Before, close()
// from such a thread spun in its index flush until a context was freed.
TEST(AioContext, NoContextFailsTheIoInsteadOfSpinning) {
    const auto path = std::filesystem::temp_directory_path() /
                      "udepot_aio_no_context_test";
    StoreConfig config;
    config.path = path.c_str();
    config.size = kStoreSize;
    config.grain_size = 512;
    config.force_destroy = true;
    struct ResetHook {
        ~ResetHook() { AioIO::setup_test_hook = nullptr; }
    } reset_hook;

    auto store = std::make_unique<uDepot<AioIO>>();
    ASSERT_EQ(store->open(config), 0);  // this thread's context: made here
    ASSERT_EQ(store->put("kept", "value").run_sync(), 0);

    AioIO::setup_test_hook = [] { return -EAGAIN; };
    int put_rc = 0;
    ::testing::internal::CaptureStderr();
    std::thread([&] { put_rc = store->put("k", "v").run_sync(); }).join();
    const std::string log = ::testing::internal::GetCapturedStderr();
    EXPECT_EQ(put_rc, -ENOMEM);
    // Not silent: the thread says why, and what to change.
    EXPECT_NE(log.find("can get no AIO context"), std::string::npos) << log;
    EXPECT_NE(log.find("fs.aio-max-nr"), std::string::npos) << log;

    // close() from a thread that cannot get a context: its index flush
    // fails (the next open scans the log) rather than retrying forever. A
    // close() that spins never returns, so it runs on a thread of its own
    // and the test ends the process if it is still running: joining it, or
    // a std::future's destructor, would hang the test instead of failing it.
    std::atomic<bool> closed{false};
    std::thread([&] {
        store->close();
        closed = true;
    }).detach();
    const auto deadline =
        std::chrono::steady_clock::now() + std::chrono::seconds(20);
    while (!closed && std::chrono::steady_clock::now() < deadline)
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    if (!closed) {
        std::fprintf(stderr, "FAIL: close() spun on the missing context\n");
        std::fflush(nullptr);
        std::_Exit(1);
    }
    AioIO::setup_test_hook = nullptr;

    uDepot<AioIO> again;
    config.force_destroy = false;
    ASSERT_EQ(again.open(config), 0);
    EXPECT_FALSE(again.index_restored());
    uint8_t val[64];
    size_t val_size = 0;
    ASSERT_EQ(again.get("kept", val, sizeof(val), &val_size).run_sync(), 0);
    EXPECT_EQ(std::string_view(reinterpret_cast<char*>(val), val_size),
              "value");
    again.close();
    std::filesystem::remove(path);
}

// Zero copy is counted, not only timed: a copying put or get copies its
// value between the caller's memory and uDepot's record buffer once, a
// zero-copy one (PutBuffer, GetBuffer) never. The zero-copy gate checks
// the same count over O_DIRECT (scripts/perf-zerocopy.sh).
TEST_F(AioStoreTest, ValueCopiesAreCountedPerThread) {
    const std::string v(4000, 'v');
    auto copies = [] { return uDepot<AioIO>::thread_value_copies(); };
    uint64_t c = copies();
    ASSERT_EQ(store_.put("copy", v).run_sync(), 0);
    EXPECT_EQ(copies() - c, 1u);

    c = copies();
    std::vector<uint8_t> out(v.size());
    size_t n = 0;
    ASSERT_EQ(store_.get("copy", out.data(), out.size(), &n).run_sync(), 0);
    EXPECT_EQ(copies() - c, 1u);

    const std::string key = "zero";
    auto pb = store_.alloc_put_buffer(key.size(), v.size());
    ASSERT_TRUE(pb.valid());
    std::memcpy(pb.value().data(), v.data(), v.size());
    auto gb = store_.alloc_get_buffer(key.size(), v.size());
    ASSERT_TRUE(gb.valid());
    c = copies();
    ASSERT_EQ(store_.put(uDepot<AioIO>::as_bytes(key), pb).run_sync(), 0);
    ASSERT_EQ(store_.get(uDepot<AioIO>::as_bytes(key), &gb).run_sync(), 0);
    EXPECT_EQ(copies() - c, 0u);
    EXPECT_EQ(std::string_view(reinterpret_cast<const char*>(gb.value().data()),
                               gb.value().size()),
              v);
}

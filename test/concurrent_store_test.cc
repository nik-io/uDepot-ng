#include "udepot/store.h"
#include "udepot/io/posix.h"

#include <atomic>
#include <cstdint>
#include <cstring>
#include <filesystem>
#include <string>
#include <thread>
#include <vector>

#include <gtest/gtest.h>

using udepot::PosixIO;
using udepot::StoreConfig;
using udepot::UDepot;

static constexpr size_t kStoreSize = 64 * 1024 * 1024;  // 64 MiB

class ConcurrentStoreTest : public ::testing::Test {
protected:
    void SetUp() override {
        path_ = std::filesystem::temp_directory_path() /
                "udepot_concurrent_store_test";
        config_.path = path_.c_str();
        config_.size = kStoreSize;
        config_.grain_size = 512;
        config_.initial_tables = 4;
        config_.index_bits = 14;

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

static std::string make_key(int thread_id, int i) {
    char buf[32];
    snprintf(buf, sizeof(buf), "t%d_k%d", thread_id, i);
    return buf;
}

static std::string make_val(int thread_id, int i) {
    char buf[64];
    snprintf(buf, sizeof(buf), "t%d_v%d_payload", thread_id, i);
    return buf;
}

TEST_F(ConcurrentStoreTest, ConcurrentPutsThenVerify) {
    constexpr int kThreads = 4;
    constexpr int kOpsPerThread = 500;
    std::atomic<int> errors{0};

    std::vector<std::thread> threads;
    for (int t = 0; t < kThreads; ++t) {
        threads.emplace_back([&, t] {
            for (int i = 0; i < kOpsPerThread; ++i) {
                std::string key = make_key(t, i);
                std::string val = make_val(t, i);
                int rc = store_.put(key, val).run_sync();
                if (rc != 0)
                    errors.fetch_add(1, std::memory_order_relaxed);
            }
        });
    }
    for (auto& th : threads) th.join();
    ASSERT_EQ(errors.load(), 0);

    for (int t = 0; t < kThreads; ++t) {
        for (int i = 0; i < kOpsPerThread; ++i) {
            std::string key = make_key(t, i);
            std::string expected = make_val(t, i);
            uint8_t val[128];
            size_t val_size = 0;
            int rc = store_.get(key, val, sizeof(val), &val_size).run_sync();
            ASSERT_EQ(rc, 0) << "missing key: " << key;
            EXPECT_EQ(std::string_view(reinterpret_cast<char*>(val), val_size),
                      expected)
                << "wrong value for key: " << key;
        }
    }
}

TEST_F(ConcurrentStoreTest, ConcurrentReadersAndWriters) {
    constexpr int kWriters = 2;
    constexpr int kReaders = 4;
    constexpr int kOpsPerWriter = 500;
    std::atomic<bool> stop{false};
    std::atomic<int> write_errors{0};
    std::atomic<uint64_t> reads{0};
    std::atomic<uint64_t> read_hits{0};
    std::atomic<int> read_mismatches{0};

    std::vector<std::thread> writers;
    for (int w = 0; w < kWriters; ++w) {
        writers.emplace_back([&, w] {
            for (int i = 0; i < kOpsPerWriter; ++i) {
                std::string key = make_key(w, i);
                std::string val = make_val(w, i);
                int rc = store_.put(key, val).run_sync();
                if (rc != 0)
                    write_errors.fetch_add(1, std::memory_order_relaxed);
            }
        });
    }

    std::vector<std::thread> readers;
    for (int r = 0; r < kReaders; ++r) {
        readers.emplace_back([&, r] {
            uint64_t local_reads = 0;
            uint64_t local_hits = 0;
            int local_mismatches = 0;
            while (!stop.load(std::memory_order_relaxed)) {
                int w = static_cast<int>(local_reads % kWriters);
                int i = static_cast<int>(local_reads % kOpsPerWriter);
                std::string key = make_key(w, i);
                std::string expected = make_val(w, i);
                uint8_t val[128];
                size_t val_size = 0;
                int rc = store_.get(key, val, sizeof(val),
                                    &val_size).run_sync();
                if (rc == 0) {
                    ++local_hits;
                    if (std::string_view(reinterpret_cast<char*>(val),
                                         val_size) != expected) {
                        ++local_mismatches;
                    }
                }
                ++local_reads;
            }
            reads.fetch_add(local_reads, std::memory_order_relaxed);
            read_hits.fetch_add(local_hits, std::memory_order_relaxed);
            read_mismatches.fetch_add(local_mismatches,
                                      std::memory_order_relaxed);
        });
    }

    for (auto& w : writers) w.join();
    stop.store(true, std::memory_order_relaxed);
    for (auto& r : readers) r.join();

    EXPECT_EQ(write_errors.load(), 0);
    EXPECT_GT(reads.load(), 0u);
    EXPECT_EQ(read_mismatches.load(), 0)
        << "a reader saw a value that didn't match the key";
}

TEST_F(ConcurrentStoreTest, ConcurrentPutDeleteGet) {
    constexpr int kThreads = 4;
    constexpr int kOpsPerThread = 300;
    std::atomic<int> errors{0};

    // Phase 1: all threads insert their keys.
    std::vector<std::thread> threads;
    for (int t = 0; t < kThreads; ++t) {
        threads.emplace_back([&, t] {
            for (int i = 0; i < kOpsPerThread; ++i) {
                std::string key = make_key(t, i);
                std::string val = make_val(t, i);
                int rc = store_.put(key, val).run_sync();
                if (rc != 0)
                    errors.fetch_add(1, std::memory_order_relaxed);
            }
        });
    }
    for (auto& th : threads) th.join();
    ASSERT_EQ(errors.load(), 0);
    threads.clear();

    // Phase 2: even threads delete their keys, odd threads read theirs.
    for (int t = 0; t < kThreads; ++t) {
        threads.emplace_back([&, t] {
            if (t % 2 == 0) {
                for (int i = 0; i < kOpsPerThread; ++i) {
                    std::string key = make_key(t, i);
                    int rc = store_.del(key).run_sync();
                    if (rc != 0)
                        errors.fetch_add(1, std::memory_order_relaxed);
                }
            } else {
                for (int i = 0; i < kOpsPerThread; ++i) {
                    std::string key = make_key(t, i);
                    std::string expected = make_val(t, i);
                    uint8_t val[128];
                    size_t val_size = 0;
                    int rc = store_.get(key, val, sizeof(val),
                                        &val_size).run_sync();
                    if (rc != 0) {
                        errors.fetch_add(1, std::memory_order_relaxed);
                        continue;
                    }
                    if (std::string_view(reinterpret_cast<char*>(val),
                                         val_size) != expected)
                        errors.fetch_add(1, std::memory_order_relaxed);
                }
            }
        });
    }
    for (auto& th : threads) th.join();
    EXPECT_EQ(errors.load(), 0);
    threads.clear();

    // Phase 3: verify deleted keys are gone, surviving keys are intact.
    for (int t = 0; t < kThreads; ++t) {
        for (int i = 0; i < kOpsPerThread; ++i) {
            std::string key = make_key(t, i);
            uint8_t val[128];
            size_t val_size = 0;
            int rc = store_.get(key, val, sizeof(val),
                                &val_size).run_sync();
            if (t % 2 == 0) {
                EXPECT_NE(rc, 0) << "deleted key still present: " << key;
            } else {
                ASSERT_EQ(rc, 0) << "surviving key missing: " << key;
                std::string expected = make_val(t, i);
                EXPECT_EQ(
                    std::string_view(reinterpret_cast<char*>(val), val_size),
                    expected)
                    << "wrong value for surviving key: " << key;
            }
        }
    }
}

TEST_F(ConcurrentStoreTest, ConcurrentUpserts) {
    constexpr int kThreads = 4;
    constexpr int kRounds = 100;
    constexpr int kKeysPerThread = 50;
    std::atomic<int> errors{0};

    std::vector<std::thread> threads;
    for (int t = 0; t < kThreads; ++t) {
        threads.emplace_back([&, t] {
            for (int r = 0; r < kRounds; ++r) {
                for (int i = 0; i < kKeysPerThread; ++i) {
                    std::string key = make_key(t, i);
                    std::string val = "r" + std::to_string(r) + "_" +
                                     make_val(t, i);
                    int rc = store_.put(key, val).run_sync();
                    if (rc != 0)
                        errors.fetch_add(1, std::memory_order_relaxed);
                }
            }
        });
    }
    for (auto& th : threads) th.join();
    ASSERT_EQ(errors.load(), 0);

    // Each key must exist with its last-written value.
    for (int t = 0; t < kThreads; ++t) {
        for (int i = 0; i < kKeysPerThread; ++i) {
            std::string key = make_key(t, i);
            std::string expected_prefix = "r" + std::to_string(kRounds - 1) +
                                          "_";
            uint8_t val[128];
            size_t val_size = 0;
            int rc = store_.get(key, val, sizeof(val),
                                &val_size).run_sync();
            ASSERT_EQ(rc, 0) << "key missing after upserts: " << key;
            auto got = std::string_view(reinterpret_cast<char*>(val),
                                        val_size);
            EXPECT_TRUE(got.substr(0, expected_prefix.size()) ==
                        expected_prefix)
                << "key " << key << " has value from wrong round: " << got;
        }
    }
}

TEST_F(ConcurrentStoreTest, ConcurrentExistsWhileWriting) {
    constexpr int kWriters = 2;
    constexpr int kCheckers = 4;
    constexpr int kOpsPerWriter = 500;
    std::atomic<bool> stop{false};
    std::atomic<int> write_errors{0};
    std::atomic<uint64_t> checks{0};
    std::atomic<int> size_mismatches{0};

    std::vector<std::thread> writers;
    for (int w = 0; w < kWriters; ++w) {
        writers.emplace_back([&, w] {
            for (int i = 0; i < kOpsPerWriter; ++i) {
                std::string key = make_key(w, i);
                std::string val = make_val(w, i);
                int rc = store_.put(key, val).run_sync();
                if (rc != 0)
                    write_errors.fetch_add(1, std::memory_order_relaxed);
            }
        });
    }

    std::vector<std::thread> checkers;
    for (int c = 0; c < kCheckers; ++c) {
        checkers.emplace_back([&, c] {
            uint64_t local = 0;
            int local_mismatches = 0;
            while (!stop.load(std::memory_order_relaxed)) {
                int w = static_cast<int>(local % kWriters);
                int i = static_cast<int>(local % kOpsPerWriter);
                std::string key = make_key(w, i);
                std::string expected_val = make_val(w, i);
                size_t val_size = 0;
                int rc = store_.exists(key, &val_size).run_sync();
                if (rc == 0 && val_size != expected_val.size())
                    ++local_mismatches;
                ++local;
            }
            checks.fetch_add(local, std::memory_order_relaxed);
            size_mismatches.fetch_add(local_mismatches,
                                      std::memory_order_relaxed);
        });
    }

    for (auto& w : writers) w.join();
    stop.store(true, std::memory_order_relaxed);
    for (auto& ch : checkers) ch.join();

    EXPECT_EQ(write_errors.load(), 0);
    EXPECT_GT(checks.load(), 0u);
    EXPECT_EQ(size_mismatches.load(), 0)
        << "exists() reported wrong value size for a present key";
}

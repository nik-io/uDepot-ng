// Copyright (c) 2024-2026 Nikolas Ioannou
// SPDX-License-Identifier: BSD-3-Clause

#include "udepot/store.h"
#include "udepot/io/aio.h"

#include <algorithm>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <numeric>
#include <string>
#include <vector>

#include <gtest/gtest.h>

using udepot::AioIO;
using udepot::CoroTask;
using udepot::StoreConfig;
using udepot::UDepot;

static constexpr size_t kStoreSize = 64 * 1024 * 1024;

class AioQueueDepthTest : public ::testing::Test {
protected:
    void SetUp() override {
        path_ = std::filesystem::temp_directory_path() /
                "udepot_aio_qdepth_test";
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
    UDepot<AioIO> store_;
};

static std::string make_key(int i) {
    char buf[32];
    snprintf(buf, sizeof(buf), "qdepth_k_%06d", i);
    return buf;
}

static std::string make_val(int i) {
    char buf[64];
    snprintf(buf, sizeof(buf), "qdepth_v_%06d_pad_data", i);
    return buf;
}

// --- Correctness: batched reads at every queue depth return correct data ---

TEST_F(AioQueueDepthTest, BatchedReadsCorrectAtAllDepths) {
    constexpr int kKeys = 256;

    for (int i = 0; i < kKeys; ++i) {
        std::string key = make_key(i);
        std::string val = make_val(i);
        ASSERT_EQ(store_.put(key, val).run_sync(), 0) << "put i=" << i;
    }

    for (int depth = 1; depth <= 32; depth *= 2) {
        SCOPED_TRACE("queue_depth=" + std::to_string(depth));

        for (int batch_start = 0; batch_start < kKeys;
             batch_start += depth) {
            int batch_end = std::min(batch_start + depth, kKeys);
            int batch_size = batch_end - batch_start;

            struct ReadCtx {
                uint8_t val[128];
                size_t val_size = 0;
            };
            std::vector<ReadCtx> ctxs(batch_size);

            // Keys must outlive the coroutines — get() reads
            // key data after the AIO co_await resumes.
            std::vector<std::string> keys(batch_size);
            for (int j = 0; j < batch_size; ++j)
                keys[j] = make_key(batch_start + j);

            // Start all coroutines eagerly — each submits its AIO.
            std::vector<CoroTask<int>> tasks;
            tasks.reserve(batch_size);
            for (int j = 0; j < batch_size; ++j) {
                tasks.push_back(store_.get(
                    keys[j], ctxs[j].val, sizeof(ctxs[j].val),
                    &ctxs[j].val_size));
            }

            // Collect results — I/Os are already in flight.
            for (int j = 0; j < batch_size; ++j) {
                int rc = tasks[j].run_sync();
                ASSERT_EQ(rc, 0) << "get failed at i=" << (batch_start + j);
                std::string expected = make_val(batch_start + j);
                EXPECT_EQ(
                    std::string_view(reinterpret_cast<char*>(ctxs[j].val),
                                     ctxs[j].val_size),
                    expected)
                    << "wrong value at i=" << (batch_start + j)
                    << " depth=" << depth;
            }
        }
    }
}

// --- Correctness: batched writes at every queue depth ---

TEST_F(AioQueueDepthTest, BatchedWritesCorrectAtAllDepths) {
    constexpr int kKeysPerDepth = 64;

    for (int depth = 1; depth <= 32; depth *= 2) {
        SCOPED_TRACE("queue_depth=" + std::to_string(depth));
        int base = depth * 1000;

        for (int batch_start = 0; batch_start < kKeysPerDepth;
             batch_start += depth) {
            int batch_end = std::min(batch_start + depth, kKeysPerDepth);
            int batch_size = batch_end - batch_start;

            // Start all put coroutines eagerly.
            std::vector<CoroTask<int>> tasks;
            tasks.reserve(batch_size);
            for (int j = 0; j < batch_size; ++j) {
                std::string key = make_key(base + batch_start + j);
                std::string val = make_val(base + batch_start + j);
                tasks.push_back(store_.put(key, val));
            }

            // Collect.
            for (int j = 0; j < batch_size; ++j) {
                int rc = tasks[j].run_sync();
                ASSERT_EQ(rc, 0) << "put failed at i="
                                 << (batch_start + j)
                                 << " depth=" << depth;
            }
        }

        // Verify all written keys.
        for (int i = 0; i < kKeysPerDepth; ++i) {
            std::string key = make_key(base + i);
            std::string expected = make_val(base + i);
            uint8_t val[128];
            size_t val_size = 0;
            int rc = store_.get(key, val, sizeof(val),
                                &val_size).run_sync();
            ASSERT_EQ(rc, 0) << "key missing after batched put: "
                             << key << " depth=" << depth;
            EXPECT_EQ(
                std::string_view(reinterpret_cast<char*>(val), val_size),
                expected);
        }
    }
}

// --- Performance: deeper queues must be faster than serial ---

static double now_secs() {
    auto t = std::chrono::steady_clock::now();
    return std::chrono::duration<double>(t.time_since_epoch()).count();
}

static double median(std::vector<double>& v) {
    std::sort(v.begin(), v.end());
    size_t n = v.size();
    if (n % 2 == 1) return v[n / 2];
    return (v[n / 2 - 1] + v[n / 2]) / 2.0;
}

TEST_F(AioQueueDepthTest, DeeperQueueFasterReads) {
    constexpr int kKeys = 128;
    constexpr int kIterations = 5;

    for (int i = 0; i < kKeys; ++i) {
        std::string key = make_key(i);
        std::string val = make_val(i);
        ASSERT_EQ(store_.put(key, val).run_sync(), 0);
    }

    struct DepthResult {
        int depth;
        double median_secs;
    };
    std::vector<DepthResult> results;

    for (int depth : {1, 2, 4, 8, 16, 32}) {
        std::vector<double> times;

        for (int iter = 0; iter < kIterations; ++iter) {
            struct ReadCtx {
                uint8_t val[128];
                size_t val_size = 0;
            };

            double t0 = now_secs();

            for (int batch_start = 0; batch_start < kKeys;
                 batch_start += depth) {
                int batch_end = std::min(batch_start + depth, kKeys);
                int batch_size = batch_end - batch_start;

                std::vector<ReadCtx> ctxs(batch_size);
                std::vector<std::string> keys(batch_size);
                for (int j = 0; j < batch_size; ++j)
                    keys[j] = make_key(batch_start + j);

                std::vector<CoroTask<int>> tasks;
                tasks.reserve(batch_size);

                for (int j = 0; j < batch_size; ++j) {
                    tasks.push_back(store_.get(
                        keys[j], ctxs[j].val, sizeof(ctxs[j].val),
                        &ctxs[j].val_size));
                }
                for (int j = 0; j < batch_size; ++j) {
                    int rc = tasks[j].run_sync();
                    ASSERT_EQ(rc, 0);
                }
            }

            times.push_back(now_secs() - t0);
        }

        double med = median(times);
        results.push_back({depth, med});
        fprintf(stderr, "  GET  depth=%-2d  median=%.6fs  (%.1f Kops/s)\n",
                depth, med, kKeys / (med * 1000));
    }

    // Assert: every depth > 1 is faster than depth == 1.
    double serial = results[0].median_secs;
    for (size_t i = 1; i < results.size(); ++i) {
        EXPECT_LT(results[i].median_secs, serial)
            << "queue depth " << results[i].depth
            << " (" << results[i].median_secs << "s) should be faster"
            << " than serial (" << serial << "s)";
    }
}

TEST_F(AioQueueDepthTest, DeeperQueueFasterWrites) {
    constexpr int kKeys = 128;
    constexpr int kIterations = 5;

    struct DepthResult {
        int depth;
        double median_secs;
    };
    std::vector<DepthResult> results;

    for (int depth : {1, 2, 4, 8, 16, 32}) {
        std::vector<double> times;

        for (int iter = 0; iter < kIterations; ++iter) {
            // Each iteration writes a distinct set of keys so upsert
            // doesn't confound the measurement with lookup-before-write
            // on the second iteration.
            int base = depth * 10000 + iter * kKeys;

            double t0 = now_secs();

            for (int batch_start = 0; batch_start < kKeys;
                 batch_start += depth) {
                int batch_end = std::min(batch_start + depth, kKeys);
                int batch_size = batch_end - batch_start;

                std::vector<CoroTask<int>> tasks;
                tasks.reserve(batch_size);

                for (int j = 0; j < batch_size; ++j) {
                    std::string key = make_key(base + batch_start + j);
                    std::string val = make_val(base + batch_start + j);
                    tasks.push_back(store_.put(key, val));
                }
                for (int j = 0; j < batch_size; ++j) {
                    int rc = tasks[j].run_sync();
                    ASSERT_EQ(rc, 0);
                }
            }

            times.push_back(now_secs() - t0);
        }

        double med = median(times);
        results.push_back({depth, med});
        fprintf(stderr, "  PUT  depth=%-2d  median=%.6fs  (%.1f Kops/s)\n",
                depth, med, kKeys / (med * 1000));
    }

    double serial = results[0].median_secs;
    for (size_t i = 1; i < results.size(); ++i) {
        EXPECT_LT(results[i].median_secs, serial)
            << "queue depth " << results[i].depth
            << " (" << results[i].median_secs << "s) should be faster"
            << " than serial (" << serial << "s)";
    }
}

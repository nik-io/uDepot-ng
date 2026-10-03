// Copyright (c) 2024-2026 Nikolas Ioannou
// SPDX-License-Identifier: BSD-3-Clause

// Queue-depth tests, shared by every asynchronous backend. They drive the
// store the way a caller wanting queue depth does:
//
//   1. preallocate zero-copy buffers for the queue depth it chose,
//   2. issue that many operations,
//   3. (optionally do other work,)
//   4. wait for their completions,
//   5. reuse the buffers for the next round, and free them at the end.
//
// and check that batched gets and puts return the right data at every
// depth, that deeper queues are faster than serial ones, that a get reads
// into the caller's buffer rather than a new one, and that a depth beyond
// what the backend's own queue holds (the AIO context, the io_uring ring,
// the SPDK request pool) is still served rather than failed.
//
// Each backend's test file defines a traits type and instantiates the suite:
//
//   struct Traits {
//       using IO = ...;
//       // Completions are resumed on a backend thread (AIO, io_uring),
//       // not by the submitting thread's poll (SPDK).
//       static constexpr bool kPollerThread = ...;
//       // Submissions past a full queue are refused and must wait.
//       static constexpr bool kFullQueueWaits = ...;
//       static uint64_t waited(IO& io);   // I/Os that had to wait
//       static void suite_setup();        // e.g. SPDK's global_init
//       static void suite_teardown();
//       static void configure(udepot::StoreConfig& config);  // path, size
//       static void cleanup(const udepot::StoreConfig& config);
//   };
//   INSTANTIATE_TYPED_TEST_SUITE_P(Name, QueueDepthTest, Traits);

#pragma once

#include "udepot/store.h"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <iterator>
#include <optional>
#include <span>
#include <string>
#include <thread>
#include <vector>

#include <gtest/gtest.h>

namespace qd {

inline std::string make_key(int i) {
    char buf[32];
    snprintf(buf, sizeof(buf), "qdepth_k_%06d", i);
    return buf;
}

inline std::string make_val(int i) {
    char buf[64];
    snprintf(buf, sizeof(buf), "qdepth_v_%06d_pad_data", i);
    return buf;
}

// Every key and value the suite uses has these sizes, so one set of
// buffers serves every operation.
inline constexpr size_t kKeySize = 15;
inline constexpr size_t kValSize = 24;

inline std::span<const uint8_t> bytes(const std::string& s) {
    return {reinterpret_cast<const uint8_t*>(s.data()), s.size()};
}

inline std::string_view text(std::span<const uint8_t> s) {
    return {reinterpret_cast<const char*>(s.data()), s.size()};
}

inline double now_secs() {
    auto t = std::chrono::steady_clock::now();
    return std::chrono::duration<double>(t.time_since_epoch()).count();
}

inline double median(std::vector<double> v) {
    std::sort(v.begin(), v.end());
    size_t n = v.size();
    if (n % 2 == 1) return v[n / 2];
    return (v[n / 2 - 1] + v[n / 2]) / 2.0;
}

struct ReadCtx {
    uint8_t val[128];
    size_t val_size = 0;
};

// Gets `key`, then holds the thread that resumed it, the backend's poller,
// until *state is 2, so nothing is reaped meanwhile. Sets *state to 1 once
// holding. Then gets the key again: a submission from the poller thread
// itself, while the queue is full.
template <typename Store>
udepot::CoroTask<int> hold_completions(Store& store, std::string key,
                                       std::atomic<int>* state) {
    ReadCtx ctx;
    int rc = co_await store.get(key, ctx.val, sizeof(ctx.val), &ctx.val_size);
    state->store(1, std::memory_order_release);
    while (state->load(std::memory_order_acquire) != 2)
        std::this_thread::yield();
    if (rc == 0)
        rc = co_await store.get(key, ctx.val, sizeof(ctx.val), &ctx.val_size);
    co_return rc;
}

}  // namespace qd

template <typename Traits>
class QueueDepthTest : public ::testing::Test {
protected:
    using Store = udepot::UDepot<typename Traits::IO>;

    static void SetUpTestSuite() { Traits::suite_setup(); }
    static void TearDownTestSuite() { Traits::suite_teardown(); }

    void SetUp() override {
        config_.grain_size = 512;
        config_.initial_tables = 4;
        config_.index_bits = 14;
        config_.force_destroy = true;
        Traits::configure(config_);
        ASSERT_EQ(store_.open(config_), 0);
    }

    void TearDown() override {
        free_buffers();
        store_.close();
        Traits::cleanup(config_);
    }

    // A fresh store whose backend is sized for queue_depth I/Os.
    void reopen(unsigned queue_depth) {
        free_buffers();
        store_.close();
        config_.queue_depth = queue_depth;
        config_.force_destroy = true;
        ASSERT_EQ(store_.open(config_), 0);
    }

    // Step 1: buffers for `depth` operations in flight, allocated once and
    // reused by every later batch.
    void ensure_buffers(int depth) {
        while (put_bufs_.size() < static_cast<size_t>(depth)) {
            put_bufs_.push_back(store_.alloc_put_buffer(qd::kKeySize,
                                                        qd::kValSize));
            ASSERT_TRUE(put_bufs_.back().valid());
            get_bufs_.push_back(store_.alloc_get_buffer(qd::kKeySize,
                                                        qd::kValSize));
            ASSERT_TRUE(get_bufs_.back().valid());
            get_at_.push_back(nullptr);
        }
    }

    void free_buffers() {
        put_bufs_.clear();
        get_bufs_.clear();
        get_at_.clear();
    }

    void put_keys(int first, int n) {
        for (int i = first; i < first + n; ++i)
            ASSERT_EQ(store_.put(qd::make_key(i), qd::make_val(i)).run_sync(),
                      0)
                << "put i=" << i;
    }

    // Step 2: issues puts of keys [first, first + n) from the preallocated
    // buffers; n must not exceed them. The keys must outlive the tasks.
    void issue_puts(int first, int n, std::vector<std::string>& keys,
                    std::vector<udepot::CoroTask<int>>& tasks) {
        keys.resize(n);
        tasks.clear();
        tasks.reserve(n);
        for (int j = 0; j < n; ++j) {
            keys[j] = qd::make_key(first + j);
            const std::string val = qd::make_val(first + j);
            ASSERT_EQ(keys[j].size(), qd::kKeySize);
            ASSERT_EQ(val.size(), qd::kValSize);
            std::memcpy(put_bufs_[j].value().data(), val.data(), val.size());
            tasks.push_back(store_.put(qd::bytes(keys[j]), put_bufs_[j]));
        }
    }

    // Step 2, for gets into the preallocated buffers.
    void issue_gets(int first, int n, std::vector<std::string>& keys,
                    std::vector<udepot::CoroTask<int>>& tasks) {
        keys.resize(n);
        tasks.clear();
        tasks.reserve(n);
        for (int j = 0; j < n; ++j) {
            keys[j] = qd::make_key(first + j);
            tasks.push_back(store_.get(qd::bytes(keys[j]), &get_bufs_[j]));
        }
    }

    // Step 4: waits for every task, then checks them. Every task is waited
    // for before anything is asserted: returning early would destroy tasks
    // whose I/O is still in flight.
    void harvest(std::vector<udepot::CoroTask<int>>& tasks, int first,
                 const char* what, int depth) {
        std::vector<int> rcs;
        rcs.reserve(tasks.size());
        for (auto& t : tasks) rcs.push_back(t.run_sync());
        for (size_t j = 0; j < rcs.size(); ++j)
            EXPECT_EQ(rcs[j], 0) << what << " failed at i=" << (first + j)
                                 << " depth=" << depth;
    }

    // The values the last issue_gets read, and that each went into the
    // caller's own buffer: the same place as every earlier get into it.
    void check_gets(int first, int n, int depth) {
        for (int j = 0; j < n; ++j) {
            EXPECT_EQ(qd::text(get_bufs_[j].value()), qd::make_val(first + j))
                << "wrong value at i=" << (first + j) << " depth=" << depth;
            const uint8_t* at = get_bufs_[j].value().data();
            if (!get_at_[j]) get_at_[j] = at;
            EXPECT_EQ(at, get_at_[j])
                << "get did not reuse the caller's buffer " << j;
        }
    }

    // Gets keys [first, first + n) in batches of `depth`: every operation
    // of a batch is issued before any is waited on.
    void batched_gets(int first, int n, int depth, bool check) {
        ensure_buffers(depth);
        std::vector<std::string> keys;
        std::vector<udepot::CoroTask<int>> tasks;
        for (int start = first; start < first + n; start += depth) {
            const int batch = std::min(depth, first + n - start);
            issue_gets(start, batch, keys, tasks);
            harvest(tasks, start, "get", depth);
            if (check) check_gets(start, batch, depth);
        }
    }

    // Puts keys [first, first + n) in batches of `depth`.
    void batched_puts(int first, int n, int depth) {
        ensure_buffers(depth);
        std::vector<std::string> keys;
        std::vector<udepot::CoroTask<int>> tasks;
        for (int start = first; start < first + n; start += depth) {
            const int batch = std::min(depth, first + n - start);
            issue_puts(start, batch, keys, tasks);
            harvest(tasks, start, "put", depth);
        }
    }

    void check_keys(int first, int n) {
        for (int i = first; i < first + n; ++i) {
            qd::ReadCtx ctx;
            ASSERT_EQ(store_.get(qd::make_key(i), ctx.val, sizeof(ctx.val),
                                 &ctx.val_size).run_sync(), 0)
                << "key missing: " << qd::make_key(i);
            EXPECT_EQ(std::string_view(reinterpret_cast<char*>(ctx.val),
                                       ctx.val_size),
                      qd::make_val(i));
        }
    }

    // Issues `depth` puts (or gets) of keys from `first` while nothing is
    // being reaped, so far more are outstanding than the backend's queue
    // holds, then lets completions run and checks every one. On a backend
    // whose completions are resumed by a poller thread, a coroutine holds
    // that thread; on SPDK, nothing polls until the tasks are waited on.
    void issue_beyond_the_queue(int first, int depth, bool puts) {
        ensure_buffers(depth);
        std::atomic<int> state{0};
        std::optional<udepot::CoroTask<int>> holder;
        if constexpr (Traits::kPollerThread) {
            holder.emplace(qd::hold_completions(store_, qd::make_key(0),
                                                &state));
            const double deadline = qd::now_secs() + 30;
            while (state.load(std::memory_order_acquire) != 1 &&
                   qd::now_secs() < deadline)
                std::this_thread::yield();
            ASSERT_EQ(state.load(), 1) << "the poller was never held";
        }

        const uint64_t waited0 = Traits::waited(store_.io());
        std::vector<std::string> keys;
        std::vector<udepot::CoroTask<int>> tasks;
        if (puts)
            issue_puts(first, depth, keys, tasks);
        else
            issue_gets(first, depth, keys, tasks);
        const uint64_t waited = Traits::waited(store_.io()) - waited0;

        state.store(2, std::memory_order_release);  // let the poller go
        if (holder) {
            EXPECT_EQ(holder->run_sync(), 0);
        }
        harvest(tasks, first, puts ? "put" : "get", depth);
        if (!puts) check_gets(first, depth, depth);

        fprintf(stderr, "  %s: %d outstanding, %llu waited for room\n",
                puts ? "PUT" : "GET", depth,
                static_cast<unsigned long long>(waited));
        if constexpr (Traits::kFullQueueWaits) {
            // Otherwise the queue never filled and this test showed
            // nothing about a full one.
            EXPECT_GT(waited, 0u)
                << "no I/O found the queue full; raise the depth";
        }
    }

    udepot::StoreConfig config_;
    Store store_;
    std::vector<udepot::PutBuffer> put_bufs_;
    std::vector<udepot::GetBuffer> get_bufs_;
    std::vector<const uint8_t*> get_at_;
};

TYPED_TEST_SUITE_P(QueueDepthTest);

TYPED_TEST_P(QueueDepthTest, BatchedReadsCorrectAtAllDepths) {
    constexpr int kKeys = 64;
    this->put_keys(0, kKeys);
    for (int depth = 1; depth <= 64; depth *= 2) {
        SCOPED_TRACE("queue_depth=" + std::to_string(depth));
        this->batched_gets(0, kKeys, depth, /*check=*/true);
    }
}

TYPED_TEST_P(QueueDepthTest, BatchedWritesCorrectAtAllDepths) {
    constexpr int kKeysPerDepth = 64;
    for (int depth = 1; depth <= 64; depth *= 2) {
        SCOPED_TRACE("queue_depth=" + std::to_string(depth));
        const int base = depth * 1000;
        this->batched_puts(base, kKeysPerDepth, depth);
        this->check_keys(base, kKeysPerDepth);
    }
}

// The backend is sized for a queue depth of 4, and the caller has 4096
// operations outstanding at once with nothing reaped meanwhile: the AIO
// context and the SPDK request pool fill up, and io_uring's completion
// queue overflows. Each operation must still complete, with its data, not
// fail. 4096 is above the AIO context the kernel allocates for 4 on hosts
// of up to ~500 CPUs (it allocates at least 8 per possible CPU).
TYPED_TEST_P(QueueDepthTest, DepthBeyondTheBackendQueue) {
    constexpr unsigned kQueueDepth = 4;
    constexpr int kDepth = 4096;
    this->reopen(kQueueDepth);
    this->put_keys(0, 1);  // the key hold_completions gets
    // A fresh thread: on SPDK its queue pair is created at the new size.
    std::thread t([&] {
        this->issue_beyond_the_queue(1, kDepth, /*puts=*/true);
        this->issue_beyond_the_queue(1, kDepth, /*puts=*/false);
        // And once more into the same buffers.
        this->issue_beyond_the_queue(1, kDepth, /*puts=*/false);
    });
    t.join();
    this->check_keys(1, kDepth);
}

// Depths are interleaved within each iteration, so machine drift over the
// run hits every depth alike. Depth 2 is left out of the assertion: on fast
// media the batching overhead can exceed the parallelism gain.
template <typename Fixture, typename Op>
void expect_deeper_is_faster(const char* what, Op op) {
    constexpr int kIterations = 9;
    constexpr int kDepths[] = {1, 2, 4, 8, 16, 32, 64};
    constexpr size_t kNumDepths = std::size(kDepths);
    std::vector<std::vector<double>> times(kNumDepths);
    for (int iter = 0; iter < kIterations; ++iter) {
        for (size_t d = 0; d < kNumDepths; ++d) {
            const double t0 = qd::now_secs();
            op(iter, kDepths[d]);
            times[d].push_back(qd::now_secs() - t0);
        }
    }
    std::vector<double> med(kNumDepths);
    for (size_t d = 0; d < kNumDepths; ++d) {
        med[d] = qd::median(times[d]);
        fprintf(stderr, "  %s depth=%-2d  median=%.6fs\n", what, kDepths[d],
                med[d]);
    }
    for (size_t d = 1; d < kNumDepths; ++d) {
        if (kDepths[d] < 4) continue;
        EXPECT_LT(med[d], med[0])
            << what << " at queue depth " << kDepths[d] << " (" << med[d]
            << "s) should be faster than serial (" << med[0] << "s)";
    }
}

TYPED_TEST_P(QueueDepthTest, DeeperQueueFasterReads) {
    constexpr int kKeys = 128;
    this->put_keys(0, kKeys);
    this->ensure_buffers(64);  // outside the timed runs
    expect_deeper_is_faster<TestFixture>("GET", [&](int, int depth) {
        this->batched_gets(0, kKeys, depth, /*check=*/false);
    });
}

TYPED_TEST_P(QueueDepthTest, DeeperQueueFasterWrites) {
    constexpr int kKeys = 128;
    this->ensure_buffers(64);
    // Distinct keys per run, so no run measures an overwrite.
    expect_deeper_is_faster<TestFixture>("PUT", [&](int iter, int depth) {
        this->batched_puts(100000 + (iter * 100 + depth) * kKeys, kKeys,
                           depth);
    });
}

REGISTER_TYPED_TEST_SUITE_P(QueueDepthTest, BatchedReadsCorrectAtAllDepths,
                            BatchedWritesCorrectAtAllDepths,
                            DepthBeyondTheBackendQueue,
                            DeeperQueueFasterReads, DeeperQueueFasterWrites);

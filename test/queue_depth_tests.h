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
// into the caller's buffer rather than a new one, and that operations
// beyond what the backend's queue holds (the AIO context, the SPDK request
// pool) fail with -EAGAIN, cleanly enough to retry, rather than with
// another error or a wrong result.
//
// Each backend's test file defines a traits type and instantiates the suite:
//
//   struct Traits {
//       using IO = ...;
//       // Completions are resumed on a backend thread (AIO, io_uring),
//       // not by the submitting thread's poll (SPDK).
//       static constexpr bool kPollerThread = ...;
//       // The backend refuses submissions past a full queue (AIO, SPDK);
//       // io_uring's kernel takes them.
//       static constexpr bool kQueueFills = ...;
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
// holding, or to 3 if the get completed without suspending (nothing to
// hold). Then gets the key again: a submission from the poller thread
// itself, while the queue is full.
template <typename Store>
udepot::CoroTask<int> hold_completions(Store& store, std::string key,
                                       std::atomic<int>* state) {
    const auto caller = std::this_thread::get_id();
    ReadCtx ctx;
    int rc = co_await store.get(key, ctx.val, sizeof(ctx.val), &ctx.val_size);
    if (std::this_thread::get_id() == caller) {
        state->store(3, std::memory_order_release);
        co_return rc;
    }
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

    // Step 2: issues puts of keys first + idx[k] from the preallocated
    // buffers (slot k); idx must not outnumber them. The keys must outlive
    // the tasks.
    void issue_puts(int first, const std::vector<int>& idx,
                    std::vector<std::string>& keys,
                    std::vector<udepot::CoroTask<int>>& tasks) {
        keys.resize(idx.size());
        tasks.clear();
        tasks.reserve(idx.size());
        for (size_t k = 0; k < idx.size(); ++k) {
            keys[k] = qd::make_key(first + idx[k]);
            const std::string val = qd::make_val(first + idx[k]);
            ASSERT_EQ(keys[k].size(), qd::kKeySize);
            ASSERT_EQ(val.size(), qd::kValSize);
            std::memcpy(put_bufs_[k].value().data(), val.data(), val.size());
            tasks.push_back(store_.put(qd::bytes(keys[k]), put_bufs_[k]));
        }
    }

    // Step 2, for gets into the preallocated buffers.
    void issue_gets(int first, const std::vector<int>& idx,
                    std::vector<std::string>& keys,
                    std::vector<udepot::CoroTask<int>>& tasks) {
        keys.resize(idx.size());
        tasks.clear();
        tasks.reserve(idx.size());
        for (size_t k = 0; k < idx.size(); ++k) {
            keys[k] = qd::make_key(first + idx[k]);
            tasks.push_back(store_.get(qd::bytes(keys[k]), &get_bufs_[k]));
        }
    }

    static std::vector<int> iota(int n) {
        std::vector<int> v(n);
        for (int i = 0; i < n; ++i) v[i] = i;
        return v;
    }

    // Step 4: waits for every task. Every one is waited for before anything
    // is checked: returning early would destroy tasks whose I/O is still in
    // flight.
    static std::vector<int> wait_all(std::vector<udepot::CoroTask<int>>& tasks) {
        std::vector<int> rcs;
        rcs.reserve(tasks.size());
        for (auto& t : tasks) rcs.push_back(t.run_sync());
        return rcs;
    }

    void expect_all_ok(const std::vector<int>& rcs, int first,
                       const char* what, int depth) {
        for (size_t j = 0; j < rcs.size(); ++j)
            EXPECT_EQ(rcs[j], 0) << what << " failed at i=" << (first + j)
                                 << " depth=" << depth;
    }

    // The value a get into slot k read, and that it went into the caller's
    // own buffer: the same place as every earlier get into that slot.
    void check_get(size_t k, int i) {
        EXPECT_EQ(qd::text(get_bufs_[k].value()), qd::make_val(i))
            << "wrong value at i=" << i;
        const uint8_t* at = get_bufs_[k].value().data();
        if (!get_at_[k]) get_at_[k] = at;
        EXPECT_EQ(at, get_at_[k]) << "get did not reuse the caller's buffer "
                                  << k;
    }

    // Gets keys [first, first + n) in batches of `depth`: every operation
    // of a batch is issued before any is waited on.
    void batched_gets(int first, int n, int depth, bool check) {
        ensure_buffers(depth);
        std::vector<std::string> keys;
        std::vector<udepot::CoroTask<int>> tasks;
        for (int start = first; start < first + n; start += depth) {
            const int batch = std::min(depth, first + n - start);
            issue_gets(start, iota(batch), keys, tasks);
            const std::vector<int> rcs = wait_all(tasks);
            expect_all_ok(rcs, start, "get", depth);
            if (!check) continue;
            for (int j = 0; j < batch; ++j)
                if (rcs[j] == 0) check_get(j, start + j);
        }
    }

    // Puts keys [first, first + n) in batches of `depth`.
    void batched_puts(int first, int n, int depth) {
        ensure_buffers(depth);
        std::vector<std::string> keys;
        std::vector<udepot::CoroTask<int>> tasks;
        for (int start = first; start < first + n; start += depth) {
            const int batch = std::min(depth, first + n - start);
            issue_puts(start, iota(batch), keys, tasks);
            expect_all_ok(wait_all(tasks), start, "put", depth);
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

    // Issues puts (or gets) of keys first + idx[k], all at once, while
    // nothing is being reaped, so far more are outstanding than the
    // backend's queue holds; then lets completions run. On a backend whose
    // completions are resumed by a poller thread, a coroutine holds that
    // thread; on SPDK, nothing polls until the tasks are waited on. Each
    // operation must succeed, with its data, or fail with -EAGAIN: those
    // are returned, for the caller to retry.
    std::vector<int> issue_beyond_the_queue(int first,
                                            const std::vector<int>& idx,
                                            bool puts) {
        ensure_buffers(static_cast<int>(idx.size()));
        std::atomic<int> state{0};
        std::optional<udepot::CoroTask<int>> holder;
        if constexpr (Traits::kPollerThread) {
            holder.emplace(qd::hold_completions(store_, qd::make_key(0),
                                                &state));
            const double deadline = qd::now_secs() + 30;
            while (state.load(std::memory_order_acquire) == 0 &&
                   qd::now_secs() < deadline)
                std::this_thread::yield();
            if (state.load() != 1) {
                ADD_FAILURE() << "the poller was never held";
                state.store(2);
                if (state.load() == 2 && holder) (void)holder->run_sync();
                return {};
            }
        }

        std::vector<std::string> keys;
        std::vector<udepot::CoroTask<int>> tasks;
        if (puts)
            issue_puts(first, idx, keys, tasks);
        else
            issue_gets(first, idx, keys, tasks);

        state.store(2, std::memory_order_release);  // let the poller go
        if (holder) {
            // Its second get is submitted from the poller thread itself,
            // into a full queue: it may be refused, never stuck.
            const int rc = holder->run_sync();
            EXPECT_TRUE(rc == 0 || rc == -EAGAIN) << "rc=" << rc;
        }
        const std::vector<int> rcs = wait_all(tasks);

        std::vector<int> refused;
        for (size_t k = 0; k < idx.size(); ++k) {
            const int i = first + idx[k];
            if (rcs[k] == -EAGAIN) {
                refused.push_back(idx[k]);
                continue;
            }
            EXPECT_EQ(rcs[k], 0) << (puts ? "put" : "get") << " i=" << i;
            if (!puts && rcs[k] == 0) check_get(k, i);
        }
        return refused;
    }

    // Issues all n at once, beyond the queue, then, as a caller would,
    // retries what was refused at the depth it sized the store for
    // (`retry_depth` at a time) until nothing is.
    void beyond_the_queue_until_done(int first, int n, bool puts,
                                     int retry_depth) {
        std::vector<int> todo = issue_beyond_the_queue(first, iota(n), puts);
        fprintf(stderr, "  %s: %d outstanding, %zu refused (-EAGAIN)\n",
                puts ? "PUT" : "GET", n, todo.size());
        if constexpr (Traits::kQueueFills) {
            // Otherwise the queue never filled and this showed nothing
            // about a full one.
            EXPECT_GT(todo.size(), 0u)
                << "no operation found the queue full; raise the depth";
        }
        std::vector<std::string> keys;
        std::vector<udepot::CoroTask<int>> tasks;
        while (!todo.empty()) {
            std::vector<int> refused;
            for (size_t at = 0; at < todo.size(); at += retry_depth) {
                const size_t end =
                    std::min(todo.size(), at + static_cast<size_t>(retry_depth));
                const std::vector<int> idx(todo.begin() + at,
                                           todo.begin() + end);
                if (puts)
                    issue_puts(first, idx, keys, tasks);
                else
                    issue_gets(first, idx, keys, tasks);
                const std::vector<int> rcs = wait_all(tasks);
                for (size_t k = 0; k < idx.size(); ++k) {
                    if (rcs[k] == -EAGAIN) {
                        refused.push_back(idx[k]);
                        continue;
                    }
                    EXPECT_EQ(rcs[k], 0) << "retry i=" << (first + idx[k]);
                    if (!puts && rcs[k] == 0) check_get(k, first + idx[k]);
                }
            }
            ASSERT_LT(refused.size(), todo.size()) << "retries made no progress";
            todo = std::move(refused);
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
// context and the SPDK request pool fill up (io_uring's kernel takes them
// all, overflowing the completion queue). What the backend cannot take
// fails with -EAGAIN and nothing else: no -EIO, no -ENOENT for a key that
// is there, no half-done put. Retried at the queue depth the store was
// sized for, everything completes, with its data.
// 4096 is above the AIO context the kernel allocates for 4 on hosts of up
// to ~500 CPUs (it allocates at least 8 per possible CPU).
TYPED_TEST_P(QueueDepthTest, DepthBeyondTheBackendQueue) {
    constexpr unsigned kQueueDepth = 4;
    constexpr int kDepth = 4096;
    this->reopen(kQueueDepth);
    this->put_keys(0, 1);  // the key hold_completions gets
    // A fresh thread: on SPDK its queue pair is created at the new size.
    std::thread t([&] {
        this->beyond_the_queue_until_done(1, kDepth, /*puts=*/true,
                                          kQueueDepth);
        this->beyond_the_queue_until_done(1, kDepth, /*puts=*/false,
                                          kQueueDepth);
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

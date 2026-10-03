// Copyright (c) 2024-2026 Nikolas Ioannou
// SPDX-License-Identifier: BSD-3-Clause

#include "udepot/io/uring.h"

#include <filesystem>
#include <string>

#include <unistd.h>

#include "queue_depth_tests.h"

namespace {

struct UringTraits {
    using IO = udepot::UringIO;
    static constexpr bool kPollerThread = true;
    // The kernel takes submissions past the ring's size, keeping the
    // extra completions on its overflow list.
    static constexpr bool kQueueFills = false;
    // A depth past the kernel's limits fails open().
    static constexpr bool kQueueSizeLimited = true;
    static void suite_setup() {}
    static void suite_teardown() {}
    static std::string& path() {
        static std::string p =
            (std::filesystem::temp_directory_path() /
             ("udepot_uring_qdepth_test_" + std::to_string(getpid())))
                .string();
        return p;
    }
    static void configure(udepot::StoreConfig& config) {
        config.path = path().c_str();
        config.size = 16 * 1024 * 1024;
        qd::make_device_file(path(), config.size);
    }
    static void cleanup(const udepot::StoreConfig&) {
        std::filesystem::remove(path());
    }
};

}  // namespace

INSTANTIATE_TYPED_TEST_SUITE_P(Uring, QueueDepthTest, UringTraits);

// The kernel refusing submissions (EBUSY/EAGAIN from io_uring_submit),
// which this kernel never does, so it is injected. The refused SQEs stay in
// the submission queue for the poller to push once it has reaped; nothing
// spins (the submitter may be the poller itself), and once the queue is full
// of them an operation fails with -EAGAIN. Before, the submitter spun until
// the kernel took them: with the poller holding no completion to reap, a
// hang.
TEST(UringSubmit, RefusedSubmissionsAreRetriedByThePoller) {
    static std::atomic<bool> refuse{false};
    udepot::UringIO::submit_test_hook = [] {
        return refuse.load() ? -EBUSY : 0;
    };
    struct ResetHook {
        ~ResetHook() { udepot::UringIO::submit_test_hook = nullptr; }
    } reset_hook;

    udepot::StoreConfig config;
    config.grain_size = 512;
    config.initial_tables = 4;
    config.index_bits = 14;
    config.force_destroy = true;
    config.queue_depth = 4;  // a 4-entry submission queue
    UringTraits::configure(config);
    udepot::UDepot<udepot::UringIO> store;
    ASSERT_EQ(store.open(config), 0);

    constexpr int kKeys = 64;
    for (int i = 0; i < kKeys; ++i)
        ASSERT_EQ(store.put(qd::make_key(i), qd::make_val(i)).run_sync(), 0);

    refuse = true;
    std::vector<qd::ReadCtx> ctxs(kKeys);
    std::vector<std::string> keys(kKeys);
    std::vector<udepot::CoroTask<int>> tasks;
    for (int i = 0; i < kKeys; ++i) {
        keys[i] = qd::make_key(i);
        tasks.push_back(store.get(keys[i], ctxs[i].val, sizeof(ctxs[i].val),
                                  &ctxs[i].val_size));
    }
    // Every get was issued without blocking: none spun on the refusal.
    refuse = false;

    std::vector<int> refused;
    for (int i = 0; i < kKeys; ++i) {
        const int rc = tasks[i].run_sync();
        if (rc == -EAGAIN) {
            refused.push_back(i);
            continue;
        }
        ASSERT_EQ(rc, 0) << "i=" << i;
        EXPECT_EQ(std::string_view(reinterpret_cast<char*>(ctxs[i].val),
                                   ctxs[i].val_size),
                  qd::make_val(i));
    }
    // The submission queue's 4 entries were taken by refused SQEs, which the
    // poller then pushed; the rest found it full.
    EXPECT_GT(refused.size(), 0u);
    EXPECT_LT(refused.size(), static_cast<size_t>(kKeys));
    for (int i : refused) {
        qd::ReadCtx ctx;
        ASSERT_EQ(store.get(qd::make_key(i), ctx.val, sizeof(ctx.val),
                            &ctx.val_size).run_sync(), 0);
        EXPECT_EQ(std::string_view(reinterpret_cast<char*>(ctx.val),
                                   ctx.val_size),
                  qd::make_val(i));
    }
    store.close();
    UringTraits::cleanup(config);
}

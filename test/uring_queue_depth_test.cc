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
// the submission queue for the thread's next poll to push once it has
// reaped; nothing spins (the submitter may be that poll itself, resuming a
// coroutine), and once the queue is full of them an operation fails with
// -EAGAIN. Before, the submitter spun until the kernel took them: with
// nothing reaping meanwhile, a hang.
TEST(UringSubmit, RefusedSubmissionsAreRetriedByThePoll) {
    static std::atomic<bool> refuse{false};
    static std::atomic<int> refusals{0};
    refusals = 0;
    udepot::UringIO::submit_test_hook = [] {
        if (!refuse.load()) return 0;
        ++refusals;
        return -EBUSY;
    };
    struct ResetHook {
        ~ResetHook() { udepot::UringIO::submit_test_hook = nullptr; }
    } reset_hook;

    udepot::StoreConfig config;
    config.grain_size = 512;
    config.force_destroy = true;
    config.queue_depth = 4;  // a 4-entry submission queue
    UringTraits::configure(config);
    udepot::uDepot<udepot::UringIO> store;
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
    // The refusal was met: the 4 queued SQEs filled the ring, so the next
    // get pushed them and the kernel (the hook) refused.
    EXPECT_GT(refusals.load(), 0);

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
    // poll then pushed; the rest found it full.
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

// A ring smaller than a submission batch (kSubmitBatch, 128) fills with
// SQEs waiting for the batch before it is ever pushed. The next I/O pushes
// them rather than being refused: nothing refused it but a full ring of
// SQEs the kernel never saw. (uDepot's ring, 1024 entries, is never smaller
// than its batch.) Before, at depth 4 the fifth of these gets failed with
// -EAGAIN.
TEST(UringSubmit, AFullRingIsPushedNotRefused) {
    udepot::StoreConfig config;
    config.grain_size = 512;
    config.force_destroy = true;
    config.queue_depth = 4;
    UringTraits::configure(config);
    udepot::uDepot<udepot::UringIO> store;
    ASSERT_EQ(store.open(config), 0);

    constexpr int kKeys = 64;
    for (int i = 0; i < kKeys; ++i)
        ASSERT_EQ(store.put(qd::make_key(i), qd::make_val(i)).run_sync(), 0);
    std::vector<qd::ReadCtx> ctxs(kKeys);
    std::vector<std::string> keys(kKeys);
    std::vector<udepot::CoroTask<int>> tasks;
    for (int i = 0; i < kKeys; ++i) {
        keys[i] = qd::make_key(i);
        tasks.push_back(store.get(keys[i], ctxs[i].val, sizeof(ctxs[i].val),
                                  &ctxs[i].val_size));
    }
    for (int i = 0; i < kKeys; ++i) {
        ASSERT_EQ(tasks[i].run_sync(), 0) << "i=" << i;
        EXPECT_EQ(std::string_view(reinterpret_cast<char*>(ctxs[i].val),
                                   ctxs[i].val_size),
                  qd::make_val(i));
    }
    store.close();
    UringTraits::cleanup(config);
}

// A poll that finds none of the thread's I/O completed spins, as uDepot's
// poller does, unless StoreConfig::poll_yields asks it to yield the CPU.
// With submissions refused (injected), nothing can complete, so every poll
// is an empty one.
TEST(UringPoll, AnEmptyPollSpinsUnlessAskedToYield) {
    static std::atomic<bool> refuse{false};
    udepot::UringIO::submit_test_hook = [] {
        return refuse.load() ? -EBUSY : 0;
    };
    struct ResetHook {
        ~ResetHook() { udepot::UringIO::submit_test_hook = nullptr; }
    } reset_hook;

    for (bool yields : {false, true}) {
        SCOPED_TRACE(yields ? "poll_yields" : "default");
        udepot::StoreConfig config;
        config.grain_size = 512;
        config.force_destroy = true;
        config.poll_yields = yields;
        UringTraits::configure(config);
        udepot::uDepot<udepot::UringIO> store;
        ASSERT_EQ(store.open(config), 0);
        ASSERT_EQ(store.put(qd::make_key(0), qd::make_val(0)).run_sync(), 0);

        refuse = true;
        qd::ReadCtx ctx;
        const std::string key = qd::make_key(0);
        auto task = store.get(key, ctx.val, sizeof(ctx.val), &ctx.val_size);
        const uint64_t before = udepot::UringIO::thread_idle_yields();
        for (int i = 0; i < 10; ++i)
            ASSERT_TRUE(udepot::UringIO::poll_thread());  // still pending
        EXPECT_EQ(udepot::UringIO::thread_idle_yields() - before,
                  yields ? 10u : 0u);
        refuse = false;
        ASSERT_EQ(task.run_sync(), 0);
        EXPECT_EQ(std::string_view(reinterpret_cast<char*>(ctx.val),
                                   ctx.val_size),
                  qd::make_val(0));
        store.close();
        UringTraits::cleanup(config);
    }
}

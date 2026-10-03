// Copyright (c) 2024-2026 Nikolas Ioannou
// SPDX-License-Identifier: BSD-3-Clause

// Runs against the namespace SpdkIO finds (UDEPOT_NVMEF, or a local NVMe
// device); scripts/spdk-nvmef-test.sh provides an NVMe-oF software target.

#include "udepot/io/spdk.h"

#include "queue_depth_tests.h"

namespace {

struct SpdkTraits {
    using IO = udepot::SpdkIO;
    // Completions are harvested by the submitting thread's own poll.
    static constexpr bool kPollerThread = false;
    // Every request of the queue pair taken: ENOMEM.
    static constexpr bool kFullQueueWaits = true;
    // The calling thread's queue pair.
    static uint64_t waited(IO&) { return udepot::SpdkIO::thread_waited_count(); }
    static void suite_setup() {
        ASSERT_EQ(udepot::SpdkIO::global_init(), 0)
            << "SpdkIO::global_init() failed — is an NVMe namespace or "
               "NVMeoF target available?";
    }
    static void suite_teardown() { udepot::SpdkIO::global_shutdown(); }
    static void configure(udepot::StoreConfig& config) {
        config.path = "SPDK";
        config.size = 0;  // the whole namespace
    }
    static void cleanup(const udepot::StoreConfig&) {}
};

}  // namespace

INSTANTIATE_TYPED_TEST_SUITE_P(Spdk, QueueDepthTest, SpdkTraits);

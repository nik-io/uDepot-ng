// Copyright (c) 2024-2026 Nikolas Ioannou
// SPDX-License-Identifier: BSD-3-Clause

#include "udepot/io/aio.h"

#include <filesystem>
#include <string>

#include <unistd.h>

#include "queue_depth_tests.h"

namespace {

struct AioTraits {
    using IO = udepot::AioIO;
    static constexpr bool kPollerThread = true;
    // io_submit fails with EAGAIN once the context is full.
    static constexpr bool kQueueFills = true;
    // A depth past the kernel's limits fails open().
    static constexpr bool kQueueSizeLimited = true;
    static void suite_setup() {}
    static void suite_teardown() {}
    static std::string& path() {
        static std::string p =
            (std::filesystem::temp_directory_path() /
             ("udepot_aio_qdepth_test_" + std::to_string(getpid())))
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

INSTANTIATE_TYPED_TEST_SUITE_P(Aio, QueueDepthTest, AioTraits);

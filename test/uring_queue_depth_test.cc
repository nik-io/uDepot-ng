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
    }
    static void cleanup(const udepot::StoreConfig&) {
        std::filesystem::remove(path());
    }
};

}  // namespace

INSTANTIATE_TYPED_TEST_SUITE_P(Uring, QueueDepthTest, UringTraits);

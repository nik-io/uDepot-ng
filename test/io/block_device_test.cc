// Copyright (c) 2024-2026 Nikolas Ioannou
// SPDX-License-Identifier: BSD-3-Clause

// A block device as the store, on the backends that open a path: posix, AIO
// and io_uring. The devices are loop devices over a file in /tmp, so these
// tests need root and losetup; without them they skip, and CI runs them
// under sudo and fails the job if any skipped (.github/workflows/ci.yml).

#include <cerrno>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <string>
#include <string_view>

#include <fcntl.h>
#include <sys/ioctl.h>
#include <sys/mount.h>
#include <sys/stat.h>
#include <unistd.h>

#include <gtest/gtest.h>

#include "udepot/io/aio.h"
#include "udepot/io/fd_io.h"
#include "udepot/io/posix.h"
#include "udepot/store.h"
#ifdef UDEPOT_BUILD_URING
#include "udepot/io/uring.h"
#endif

using udepot::AioIO;
using udepot::PosixIO;
using udepot::StoreConfig;
using udepot::uDepot;
#ifdef UDEPOT_BUILD_URING
using udepot::UringIO;
#endif

namespace {

// 16 MiB plus a page: the store needs a tail past its last whole segment
// for the device metadata, so the device is not a multiple of the segment.
constexpr uint64_t kDevBytes = (uint64_t{16} << 20) + 4096;
// The store's geometry is fixed so the device is large enough for it and
// the store never shrinks its segment (and prints another message).
constexpr uint32_t kGrain = 512;
constexpr uint64_t kSegmentGrains = 64;

uint64_t block_device_bytes(const char* dev) {
    int fd = ::open(dev, O_RDONLY);
    uint64_t bytes = 0;
    if (fd >= 0) {
        if (::ioctl(fd, BLKGETSIZE64, &bytes) < 0) bytes = 0;
        ::close(fd);
    }
    return bytes;
}

// Open file descriptors of this process: a failed open must leak none.
size_t open_fds() {
    size_t n = 0;
    for (const auto& e : std::filesystem::directory_iterator("/proc/self/fd")) {
        (void)e;
        ++n;
    }
    return n;
}

// A loop device over a temporary file, detached and removed on destruction
// whatever the test did.
class LoopDevice {
public:
    // Returns false and sets *why if there is no loop device to be had.
    bool create(uint64_t bytes, std::string* why) {
        if (::geteuid() != 0) {
            *why = "not root: losetup needs it";
            return false;
        }
        file_ = (std::filesystem::temp_directory_path() /
                 ("udepot_blockdev_" + std::to_string(::getpid()) + ".img"))
                    .string();
        const int fd = ::open(file_.c_str(), O_RDWR | O_CREAT | O_TRUNC, 0600);
        if (fd < 0 || ::ftruncate(fd, static_cast<off_t>(bytes)) < 0) {
            *why = "cannot create " + file_ + ": " + std::strerror(errno);
            if (fd >= 0) ::close(fd);
            return false;
        }
        ::close(fd);
        FILE* p = ::popen(("losetup --find --show " + file_ + " 2>&1").c_str(),
                          "r");
        if (p == nullptr) {
            *why = "losetup is not available";
            return false;
        }
        char line[256] = {};
        const char* got = std::fgets(line, sizeof(line), p);
        const int rc = ::pclose(p);
        std::string out = got != nullptr ? line : "";
        while (!out.empty() && (out.back() == '\n' || out.back() == '\r'))
            out.pop_back();
        if (rc != 0 || out.rfind("/dev/", 0) != 0) {
            *why = "losetup failed: " + out;
            return false;
        }
        dev_ = out;
        bytes_ = bytes;
        return true;
    }

    ~LoopDevice() {
        if (!dev_.empty() &&
            std::system(("losetup -d " + dev_ + " >/dev/null 2>&1").c_str()) != 0)
            std::fprintf(stderr, "block_device_test: could not detach %s\n",
                         dev_.c_str());
        if (!file_.empty()) std::filesystem::remove(file_);
    }

    const char* dev() const { return dev_.c_str(); }
    uint64_t bytes() const { return bytes_; }

    // The backing file's size: what ftruncate on the device would change.
    uint64_t file_bytes() const {
        struct stat st;
        return ::stat(file_.c_str(), &st) == 0 ? static_cast<uint64_t>(st.st_size)
                                               : 0;
    }

private:
    std::string file_;
    std::string dev_;
    uint64_t bytes_ = 0;
};

template <typename IO>
class BlockDeviceTest : public ::testing::Test {
protected:
    void SetUp() override {
        std::string why;
        if (!loop_.create(kDevBytes, &why)) GTEST_SKIP() << why;
        ASSERT_EQ(block_device_bytes(loop_.dev()), kDevBytes);
    }

    StoreConfig config(size_t size) {
        StoreConfig cfg;
        cfg.path = loop_.dev();
        cfg.size = size;
        cfg.grain_size = kGrain;
        cfg.segment_size = kSegmentGrains;
        cfg.force_destroy = true;
        return cfg;
    }

    LoopDevice loop_;
};

using Backends = ::testing::Types<PosixIO, AioIO
#ifdef UDEPOT_BUILD_URING
                                  ,
                                  UringIO
#endif
                                  >;
TYPED_TEST_SUITE(BlockDeviceTest, Backends);

// Size 0 is the whole device, not an empty store.
TYPED_TEST(BlockDeviceTest, SizeZeroOpensTheWholeDevice) {
    TypeParam io;
    ASSERT_EQ(io.open(this->loop_.dev(), 0), 0);
    EXPECT_EQ(io.get_size(), kDevBytes);
    if constexpr (requires { io.direct(); }) {
        EXPECT_TRUE(io.direct());
    }
    io.close();
}

// A size up to the device's is the size used, as the SPDK backend does.
TYPED_TEST(BlockDeviceTest, SmallerSizeIsUsedAsIs) {
    constexpr size_t kSize = (size_t{8} << 20) + 4096;
    TypeParam io;
    ASSERT_EQ(io.open(this->loop_.dev(), kSize), 0);
    EXPECT_EQ(io.get_size(), kSize);
    io.close();
}

TYPED_TEST(BlockDeviceTest, ExactDeviceSizeIsAccepted) {
    TypeParam io;
    ASSERT_EQ(io.open(this->loop_.dev(), kDevBytes), 0);
    EXPECT_EQ(io.get_size(), kDevBytes);
    io.close();
}

// More than the device holds cannot be provided, and the open says why and
// what to do; it is not truncated up to fit.
TYPED_TEST(BlockDeviceTest, LargerSizeFailsWithENOSPCAndSaysSo) {
    const size_t asked = kDevBytes + 4096;
    TypeParam io;
    const size_t fds = open_fds();
    ::testing::internal::CaptureStderr();
    const int rc = io.open(this->loop_.dev(), asked);
    const std::string log = ::testing::internal::GetCapturedStderr();
    EXPECT_EQ(rc, -ENOSPC);
    EXPECT_EQ(open_fds(), fds);  // the fd was closed
    EXPECT_NE(log.find("udepot: block device " + std::string(this->loop_.dev())),
              std::string::npos) << log;
    EXPECT_NE(log.find("is " + std::to_string(kDevBytes) + " bytes"),
              std::string::npos) << log;
    EXPECT_NE(log.find("smaller than the " + std::to_string(asked) + " asked for"),
              std::string::npos) << log;
    EXPECT_NE(log.find("or 0 for the whole device"), std::string::npos) << log;
    EXPECT_EQ(io.get_size(), 0u);
}

// A device with no capacity (an unbound loop device opens fine and reports
// 0 bytes) is refused, not taken as "the whole device": the store's
// geometry would underflow.
TYPED_TEST(BlockDeviceTest, DeviceOfZeroBytesFailsWithENOSPCAndSaysSo) {
    FILE* p = ::popen("losetup -f 2>/dev/null", "r");
    char line[256] = {};
    std::string free_dev;
    if (p != nullptr) {
        if (std::fgets(line, sizeof(line), p) != nullptr) free_dev = line;
        ::pclose(p);
    }
    while (!free_dev.empty() && (free_dev.back() == '\n')) free_dev.pop_back();
    if (free_dev.rfind("/dev/", 0) != 0 || block_device_bytes(free_dev.c_str()) != 0)
        GTEST_SKIP() << "no free (unbound, 0-byte) loop device: '" << free_dev << "'";
    for (size_t size : {size_t{0}, size_t{4096}}) {
        TypeParam io;
        const size_t fds = open_fds();
        ::testing::internal::CaptureStderr();
        const int rc = io.open(free_dev.c_str(), size);
        const std::string log = ::testing::internal::GetCapturedStderr();
        EXPECT_EQ(rc, -ENOSPC) << size;
        EXPECT_NE(log.find("udepot: block device " + free_dev + " has no capacity"),
                  std::string::npos) << log;
        EXPECT_EQ(io.get_size(), 0u);
        EXPECT_EQ(open_fds(), fds) << size;  // the fd was closed
    }
}

// A device without a store on it is not formatted unless told to: it may
// hold one under another size.
TYPED_TEST(BlockDeviceTest, NoStoreOnTheDeviceFailsWithEEXISTUnlessForced) {
    StoreConfig cfg = this->config(0);
    cfg.force_destroy = false;
    {
        uDepot<TypeParam> store;
        const size_t fds = open_fds();
        ::testing::internal::CaptureStderr();
        const int rc = store.open(cfg);
        const std::string log = ::testing::internal::GetCapturedStderr();
        EXPECT_EQ(rc, -EEXIST);
        EXPECT_NE(log.find("udepot: no store of " + std::to_string(kDevBytes) +
                           " bytes found on " + std::string(this->loop_.dev())),
                  std::string::npos) << log;
        EXPECT_NE(log.find("force_destroy"), std::string::npos) << log;
        EXPECT_EQ(open_fds(), fds);  // closed
    }
    cfg.force_destroy = true;
    uDepot<TypeParam> store;
    ASSERT_EQ(store.open(cfg), 0);
    ASSERT_EQ(store.put("a", "b").run_sync(), 0);
    store.close();
}

// The store sizes itself from the backend: size 0 is the whole device, a
// put/get works, and both survive a clean close and reopen.
TYPED_TEST(BlockDeviceTest, StoreOnTheWholeDeviceRoundTripsAndReopens) {
    StoreConfig cfg = this->config(0);
    {
        uDepot<TypeParam> store;
        ASSERT_EQ(store.open(cfg), 0);
        for (int i = 0; i < 200; ++i)
            ASSERT_EQ(store.put("k" + std::to_string(i),
                                "value-" + std::to_string(i)).run_sync(), 0);
        store.close();
    }
    cfg.force_destroy = false;
    uDepot<TypeParam> store;
    ASSERT_EQ(store.open(cfg), 0);
    EXPECT_TRUE(store.index_restored());
    for (int i = 0; i < 200; ++i) {
        uint8_t val[64];
        size_t n = 0;
        ASSERT_EQ(store.get("k" + std::to_string(i), val, sizeof(val), &n)
                      .run_sync(), 0) << i;
        EXPECT_EQ(std::string_view(reinterpret_cast<char*>(val), n),
                  "value-" + std::to_string(i));
    }
    store.close();
}

// The store reopens under the size it was given, not the device's: the
// metadata sits at the end of what it was sized to, so a different size on
// reopen would not find it.
TYPED_TEST(BlockDeviceTest, StoreOnPartOfTheDeviceRoundTripsAndReopens) {
    StoreConfig cfg = this->config((size_t{8} << 20) + 4096);
    {
        uDepot<TypeParam> store;
        ASSERT_EQ(store.open(cfg), 0);
        ASSERT_EQ(store.put("alpha", "one").run_sync(), 0);
        store.close();
    }
    cfg.force_destroy = false;
    uDepot<TypeParam> store;
    ASSERT_EQ(store.open(cfg), 0);
    uint8_t val[16];
    size_t n = 0;
    ASSERT_EQ(store.get("alpha", val, sizeof(val), &n).run_sync(), 0);
    EXPECT_EQ(std::string_view(reinterpret_cast<char*>(val), n), "one");
    store.close();
}

TYPED_TEST(BlockDeviceTest, StoreLargerThanTheDeviceFailsToOpen) {
    StoreConfig cfg = this->config(kDevBytes * 2);
    uDepot<TypeParam> store;
    ::testing::internal::CaptureStderr();
    const int rc = store.open(cfg);
    const std::string log = ::testing::internal::GetCapturedStderr();
    EXPECT_EQ(rc, -ENOSPC);
    EXPECT_NE(log.find("smaller than the"), std::string::npos) << log;
}

// Not block devices: sized by the file path as before, not by this helper.
TEST(SizeBlockDevice, RegularFileAndCharDeviceAreLeftToTheCaller) {
    const auto path = std::filesystem::temp_directory_path() /
                      ("udepot_blockdev_reg_" + std::to_string(::getpid()));
    int fd = ::open(path.c_str(), O_RDWR | O_CREAT, 0600);
    ASSERT_GE(fd, 0);
    size_t used = 12345;
    EXPECT_EQ(udepot::size_block_device(fd, path.c_str(), 4096, &used), 0);
    EXPECT_EQ(used, 12345u);  // untouched
    ::close(fd);
    std::filesystem::remove(path);

    fd = ::open("/dev/null", O_RDWR);
    ASSERT_GE(fd, 0);
    EXPECT_EQ(udepot::size_block_device(fd, "/dev/null", 4096, &used), 0);
    EXPECT_EQ(used, 12345u);
    ::close(fd);
}

// A regular file keeps its meaning of size: asked for, not "whole file".
TEST(SizeBlockDevice, RegularFileIsStillSizedToTheSizeAsked) {
    const auto path = std::filesystem::temp_directory_path() /
                      ("udepot_blockdev_reg2_" + std::to_string(::getpid()));
    PosixIO io;
    ASSERT_EQ(io.open(path.c_str(), 1 << 20), 0);
    EXPECT_EQ(io.get_size(), size_t{1} << 20);
    io.close();
    struct stat st;
    ASSERT_EQ(::stat(path.c_str(), &st), 0);
    EXPECT_EQ(static_cast<size_t>(st.st_size), size_t{1} << 20);
    std::filesystem::remove(path);
}

}  // namespace

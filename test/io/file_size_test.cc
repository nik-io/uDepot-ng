// Copyright (c) 2024-2026 Nikolas Ioannou
// SPDX-License-Identifier: BSD-3-Clause

// Size 0 as "the whole thing" on regular files, on the backends that open a
// path (posix, AIO, io_uring): a non-empty file is used whole, a new or
// empty one has no size and the open fails. Files are in /tmp, so the AIO
// and io_uring tests run O_DIRECT on a real filesystem. No root needed.

#include <cerrno>
#include <cstdint>
#include <cstring>
#include <filesystem>
#include <string>
#include <string_view>
#include <thread>

#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>

#include <gtest/gtest.h>

#include "udepot/io/aio.h"
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

constexpr uint32_t kGrain = 512;

// As the store tests: a tail past the last whole segment for the device
// metadata, and a segment that fits without the store shrinking it.
constexpr uint64_t kFileBytes = (uint64_t{16} << 20) + 4096;
constexpr uint64_t kSegmentGrains = 64;

size_t open_fds() {
    size_t n = 0;
    for (const auto& e : std::filesystem::directory_iterator("/proc/self/fd")) {
        (void)e;
        ++n;
    }
    return n;
}

template <typename IO>
class FileSizeTest : public ::testing::Test {
protected:
    void SetUp() override {
        path_ = std::filesystem::temp_directory_path() /
                ("udepot_file_size_" + std::to_string(::getpid()));
        std::filesystem::remove(path_);
    }
    void TearDown() override { std::filesystem::remove(path_); }

    // A sparse file of `bytes`: data (zero) the store has to be sized to.
    void make_file(uint64_t bytes) {
        int fd = ::open(path_.c_str(), O_RDWR | O_CREAT | O_TRUNC, 0600);
        ASSERT_GE(fd, 0);
        ASSERT_EQ(::ftruncate(fd, static_cast<off_t>(bytes)), 0);
        ::close(fd);
    }

    uint64_t file_bytes() const {
        struct stat st;
        return ::stat(path_.c_str(), &st) == 0 ? static_cast<uint64_t>(st.st_size)
                                               : UINT64_MAX;
    }

    StoreConfig config(size_t size) const {
        StoreConfig cfg;
        cfg.path = path_.c_str();
        cfg.size = size;
        cfg.grain_size = 512;
        cfg.segment_size = kSegmentGrains;
        cfg.force_destroy = true;
        return cfg;
    }

    std::filesystem::path path_;
};

using Backends = ::testing::Types<PosixIO, AioIO
#ifdef UDEPOT_BUILD_URING
                                  ,
                                  UringIO
#endif
                                  >;
TYPED_TEST_SUITE(FileSizeTest, Backends);

// Size 0 on a file with a length is that length, and the file keeps it.
TYPED_TEST(FileSizeTest, SizeZeroOnANonEmptyFileIsItsWholeLength) {
    this->make_file(kFileBytes);
    TypeParam io;
    ASSERT_EQ(io.open(this->path_.c_str(), 0), 0);
    EXPECT_EQ(io.get_size(), kFileBytes);
    io.close();
    EXPECT_EQ(this->file_bytes(), kFileBytes);
}

// The O_DIRECT backends fill the holes of the file they were given, as for
// a size asked for: the store is never silently sparse.
TYPED_TEST(FileSizeTest, SizeZeroPreallocatesWhereTheBackendPreallocates) {
    // Posix does not preallocate: nothing to check there.
    if constexpr (requires(TypeParam io) { io.direct(); }) {
        this->make_file(kFileBytes);
        struct stat st;
        ASSERT_EQ(::stat(this->path_.c_str(), &st), 0);
        ASSERT_LT(static_cast<uint64_t>(st.st_blocks) * 512, kFileBytes);
        TypeParam io;
        ASSERT_EQ(io.open(this->path_.c_str(), 0), 0);
        io.close();
        ASSERT_EQ(::stat(this->path_.c_str(), &st), 0);
        EXPECT_GE(static_cast<uint64_t>(st.st_blocks) * 512,
                  static_cast<uint64_t>(st.st_size));
        EXPECT_EQ(this->file_bytes(), kFileBytes);
    }
}

// A store with size 0 on such a file works and survives close and reopen,
// both with size 0.
TYPED_TEST(FileSizeTest, StoreWithSizeZeroRoundTripsAndReopens) {
    this->make_file(kFileBytes);
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
    EXPECT_EQ(this->file_bytes(), kFileBytes);
}

// A new file, or an empty one, has no size to be the whole of.
TYPED_TEST(FileSizeTest, SizeZeroOnANewFileFailsWithEINVALAndSaysSo) {
    TypeParam io;
    const size_t fds = open_fds();
    ::testing::internal::CaptureStderr();
    const int rc = io.open(this->path_.c_str(), 0);
    const std::string log = ::testing::internal::GetCapturedStderr();
    EXPECT_EQ(rc, -EINVAL);
    EXPECT_NE(log.find("udepot: " + this->path_.string() +
                       " is a new or empty file"), std::string::npos) << log;
    EXPECT_NE(log.find("size 0 asks for its whole size"), std::string::npos)
        << log;
    EXPECT_NE(log.find("give the store a size"), std::string::npos) << log;
    EXPECT_EQ(open_fds(), fds);  // the fd was closed
    EXPECT_EQ(this->file_bytes(), 0u);  // created, as uDepot does, and kept
}

TYPED_TEST(FileSizeTest, SizeZeroOnAnEmptyFileFailsWithEINVALAndSaysSo) {
    this->make_file(0);
    TypeParam io;
    const size_t fds = open_fds();
    ::testing::internal::CaptureStderr();
    const int rc = io.open(this->path_.c_str(), 0);
    const std::string log = ::testing::internal::GetCapturedStderr();
    EXPECT_EQ(rc, -EINVAL);
    EXPECT_NE(log.find("is a new or empty file"), std::string::npos) << log;
    EXPECT_EQ(open_fds(), fds);
    EXPECT_EQ(this->file_bytes(), 0u);
}

// A size above 0 is unchanged: grown to it, and a larger file used up to it.
TYPED_TEST(FileSizeTest, SizeAskedForIsUnchangedOnAFile) {
    this->make_file(4096);
    {
        TypeParam io;
        ASSERT_EQ(io.open(this->path_.c_str(), 1 << 20), 0);
        EXPECT_EQ(io.get_size(), size_t{1} << 20);
        io.close();
        EXPECT_EQ(this->file_bytes(), uint64_t{1} << 20);
    }
    TypeParam io;
    ASSERT_EQ(io.open(this->path_.c_str(), 8192), 0);
    EXPECT_EQ(io.get_size(), 8192u);
    io.close();
    EXPECT_EQ(this->file_bytes(), uint64_t{1} << 20);
}

// Data in the file before an open with size 0 is there after it: the
// preallocation fills holes and keeps what is written.
TYPED_TEST(FileSizeTest, DataSurvivesTheSizeZeroOpen) {
    this->make_file(kFileBytes);
    {
        char marker[4096];
        std::memset(marker, 0x5a, sizeof(marker));
        std::memcpy(marker, "marker", 6);
        int fd = ::open(this->path_.c_str(), O_RDWR);
        ASSERT_GE(fd, 0);
        ASSERT_EQ(::pwrite(fd, marker, sizeof(marker), 8192),
                  ssize_t{4096});
        ::close(fd);
    }
    TypeParam io;
    ASSERT_EQ(io.open(this->path_.c_str(), 0), 0);
    auto buf = io.alloc_buffer(4096);
    ASSERT_NE(buf.data, nullptr);
    ASSERT_EQ(io.pread(buf.data, 4096, 8192).run_sync(), ssize_t{4096});
    EXPECT_EQ(std::memcmp(buf.data, "marker", 6), 0);
    EXPECT_EQ(static_cast<uint8_t*>(buf.data)[4095], 0x5a);
    io.close();
}

// A store is formatted over nothing it was not told to destroy. These
// reopen one under another size, where the metadata is not looked for at
// the end of what it was created with.
class OpenStore {
public:
    template <typename IO>
    static int open(uDepot<IO>& store, StoreConfig cfg, std::string* log) {
        ::testing::internal::CaptureStderr();
        const int rc = store.open(cfg);
        *log = ::testing::internal::GetCapturedStderr();
        return rc;
    }
};

TYPED_TEST(FileSizeTest, ReopenWithAnotherSizeFailsWithEEXISTAndKeepsTheData) {
    // The store made on a bigger file than it was sized to.
    this->make_file(2 * kFileBytes);
    {
        uDepot<TypeParam> store;
        ASSERT_EQ(store.open(this->config(kFileBytes)), 0);
        ASSERT_EQ(store.put("keep", "me").run_sync(), 0);
        store.close();
    }
    // Size 0 now is the whole (bigger) file; the others are smaller and
    // larger than the store was created with.
    for (size_t size : {size_t{0}, kFileBytes - (1 << 20), kFileBytes + (1 << 20)}) {
        StoreConfig cfg = this->config(size);
        cfg.force_destroy = false;
        uDepot<TypeParam> store;
        const size_t fds = open_fds();
        std::string log;
        EXPECT_EQ(OpenStore::open(store, cfg, &log), -EEXIST) << size;
        EXPECT_NE(log.find("udepot: no store of "), std::string::npos) << log;
        EXPECT_NE(log.find(" found on " + this->path_.string() +
                           ", which is not a new or empty file"), std::string::npos) << log;
        EXPECT_NE(log.find("force_destroy"), std::string::npos) << log;
        EXPECT_EQ(open_fds(), fds) << size;  // closed
    }
    // The data is still there under the size it was created with.
    StoreConfig cfg = this->config(kFileBytes);
    cfg.force_destroy = false;
    uDepot<TypeParam> store;
    ASSERT_EQ(store.open(cfg), 0);
    EXPECT_TRUE(store.index_restored());
    uint8_t val[16];
    size_t n = 0;
    ASSERT_EQ(store.get("keep", val, sizeof(val), &n).run_sync(), 0);
    EXPECT_EQ(std::string_view(reinterpret_cast<char*>(val), n), "me");
    store.close();
}

// force_destroy says to start a new store over what is there.
TYPED_TEST(FileSizeTest, ForceDestroyFormatsOverAFileWithData) {
    this->make_file(kFileBytes);
    uDepot<TypeParam> store;
    StoreConfig cfg = this->config(kFileBytes);
    cfg.force_destroy = false;
    std::string log;
    EXPECT_EQ(OpenStore::open(store, cfg, &log), -EEXIST);  // nothing on it
    cfg.force_destroy = true;
    ASSERT_EQ(store.open(cfg), 0);
    store.close();
}

// A file that is new, or empty, holds nothing to destroy: it opens as a
// new store without force_destroy.
TYPED_TEST(FileSizeTest, NewFileOpensAsANewStore) {
    StoreConfig cfg = this->config(kFileBytes);
    cfg.force_destroy = false;
    uDepot<TypeParam> store;
    ASSERT_EQ(store.open(cfg), 0);
    EXPECT_FALSE(store.index_restored());
    ASSERT_EQ(store.put("a", "b").run_sync(), 0);
    store.close();
}

TYPED_TEST(FileSizeTest, EmptyFileOpensAsANewStore) {
    this->make_file(0);
    StoreConfig cfg = this->config(kFileBytes);
    cfg.force_destroy = false;
    uDepot<TypeParam> store;
    ASSERT_EQ(store.open(cfg), 0);
    EXPECT_FALSE(store.index_restored());
    store.close();
}

// A refused open leaves the file as it was: the store created with size 0
// (the whole file) is not grown by an open at a larger size that is
// refused, and still opens with size 0.
TYPED_TEST(FileSizeTest, RefusedOpenAtALargerSizeLeavesTheFileAlone) {
    this->make_file(kFileBytes);
    {
        uDepot<TypeParam> store;
        ASSERT_EQ(store.open(this->config(0)), 0);
        ASSERT_EQ(store.put("keep", "me").run_sync(), 0);
        store.close();
    }
    StoreConfig cfg = this->config(kFileBytes + (1 << 20));
    cfg.force_destroy = false;
    uDepot<TypeParam> store;
    const size_t fds = open_fds();
    std::string log;
    EXPECT_EQ(OpenStore::open(store, cfg, &log), -EEXIST);
    EXPECT_NE(log.find("udepot: a store of " + std::to_string(cfg.size) +
                       " bytes cannot be on " + this->path_.string()),
              std::string::npos) << log;
    EXPECT_NE(log.find("force_destroy"), std::string::npos) << log;
    EXPECT_EQ(open_fds(), fds);
    EXPECT_EQ(this->file_bytes(), kFileBytes);  // not grown

    cfg = this->config(0);
    cfg.force_destroy = false;
    ASSERT_EQ(store.open(cfg), 0);
    EXPECT_TRUE(store.index_restored());
    uint8_t val[16];
    size_t n = 0;
    ASSERT_EQ(store.get("keep", val, sizeof(val), &n).run_sync(), 0);
    EXPECT_EQ(std::string_view(reinterpret_cast<char*>(val), n), "me");
    store.close();
}

// A first open that fails before the store is created leaves the new file
// empty, so the next open is a first one again, not a refusal.
TYPED_TEST(FileSizeTest, FailedFirstOpenLeavesTheNewFileEmpty) {
    StoreConfig cfg = this->config(64 * 1024);  // too few spare segments
    cfg.force_destroy = false;
    {
        uDepot<TypeParam> store;
        std::string log;
        EXPECT_EQ(OpenStore::open(store, cfg, &log), -EINVAL) << log;
        EXPECT_EQ(this->file_bytes(), 0u);
    }
    cfg = this->config(kFileBytes);
    cfg.force_destroy = false;
    uDepot<TypeParam> store;
    ASSERT_EQ(store.open(cfg), 0);
    EXPECT_FALSE(store.index_restored());
    store.close();
}

// zlib's CRC32 as the store's metadata checksums use it.
uint32_t crc32_update(uint32_t crc, const uint8_t* data, size_t len) {
    crc = ~crc;
    for (size_t i = 0; i < len; ++i) {
        crc ^= data[i];
        for (int j = 0; j < 8; ++j)
            crc = (crc >> 1) ^ (crc & 1 ? 0xEDB88320u : 0);
    }
    return ~crc;
}

// A store whose metadata says another grain size is told so, not told it
// has the wrong size. Geometry cannot lead a reopen under another grain
// size to that metadata (it is looked for at the end of the size, a grain
// from it), so the metadata is rewritten as a store of 4096-byte grains
// would have it, with its checksum (a CRC32 and the store's global seed).
TYPED_TEST(FileSizeTest, MetadataOfAnotherGrainSizeSaysSo) {
    this->make_file(kFileBytes);
    {
        uDepot<TypeParam> store;
        ASSERT_EQ(store.open(this->config(kFileBytes)), 0);
        store.close();
    }
    // salsa_dev_md: u64 physical, logical, segment, grain, seed; u32 csum.
    uint8_t md[44];
    const off_t md_off = static_cast<off_t>(kFileBytes) - kGrain;
    int fd = ::open(this->path_.c_str(), O_RDWR);
    ASSERT_GE(fd, 0);
    ASSERT_EQ(::pread(fd, md, sizeof(md), md_off), ssize_t{44});
    const uint64_t grain = 4096;
    std::memcpy(md + 24, &grain, sizeof(grain));
    uint32_t seed;
    std::memcpy(&seed, md + 32, sizeof(seed));
    const uint32_t global = 0xDEADBEEF;
    uint32_t csum = crc32_update(seed, md, 40);
    csum = crc32_update(csum, reinterpret_cast<const uint8_t*>(&global), 4);
    std::memcpy(md + 40, &csum, sizeof(csum));
    ASSERT_EQ(::pwrite(fd, md, sizeof(md), md_off), ssize_t{44});
    ::close(fd);

    StoreConfig cfg = this->config(kFileBytes);
    cfg.force_destroy = false;
    uDepot<TypeParam> store;
    const size_t fds = open_fds();
    std::string log;
    EXPECT_EQ(OpenStore::open(store, cfg, &log), -EINVAL);
    EXPECT_NE(log.find("grains of 4096 bytes, not the 512 asked for"),
              std::string::npos) << log;
    EXPECT_EQ(open_fds(), fds);
}

// An open that fails after the file was sized (no AIO context, no ring)
// leaves a new file empty, as one that fails in the store does. Only AIO and
// io_uring have a per-thread setup to fail. It runs on a thread of its own,
// whose context or ring is made by the open.
TYPED_TEST(FileSizeTest, FailedThreadSetupLeavesTheNewFileEmpty) {
    if constexpr (requires { TypeParam::setup_test_hook; }) {
        struct ResetHook {
            ~ResetHook() { TypeParam::setup_test_hook = nullptr; }
        } reset_hook;
        StoreConfig cfg = this->config(kFileBytes);
        cfg.force_destroy = false;
        std::thread([&] {
            TypeParam::setup_test_hook = [] { return -ENOMEM; };
            uDepot<TypeParam> store;
            const size_t fds = open_fds();
            std::string log;
            EXPECT_EQ(OpenStore::open(store, cfg, &log), -ENOMEM);
            EXPECT_EQ(open_fds(), fds);
            EXPECT_EQ(this->file_bytes(), 0u);

            TypeParam::setup_test_hook = nullptr;
            ASSERT_EQ(store.open(cfg), 0);
            EXPECT_FALSE(store.index_restored());
            store.close();
        }).join();
    }
}

}  // namespace

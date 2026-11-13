// Copyright (c) 2024-2026 Nikolas Ioannou
// SPDX-License-Identifier: BSD-3-Clause

#include "pyudepot.h"

#include <algorithm>
#include <cerrno>
#include <cstdio>
#include <cstring>
#include <span>

#include <fcntl.h>
#include <linux/fs.h>
#include <sys/ioctl.h>
#include <sys/stat.h>
#include <unistd.h>

#include "sto-ctlr/sto-capacity-mgr-parameters.h"
#include "udepot/multi_get.h"
#include "bindings/segment_check.h"
#include "udepot/store.h"
#include "udepot/io/aio.h"

using Store = udepot::uDepot<udepot::AioIO>;

namespace pyudepot_internal {

// The alignment O_DIRECT needs of the backing at path, in bytes, or 0 when
// it cannot be told: a block device's logical sector size (BLKSSZGET); a
// regular file's dio_offset_align (statx STATX_DIOALIGN, Linux 6.1 and a
// filesystem that reports it). A file that does not exist yet has no
// filesystem to ask, and a kernel without STATX_DIOALIGN does not say: the
// grain is then not checked against it, and the store's open reports what
// the device refuses. (The filesystem's block size is not a fallback: ext4
// takes 512-byte O_DIRECT I/O on a 4096-byte block.) *what names the source.
uint32_t direct_io_alignment(const char* path, const char** what) {
    struct stat st;
    if (::stat(path, &st) != 0) return 0;
    if (S_ISBLK(st.st_mode)) {
        int fd = ::open(path, O_RDONLY);
        if (fd < 0) return 0;
        int sector = 0;
        int rc = ::ioctl(fd, BLKSSZGET, &sector);
        ::close(fd);
        if (rc != 0 || sector <= 0) return 0;
        *what = "the block device's logical sector size";
        return static_cast<uint32_t>(sector);
    }
#ifdef STATX_DIOALIGN
    if (S_ISREG(st.st_mode)) {
        struct statx stx;
        if (::statx(AT_FDCWD, path, 0, STATX_DIOALIGN, &stx) == 0 &&
            (stx.stx_mask & STATX_DIOALIGN) && stx.stx_dio_offset_align) {
            *what = "the file's direct I/O offset alignment (statx)";
            return stx.stx_dio_offset_align;
        }
    }
#endif
    return 0;
}

// Whether a grain is a multiple of the backing's O_DIRECT alignment (0 =
// unknown: accepted). If not, says so on stderr, naming both numbers.
bool grain_fits_alignment(uint32_t grain, uint32_t align, const char* path,
                          const char* what) {
    if (align == 0 || grain % align == 0) return true;
    fprintf(stderr, "uDepot-ng: grain size %u is smaller than the %u bytes "
            "O_DIRECT needs on %s (%s): every I/O is a multiple of the grain, "
            "and the device would refuse it. Use a grain of at least %u "
            "bytes\n", grain, align, path, what, align);
    return false;
}

}  // namespace pyudepot_internal

extern "C" {

static constexpr uint32_t kDefaultGrainBytes = 4096;

// The StoreConfig::grain_size values the AIO store works with, which the
// store itself does not check (uDepot::open takes any):
//  - a power of two: below 4096, `sanitize` in uDepot::open rounds a segment
//    to whole 4096-byte pages by 4096 / grain, which needs a divisor of
//    4096. At and above 4096 the store needs no such thing that anyone has
//    found, but only powers of two (uDepot's grains) are ever run, so
//    nothing else is let in;
//  - at least 512: the store file is opened O_DIRECT (open_direct), whose
//    offsets and lengths must be a multiple of the device's logical block
//    size, 512 at the smallest (a larger one is checked against the
//    backing, direct_io_alignment); every record, segment and metadata I/O
//    is a multiple of the grain;
//  - at most 1 MiB: a record is at most HashEntry::kKvSizeMask = 2047 grains
//    and is written with one pwrite, which Linux caps at MAX_RW_COUNT =
//    INT_MAX & PAGE_MASK = 2147479552 bytes. 2047 grains of 1 MiB are
//    2146435072; of 2 MiB they are twice that, and put would fail with -EIO
//    (store.cc, put_record: written != total) with no message.
static constexpr uint32_t kMinGrainBytes = 512;
static constexpr uint32_t kMaxGrainBytes = uint32_t{1} << 20;

static bool grain_is_valid(uint32_t grain) {
    return grain >= kMinGrainBytes && grain <= kMaxGrainBytes &&
           (grain & (grain - 1)) == 0;
}

void* uDepotOpenSegment(const char* fname, uint64_t size, int force_destroy,
                        uint32_t queue_depth, uint32_t grain_bytes,
                        uint64_t segment_bytes) {
    if (!grain_is_valid(grain_bytes)) {
        fprintf(stderr, "uDepot-ng: grain size %u is not valid: it must be "
                "a power of two from %u to %u bytes (O_DIRECT needs at least "
                "512-byte blocks; a record of 2047 grains must fit in one "
                "write of at most 2147479552 bytes)\n", grain_bytes,
                kMinGrainBytes, kMaxGrainBytes);
        return nullptr;
    }
    {
        const char* what = "";
        const uint32_t align =
            pyudepot_internal::direct_io_alignment(fname, &what);
        if (!pyudepot_internal::grain_fits_alignment(grain_bytes, align,
                                                     fname, what))
            return nullptr;
    }
    if (segment_bytes != 0) {
        if (!udepot_binding::check_segment_size(segment_bytes, grain_bytes))
            return nullptr;
    }
    udepot::StoreConfig config;
    config.queue_depth = queue_depth;
    config.force_destroy = force_destroy != 0;
    config.path = fname;
    config.size = static_cast<size_t>(size);
    config.grain_size = grain_bytes;
    // 0 is the store's own default segment (fitted to a power-of-two table
    // and halved to fit the device).
    config.segment_size = segment_bytes / grain_bytes;

    auto* store = new (std::nothrow) Store();
    if (!store) return nullptr;
    int rc = store->open(config);
    if (rc != 0) {
        fprintf(stderr, "failed to init uDepot-ng err=%s\n", strerror(-rc));
        delete store;
        return nullptr;
    }
    return static_cast<void*>(store);
}

uint64_t uDepotSegmentForRecords(uint32_t grain_bytes,
                                 uint32_t record_grains) {
    if (!grain_is_valid(grain_bytes)) {
        fprintf(stderr, "uDepot-ng: grain size %u is not valid: it must be "
                "a power of two from %u to %u bytes\n", grain_bytes,
                kMinGrainBytes, kMaxGrainBytes);
        return 0;
    }
    return udepot_binding::segment_bytes_for_records(grain_bytes,
                                                     record_grains);
}

void* uDepotOpenGrain(const char* fname, uint64_t size, int force_destroy,
                      uint32_t queue_depth, uint32_t grain_bytes) {
    return uDepotOpenSegment(fname, size, force_destroy, queue_depth,
                             grain_bytes, 0);
}

void* uDepotOpenQueue(const char* fname, uint64_t size, int force_destroy,
                      uint32_t queue_depth) {
    return uDepotOpenGrain(fname, size, force_destroy, queue_depth,
                           kDefaultGrainBytes);
}

void* uDepotOpen(const char* fname, uint64_t size, int force_destroy) {
    return uDepotOpenQueue(fname, size, force_destroy, 0);
}

// As uDepot's uDepotClose: shut the store down and free it. The caller
// must ensure no call on this handle is in progress or follows.
void uDepotClose(void* kv) {
    if (!kv) return;
    auto* store = static_cast<Store*>(kv);
    store->close();
    delete store;
}

int uDepotGet(void* kv, const uint8_t key[], uint32_t key_size,
              uint8_t val_buf[], uint64_t val_buf_size) {
    auto* store = static_cast<Store*>(kv);
    auto key_span = std::span<const uint8_t>(key, key_size);
    size_t val_size_read = 0;
    int rc = store->get(key_span, val_buf,
                        static_cast<size_t>(val_buf_size),
                        &val_size_read).run_sync();
    return rc;
}

int uDepotPut(void* kv, const uint8_t key[], uint32_t key_size,
              const uint8_t val[], uint64_t val_size) {
    auto* store = static_cast<Store*>(kv);
    auto key_span = std::span<const uint8_t>(key, key_size);
    auto val_span = std::span<const uint8_t>(val,
                                             static_cast<size_t>(val_size));
    return store->put(key_span, val_span).run_sync();
}

int uDepotDel(void* kv, const uint8_t key[], uint32_t key_size) {
    auto* store = static_cast<Store*>(kv);
    auto key_span = std::span<const uint8_t>(key, key_size);
    return store->del(key_span).run_sync();
}

int uDepotExists(void* kv, const uint8_t key[], uint32_t key_size,
                 uint64_t* val_size_out) {
    auto* store = static_cast<Store*>(kv);
    auto key_span = std::span<const uint8_t>(key, key_size);
    size_t val_size = 0;
    int rc = store->exists(key_span, &val_size).run_sync();
    if (rc == 0 && val_size_out)
        *val_size_out = val_size;
    return rc;
}

void* uDepotAllocPutBuffer(void* kv, uint32_t key_size, uint64_t val_size) {
    auto* store = static_cast<Store*>(kv);
    auto* pb = new (std::nothrow) udepot::PutBuffer();
    if (pb) *pb = store->alloc_put_buffer(key_size,
                                          static_cast<size_t>(val_size));
    if (!pb || !pb->valid()) {
        fprintf(stderr, "uDepot-ng: cannot allocate a put buffer for a key "
                "of %u bytes and a value of %llu bytes\n", key_size,
                static_cast<unsigned long long>(val_size));
        delete pb;
        return nullptr;
    }
    return pb;
}

uint8_t* uDepotPutBufferValue(void* put_buf, uint64_t* size_out) {
    auto value = static_cast<udepot::PutBuffer*>(put_buf)->value();
    *size_out = value.size();
    return value.data();
}

int uDepotPutBuf(void* kv, const uint8_t key[], uint32_t key_size,
                 void* put_buf) {
    auto* store = static_cast<Store*>(kv);
    auto key_span = std::span<const uint8_t>(key, key_size);
    return store->put(key_span, *static_cast<udepot::PutBuffer*>(put_buf))
        .run_sync();
}

void uDepotFreePutBuffer(void* put_buf) {
    delete static_cast<udepot::PutBuffer*>(put_buf);
}

void* uDepotAllocGetBuffer(void* kv, uint32_t key_size, uint64_t val_size) {
    auto* store = static_cast<Store*>(kv);
    auto* gb = new (std::nothrow) udepot::GetBuffer();
    if (gb) *gb = store->alloc_get_buffer(key_size,
                                          static_cast<size_t>(val_size));
    if (!gb || !gb->valid()) {
        fprintf(stderr, "uDepot-ng: cannot allocate a get buffer for a key "
                "of %u bytes and a value of %llu bytes\n", key_size,
                static_cast<unsigned long long>(val_size));
        delete gb;
        return nullptr;
    }
    return gb;
}

void* uDepotNewGetBuffer(void) {
    return new (std::nothrow) udepot::GetBuffer();
}

int uDepotGetBuf(void* kv, const uint8_t key[], uint32_t key_size,
                 void* get_buf) {
    auto* store = static_cast<Store*>(kv);
    auto key_span = std::span<const uint8_t>(key, key_size);
    return store->get(key_span, static_cast<udepot::GetBuffer*>(get_buf))
        .run_sync();
}

int uDepotMultiGetBuf(void* kv, uint32_t n, const uint8_t* const keys[],
                      const uint32_t key_sizes[], void* const get_bufs[],
                      int results[]) {
    auto* store = static_cast<Store*>(kv);
    // At most what the calling thread's AIO context takes (a get past it
    // fails -EAGAIN). The context is the thread's, shared by every store on
    // it and sized by the store opened last, so the store's own
    // queue_depth() (StoreConfig::queue_depth, default 1024) can exceed it.
    const unsigned window =
        std::min(store->io().queue_depth(), udepot::AioIO::thread_queue_room());
    return udepot::multi_get(
        *store, n, keys, key_sizes,
        reinterpret_cast<udepot::GetBuffer* const*>(get_bufs), results,
        window);
}

const uint8_t* uDepotGetBufferValue(void* get_buf, uint64_t* size_out) {
    auto value = static_cast<udepot::GetBuffer*>(get_buf)->value();
    *size_out = value.size();
    return value.data();
}

void uDepotFreeGetBuffer(void* get_buf) {
    delete static_cast<udepot::GetBuffer*>(get_buf);
}

uint64_t uDepotThreadValueCopies(void) { return Store::thread_value_copies(); }


uint64_t uDepotMaxValueBytes(void* kv, uint32_t key_size) {
    return static_cast<Store*>(kv)->max_value_bytes(key_size);
}

uint32_t uDepotGrainBytes(void* kv) {
    return static_cast<Store*>(kv)->grain_size();
}

uint64_t uDepotSegmentBytes(void* kv) {
    auto* store = static_cast<Store*>(kv);
    return store->segment_grains() * store->grain_size();
}

}  // extern "C"

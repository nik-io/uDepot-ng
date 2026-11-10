// Copyright (c) 2024-2026 Nikolas Ioannou
// SPDX-License-Identifier: BSD-3-Clause

#include "pyudepot.h"

#include <algorithm>
#include <cerrno>
#include <cstdio>
#include <cstring>
#include <span>

#include "udepot/multi_get.h"
#include "udepot/store.h"
#include "udepot/io/aio.h"

using Store = udepot::uDepot<udepot::AioIO>;

extern "C" {

// Small tables for tests, nothing inherent to the bindings: a store sized
// for real use passes its own (uDepotOpenTables). Each directory table
// fills one segment and all of its memory is touched when it is created
// (clear()), so the store's default would be up to 512 MiB per table. A
// segment this small maps on 4 KiB pages, and open() says so.
static constexpr uint64_t kDefaultTableBytes = uint64_t{2} << 20;

void* uDepotOpenQueue(const char* fname, uint64_t size, int force_destroy,
                      uint64_t table_bytes, uint32_t queue_depth) {
    udepot::StoreConfig config;
    config.queue_depth = queue_depth;
    config.force_destroy = force_destroy != 0;
    config.path = fname;
    config.size = static_cast<size_t>(size);
    config.grain_size = 4096;
    if (int rc = udepot::segment_size_for_table(table_bytes, config.grain_size,
                                                &config.segment_size);
        rc != 0) {
        fprintf(stderr, "uDepot-ng: table size %llu is not a power of two "
                "of at least 4096 bytes, nor 0 (the default)\n",
                static_cast<unsigned long long>(table_bytes));
        return nullptr;
    }

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

void* uDepotOpenTables(const char* fname, uint64_t size, int force_destroy,
                       uint64_t table_bytes) {
    return uDepotOpenQueue(fname, size, force_destroy, table_bytes, 0);
}

void* uDepotOpen(const char* fname, uint64_t size, int force_destroy) {
    return uDepotOpenTables(fname, size, force_destroy, kDefaultTableBytes);
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

}  // extern "C"

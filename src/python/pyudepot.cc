// Copyright (c) 2024-2026 Nikolas Ioannou
// SPDX-License-Identifier: BSD-3-Clause

#include "pyudepot.h"

#include <cerrno>
#include <cstdio>
#include <cstring>
#include <span>

#include "udepot/store.h"
#include "udepot/io/aio.h"

using Store = udepot::UDepot<udepot::AioIO>;

extern "C" {

void* uDepotOpen(const char* fname, uint64_t size, int force_destroy) {
    (void)force_destroy;

    auto* store = new (std::nothrow) Store();
    if (!store) return nullptr;

    udepot::StoreConfig config;
    config.path = fname;
    config.size = static_cast<size_t>(size);
    config.grain_size = 4096;
    config.initial_tables = 4;
    config.index_bits = 14;

    int rc = store->open(config);
    if (rc != 0) {
        fprintf(stderr, "failed to init uDepot-ng err=%s\n", strerror(-rc));
        delete store;
        return nullptr;
    }
    return static_cast<void*>(store);
}

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

}  // extern "C"

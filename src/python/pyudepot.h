// Copyright (c) 2024-2026 Nikolas Ioannou
// SPDX-License-Identifier: BSD-3-Clause

#pragma once

#define DLLOUT __attribute__((visibility("default")))

#include <cstdint>

extern "C" {

DLLOUT void* uDepotOpen(const char* fname, uint64_t size, int force_destroy);
DLLOUT void  uDepotClose(void* kv);
DLLOUT int   uDepotGet(void* kv, const uint8_t key[], uint32_t key_size,
                        uint8_t val_buf[], uint64_t val_buf_size);
DLLOUT int   uDepotPut(void* kv, const uint8_t key[], uint32_t key_size,
                        const uint8_t val[], uint64_t val_size);
DLLOUT int   uDepotDel(void* kv, const uint8_t key[], uint32_t key_size);
DLLOUT int   uDepotExists(void* kv, const uint8_t key[], uint32_t key_size,
                           uint64_t* val_size_out);

}

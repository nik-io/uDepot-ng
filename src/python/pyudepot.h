// Copyright (c) 2024-2026 Nikolas Ioannou
// SPDX-License-Identifier: BSD-3-Clause

#pragma once

#define DLLOUT __attribute__((visibility("default")))

#include <cstdint>

extern "C" {

// Index tables of 2 MiB (kDefaultTableBytes): small, for tests. A store
// sized for real use passes its own to uDepotOpenTables.
DLLOUT void* uDepotOpen(const char* fname, uint64_t size, int force_destroy);
// As uDepotOpen, with index tables of table_bytes: a power of two of at
// least 4 KiB, or 0 for the store's default (512 MiB tables).
DLLOUT void* uDepotOpenTables(const char* fname, uint64_t size,
                              int force_destroy, uint64_t table_bytes);
DLLOUT void  uDepotClose(void* kv);
DLLOUT int   uDepotGet(void* kv, const uint8_t key[], uint32_t key_size,
                        uint8_t val_buf[], uint64_t val_buf_size);
DLLOUT int   uDepotPut(void* kv, const uint8_t key[], uint32_t key_size,
                        const uint8_t val[], uint64_t val_size);
DLLOUT int   uDepotDel(void* kv, const uint8_t key[], uint32_t key_size);
DLLOUT int   uDepotExists(void* kv, const uint8_t key[], uint32_t key_size,
                           uint64_t* val_size_out);

// Zero copy (uDepot's Mbuff interface; see PutBuffer and GetBuffer in
// udepot/store.h). A handle is a heap PutBuffer or GetBuffer, freed with
// its own Free call exactly once; handles are not thread-safe, and a
// buffer's memory is plain heap memory: this binding is AIO-only, so it is
// aligned_alloc/free memory and freeing it after uDepotClose is fine. (On
// SPDK it would be rte_malloc memory tied to the SPDK environment; the
// binding does not expose that.)
//
// A PutBuffer for a key of key_size bytes and a value of val_size bytes, or
// NULL (with a message on stderr) if the sizes cannot be stored or
// allocation fails.
DLLOUT void* uDepotAllocPutBuffer(void* kv, uint32_t key_size,
                                  uint64_t val_size);
// The buffer's value: writable, *size_out bytes, starting
// sizeof(KvHeader) + key_size = 14 + key_size bytes into the record buffer
// (so unaligned, whatever the buffer's own alignment).
DLLOUT uint8_t* uDepotPutBufferValue(void* put_buf, uint64_t* size_out);
// -EINVAL if key_size is not the size the buffer was allocated for.
DLLOUT int   uDepotPutBuf(void* kv, const uint8_t key[], uint32_t key_size,
                           void* put_buf);
DLLOUT void  uDepotFreePutBuffer(void* put_buf);

// A GetBuffer able to hold a record of a key of key_size bytes and a value
// of up to val_size bytes, or NULL as for uDepotAllocPutBuffer.
DLLOUT void* uDepotAllocGetBuffer(void* kv, uint32_t key_size,
                                  uint64_t val_size);
// A GetBuffer with no memory yet: uDepotGetBuf allocates it.
DLLOUT void* uDepotNewGetBuffer(void);
// Reads into the buffer if it is large enough, else into a new one that
// frees the old (so a pointer from uDepotGetBufferValue is invalid after a
// get). On error the value is empty and the buffer is kept.
DLLOUT int   uDepotGetBuf(void* kv, const uint8_t key[], uint32_t key_size,
                           void* get_buf);
// The value of the last uDepotGetBuf: read-only, *size_out bytes (NULL
// when there is none).
DLLOUT const uint8_t* uDepotGetBufferValue(void* get_buf,
                                           uint64_t* size_out);
DLLOUT void  uDepotFreeGetBuffer(void* get_buf);

// uDepot::thread_value_copies() of the calling thread: the values its
// operations copied between caller memory and uDepot's buffers.
DLLOUT uint64_t uDepotThreadValueCopies(void);

}

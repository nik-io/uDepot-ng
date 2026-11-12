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
// As uDepotOpenTables, with queue_depth I/Os in flight at once per thread
// (StoreConfig::queue_depth: the size of each thread's AIO context), or 0
// for the default (1024). uDepotMultiGetBuf keeps at most that many gets in
// flight, fewer if the calling thread's AIO context has less room
// (AioIO::thread_queue_room()).
DLLOUT void* uDepotOpenQueue(const char* fname, uint64_t size,
                             int force_destroy, uint64_t table_bytes,
                             uint32_t queue_depth);
// As uDepotOpenQueue, with grains of grain_bytes (StoreConfig::grain_size:
// the unit of allocation and I/O; a record of up to 2047 grains is stored,
// so 4096, uDepotOpenQueue's, holds values up to ~8 MiB and 32768 ~64 MiB):
// a power of two from 512 to 1 MiB (a record of 2047 grains is one write,
// which Linux caps at 2147479552 bytes), and a multiple of the O_DIRECT
// alignment of the backing where that can be told (a block device's sector
// size; an existing file's statx dio_offset_align), else NULL with a message
// on stderr. A store is reopened with the grain it was created with; another
// fails the open (NULL, the store's message on stderr) as long as the file
// was not formatted by force_destroy at a smaller grain or size since: see
// "Grain size" in docs/architecture.md.
DLLOUT void* uDepotOpenGrain(const char* fname, uint64_t size,
                             int force_destroy, uint64_t table_bytes,
                             uint32_t queue_depth, uint32_t grain_bytes);
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
// The value of the last uDepotGetBuf: *size_out bytes (NULL when there is
// none). The memory is the buffer's own, private to the caller until the
// next uDepotGetBuf on it, so the caller may write into it (the store keeps
// no reference to it); the const only says the store does not.
DLLOUT const uint8_t* uDepotGetBufferValue(void* get_buf,
                                           uint64_t* size_out);
DLLOUT void  uDepotFreeGetBuffer(void* get_buf);
// uDepotGetBuf for n keys on the calling thread, in one call: keys[i] (of
// key_sizes[i] bytes) is read into get_bufs[i] and results[i] set to its rc
// (0, or -errno: -ENOENT for a missing key). The gets run as coroutines on
// this thread, no more than the thread's AIO context takes (at most the store's
// queue depth, uDepotOpenQueue; less if another store's open sized the
// thread's context smaller) in flight at once, a new one starting as each completes: no threads, no
// locks. Returns 0, or -ENOMEM (nothing started) if it cannot allocate; on 0
// every results[i] has been set. The get_bufs must be distinct (one buffer
// serves one get; the Python wrapper refuses a repeat), and everything
// passed stays alive and untouched until it returns.
DLLOUT int   uDepotMultiGetBuf(void* kv, uint32_t n,
                               const uint8_t* const keys[],
                               const uint32_t key_sizes[],
                               void* const get_bufs[], int results[]);

// uDepot::thread_value_copies() of the calling thread: the values its
// operations copied between caller memory and uDepot's buffers.
DLLOUT uint64_t uDepotThreadValueCopies(void);

}

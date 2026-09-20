# uDepot-ng Architecture

High-performance NVMe key-value store. Rewrite of
[uDepot](https://www.usenix.org/system/files/fast19-kourtis.pdf) with a
modernized concurrency model, simplified runtime, and C++23 throughout.

## Design Principles

Inherited from uDepot — non-negotiable.

1. **Zero copy** (on by default). User buffers reach storage without
   intermediate copies. DMA buffers (`rte_malloc`) only at the SPDK boundary
   where hardware requires them.
2. **No global locking**. Per-thread, per-table, or lock-free. The directory
   is protected by userspace RCU; individual hash-table neighborhoods use
   stripe locks for writes and are lock-free for reads.
3. **Minimal amplification**. No indirection layers, journaling, or metadata
   overhead beyond what the log-structured allocator needs.
4. **Enterprise-grade crash recovery only**. Either the recovery path is
   correct and complete, or it does not exist.

## Language and Style

- **C++23** (`-std=c++23`), compiled with `-Wall -Wextra -Werror`.
- **Google C++ Style Guide** throughout, with these project conventions:
  - Member variables: `_` suffix (e.g., `directory_`, `epoch_`).
  - `snake_case` for functions and variables, `PascalCase` for types/classes.
  - `#pragma once` for all headers.
  - No exceptions on the I/O hot path.

## Major Architectural Changes (vs. uDepot)

### 1. RCU-Protected Hash Directory

The hash directory (mapping from hash prefix to hash table) is protected by a
hand-rolled, epoch-based userspace RCU with per-thread thread-local state.

**Why**: The original uDepot drains all readers before a grow operation can
proceed (a write lock on a `BRLock` with 128 per-thread RWLocks), stalling the
entire store. RCU eliminates this: readers never block, and the writer defers
reclamation of the old directory until a grace period elapses.

**Per-thread RCU state** (one cache line per registered thread):

```cpp
struct alignas(64) RcuThreadState {
    std::atomic<uint64_t> epoch;  // odd = active, even/0 = quiescent
    uint32_t nesting;             // concurrent coroutines on same thread
};
```

**Reader path** (zero shared-line atomic RMW):

```cpp
void rcu_read_lock() {
    if (tls_rcu->nesting++ == 0)
        tls_rcu->epoch.store(global_epoch_.load(std::memory_order_relaxed) | 1,
                             std::memory_order_release);
}

void rcu_read_unlock() {
    if (--tls_rcu->nesting == 0)
        tls_rcu->epoch.store(0, std::memory_order_release);
}
```

Cost: two thread-local stores, one global load (read-shared, rarely written).

**Writer path** (grow):

```cpp
void synchronize_rcu() {
    uint64_t target = global_epoch_.fetch_add(2, std::memory_order_acq_rel);
    for (auto& ts : all_thread_states_) {
        while (true) {
            uint64_t e = ts.epoch.load(std::memory_order_acquire);
            if (e == 0 || e > target) break;  // quiescent or past target
            // spin/yield
        }
    }
}
```

**RCU critical section spans the entire KV operation**, including I/O. This is
correct: the grace period is bounded by the slowest in-flight I/O (milliseconds
on NVMe), and grows are rare. This means **no separate table refcounting** —
RCU alone guarantees that no reader references a freed table.

Nesting handles multiple coroutines on the same thread: the thread goes
quiescent only when all coroutines have exited their critical sections.

### 2. Lock-Free Reads, Stripe-Locked Writes

Hash tables use hopscotch hashing with 8-byte `HashEntry` values (fits in one
atomic load on x86-64).

**Lock-free get path**: scan the neighborhood bitmap, load candidate entries
atomically, check tags, go to disk for key confirmation. No locks acquired.

**Stripe-locked put/del path**: 1024 stripe locks per table. The hopscotch
displacement chain modifies multiple entries under the stripe lock, with stores
ordered so that concurrent readers always see a valid state:

- **Insert**: write entry at target slot (`store(entry, release)`), then set
  bitmap bit (`store(bitmap, release)`).
- **Delete**: clear bitmap bit first, then clear entry.
- **Displace**: copy entry to new slot, set new bitmap bit, clear old bitmap
  bit. Entry visible in at least one position at all times.

Worst case for a concurrent reader: an unnecessary disk read (false positive
from stale bitmap) or missing an entry mid-displacement (but it is still
visible at the old position). Both are safe.

### 3. Eager-Start C++23 Coroutines

All API operations return an eagerly-started coroutine. The coroutine body
runs immediately up to its first suspension point (the I/O submit), then
control returns to the caller with a handle they can `co_await` later.

```cpp
auto op1 = store.get(key1, buf1);  // submits I/O, returns immediately
auto op2 = store.get(key2, buf2);  // submits I/O, returns immediately
// both in flight concurrently
auto r1 = co_await op1;            // waits for completion
auto r2 = co_await op2;
```

For the synchronous backend (pread/pwrite), the coroutine never suspends —
the I/O completes inline and `co_return`s, so `co_await store.get(...)` is
semantically a blocking call with zero overhead.

For non-coroutine callers (e.g., pyudepot.cc), `run_sync()` drives the
coroutine to completion synchronously.

```cpp
// CoroTask with eager start
struct CoroTask {
    struct promise_type {
        std::suspend_never initial_suspend() noexcept { return {}; }
        FinalAwaitable final_suspend() noexcept { return {}; }
        // ...
    };
    // [[nodiscard]]
};
```

### 4. Simplified Runtime — No Separate TRT

The original TRT (Task Runtime) provided a cooperative scheduler, task
spawn/yield/wait primitives, futures/waitsets, page-fault rollback, and I/O
backend pollers. C++23 coroutines replace everything except the pollers.

What remains lives in `io/`:

- **Poller loop** per async backend (aio, uring, spdk): runs on a dedicated
  thread, harvests completions, resumes the coroutine handles that completed.
- **Awaitable** that bridges I/O completion to coroutine resume.
- **Buffer allocation** utilities (tagged by backend).

No scheduler, no task types, no run queues, no futures/waitsets, no
page-fault rollback, no cross-core task migration.

### 5. Single KV Implementation

The original uDepot had `KV` (pure virtual) → `uDepot<RT>` (template on
runtime type bundle) → `uDepotSalsa<RT>` (the only concrete implementation),
plus 10+ runtime type bundles cross-producting I/O × Lock × Sched × Net ×
RwpfTy.

uDepot-ng has one class, parameterized only on the I/O backend:

```cpp
template <typename IO>
class UDepot {
    Directory directory_;
    IO io_;
    SegmentAllocator segments_;

    CoroTask get(std::span<const uint8_t> key, IoBuffer& val_out);
    CoroTask put(std::span<const uint8_t> key, std::span<const uint8_t> val);
    CoroTask del(std::span<const uint8_t> key);
    CoroTask exists(std::span<const uint8_t> key, size_t* val_size);
};
```

A factory function selects the backend from a config enum and returns a
type-erased handle (one virtual dispatch at construction, none on the hot
path).

### 6. I/O Backends

Six backends, all implementing the same `IoBackend` concept:

| Backend | Completion model | Poller | Buffer | Notes |
|---|---|---|---|---|
| `PosixIO` | Synchronous | No | aligned malloc | pread/pwrite, never suspends |
| `AioIO` | Async (io_getevents) | Yes | aligned malloc | Linux AIO |
| `UringIO` | Async (CQ ring) | Yes | aligned malloc | io_uring |
| `SpdkIO` | Async (qpair) | Yes | rte_malloc (DMA) | SPDK/NVMe, DPDK lcores |
| `NetIO` | Async (epoll or uring) | Yes | malloc | Socket I/O for memcache server |
| `ODirectIO` | Synchronous | No | aligned malloc | pread/pwrite with O_DIRECT |

Common interface:

```cpp
concept IoBackend = requires(T io, void* buf, size_t n, off_t off) {
    { io.pread(buf, n, off) } -> std::same_as<CoroTask>;
    { io.pwrite(buf, n, off) } -> std::same_as<CoroTask>;
    { io.alloc_buffer(n) } -> std::same_as<IoBuffer>;
    { io.free_buffer(std::declval<IoBuffer&>()) } -> std::same_as<void>;
};
```

### 7. Simplified Buffer (Replacing Mbuff)

The original Mbuff was a linked list of buffer nodes with prepend/append,
inline object cache, type-index safety, and iterators. Designed for zero-copy
network framing (prepend protocol headers without copying the payload).

uDepot-ng replaces it with a contiguous buffer:

```cpp
struct IoBuffer {
    void* data;
    size_t length;
    size_t capacity;
    Deallocator dealloc;  // free / aligned_free / rte_free
};
```

Zero-copy works the same way:
- **put**: caller passes their buffer → backend writes from it directly.
- **get**: caller passes a pre-allocated buffer → backend reads into it.
- **SPDK path**: `store.alloc_buffer()` returns a DMA-safe buffer.

For the memcache server, SET receives the value into an `IoBuffer` from the
storage backend's allocator and writes it directly; GET reads into an
`IoBuffer` and sends it over the network. No linked list needed.

### 8. Network Backend and Memcache Server

The network backend (`NetIO`) handles socket I/O for the memcache-compatible
protocol server. It sits alongside the storage backends as another
`IoBackend` for accept/recv/send operations. The memcache protocol handler is
a layer above that uses both the network backend (for the connection) and the
storage backend (for the data).

Remote access for flywheel is planned — the memcache protocol provides a
ready-made wire format, and the zero-copy path through the protocol handler
(recv into storage-compatible buffer, send from storage buffer) preserves the
no-copy property end to end.

## Directory Layout

```
uDepot-ng/
├── docs/
│   └── architecture.md          # this document
├── src/udepot/
│   ├── store.h                  # UDepot<IO> — the single KV implementation
│   ├── store.cc
│   ├── directory.h              # RCU-protected hash directory
│   ├── directory.cc
│   ├── hash_table.h             # hopscotch table (lock-free reads)
│   ├── hash_table.cc
│   ├── hash_entry.h             # 8-byte packed hash entry
│   ├── rcu.h                    # per-thread epoch-based userspace RCU
│   ├── rcu.cc
│   ├── buffer.h                 # IoBuffer, allocator tags
│   ├── coro.h                   # CoroTask (eager start, symmetric transfer)
│   └── io/
│       ├── backend.h            # IoBackend concept
│       ├── posix.h              # pread/pwrite (sync)
│       ├── o_direct.h           # pread/pwrite with O_DIRECT (sync)
│       ├── aio.h                # Linux AIO + poller
│       ├── aio.cc
│       ├── uring.h              # io_uring + poller
│       ├── uring.cc
│       ├── spdk.h               # SPDK/NVMe + poller, DMA buffers
│       ├── spdk.cc
│       ├── net.h                # socket I/O (epoll or io_uring)
│       ├── net.cc
│       └── poller.h             # shared poller infrastructure
├── src/net/
│   └── memcache.cc              # memcache protocol server
├── python/
│   ├── wrapper/
│   │   ├── pyudepot.h           # C ABI (7 functions)
│   │   └── pyudepot.cc          # bridges C ABI to UDepot via run_sync()
│   └── pyudepot/
│       ├── __init__.py
│       └── udepot.py            # ctypes bindings (sync API)
├── test/
│   ├── rcu_test.cc
│   ├── hash_table_test.cc
│   ├── directory_test.cc
│   ├── store_test.cc
│   ├── io/
│   │   ├── posix_test.cc
│   │   ├── aio_test.cc
│   │   ├── uring_test.cc
│   │   └── spdk_test.cc
│   └── perf/
│       └── zerocopy_bench.cc    # zero-copy vs copy invariant
├── scripts/
│   └── perf-zerocopy.sh
├── CMakeLists.txt               # CMake build (replaces Make)
├── CLAUDE.md
└── LICENSE
```

## Build System

CMake replaces the hand-written Makefiles. Feature flags:

```cmake
option(UDEPOT_BUILD_SPDK   "Build SPDK backend"           OFF)
option(UDEPOT_BUILD_URING  "Build io_uring backend"        ON)
option(UDEPOT_BUILD_AIO    "Build Linux AIO backend"       ON)
option(UDEPOT_BUILD_NET    "Build network/memcache server"  ON)
option(UDEPOT_BUILD_PYTHON "Build pyudepot shared library"  ON)
option(UDEPOT_BUILD_TESTS  "Build test suite"               ON)
```

ccache is picked up automatically when present (same as the original).

## Python Bindings (pyudepot)

The C ABI stays synchronous for the initial version — each call blocks via
`run_sync()`. The Python `uDepot` class exposes the same 4-method surface
flywheel uses today: `get`, `put`, `delete`, `exists`.

An async Python API (asyncio-compatible) is planned as a follow-up. Not
urgent — flywheel's `ThreadPoolExecutor` with GIL release works well with
the sync API.

## On-Disk Format

The on-disk format (segment layout, directory table layout, salsa metadata)
is **unchanged** from uDepot. A uDepot-ng store can read a uDepot store and
vice versa — the rewrite changes the in-memory concurrency model and runtime,
not the persistent format.

## Implementation Order

1. **`coro.h`** — CoroTask with eager start, `run_sync()`.
2. **`rcu.h`** — per-thread epoch-based RCU.
3. **`buffer.h`** — IoBuffer with allocator tags.
4. **`io/posix.h`** — synchronous pread/pwrite backend.
5. **`hash_entry.h`, `hash_table.h`** — hopscotch table with lock-free
   reads and stripe-locked writes.
6. **`directory.h`** — RCU-protected directory of hash tables.
7. **`segment.h`** — segment geometry and salsa GC.
8. **`store.h`** — `UDepot<PosixIO>`, the first working configuration.
9. **Tests** for each layer as it is built.
10. **`io/uring.h`**, **`io/aio.h`** — async backends with pollers.
11. **`io/spdk.h`** — SPDK backend.
12. **`python/`** — pyudepot bindings.
13. **`io/net.h`**, **`net/memcache.cc`** — network backend and protocol
    server.

The first milestone is `UDepot<PosixIO>` passing the existing test suite —
RCU directory, lock-free gets, hopscotch table, sync I/O. Everything after
that adds backends and protocol support.

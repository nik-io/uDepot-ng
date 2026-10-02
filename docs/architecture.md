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
hand-rolled userspace RCU with per-thread counters (the sleepable-RCU
algorithm).

**Why**: The original uDepot drains all readers before a grow operation can
proceed (a write lock on a `BRLock` with 128 per-thread RWLocks), stalling the
entire store. RCU eliminates this: readers never block, and the writer defers
reclamation of the old directory until a grace period elapses.

**Per-thread slot** (one cache line, written only by the thread that owns it):

```cpp
struct alignas(64) Slot {
    std::atomic<uint64_t> lock[2];    // sections entered, per index
    std::atomic<uint64_t> unlock[2];  // sections left, per index
};
```

**Reader path** (zero shared-line atomic RMW, no shared-line writes):

```cpp
uint32_t read_lock() {
    Slot& s = this_thread_slot();
    uint32_t idx = gp_idx_.load(relaxed) & 1;
    s.lock[idx].store(s.lock[idx].load(relaxed) + 1, relaxed);
    compiler_barrier();   // full fence where membarrier is unavailable
    return idx;
}

void read_unlock(uint32_t idx) {   // may run on a different thread
    Slot& s = this_thread_slot();
    s.unlock[idx].store(s.unlock[idx].load(relaxed) + 1, release);
}
```

A section is counted in on the thread that enters it and counted out on the
thread that leaves it. They differ routinely: a coroutine starts on its
caller's thread and finishes on an I/O backend's poller. Because only the
sums matter, that is correct by construction, and each bump is a plain load
and store on the bumping thread's own slot.

**Writer path** (grow, close):

```cpp
void synchronize() {
    heavy_barrier();
    wait_until_drained(other index);   // stragglers from the previous flip
    flip gp_idx_;
    heavy_barrier();
    wait_until_drained(old index);     // sum(unlock) == sum(lock), all slots
    heavy_barrier();
}
```

**Ordering.** A reader's lock bump must be visible before its protected loads
(a StoreLoad ordering, which x86 and arm64 both relax). On Linux (x86-64 and
arm64) `heavy_barrier()` is `membarrier(MEMBARRIER_CMD_PRIVATE_EXPEDITED)`,
which executes a full barrier on every running thread, so readers need only a
compiler barrier, the trade the Linux kernel's `srcu_read_lock_lite` makes with
`synchronize_rcu()`. On macOS, and on kernels without membarrier, readers issue
a full fence instead (`dmb ish` on arm64). `UDEPOT_RCU_READER_FENCE=1` forces
that path; CTest runs `rcu_test` both ways.

Slots are claimed per thread on first use and released when the thread exits.
Counts only grow, so a reused slot keeps counting. Beyond 256 concurrent
threads, the rest share one slot updated with atomic RMW: slower, still
correct.

**RCU critical section spans the entire KV operation**, including I/O. This is
correct: the grace period is bounded by the slowest in-flight I/O (milliseconds
on NVMe). This means **no separate table refcounting** — RCU alone guarantees
that no reader references a freed table, and that GC does not reuse a segment
a reader may still be reading (GC waits a grace period before handing a
segment back). `close()` uses the same grace period to wait for operations in
flight. The exceptions are waiting for free space (see *Space management*)
and waiting for a directory grow: an operation leaves its section while it
waits, so it can never hold up the grace period that GC or the grow needs to
let it continue.

`Rcu::call(fn)` (call_rcu) runs `fn` after a grace period on a reclaimer
thread, batching callbacks behind one `synchronize()`; `Rcu::barrier()` waits
for those queued. The directory frees replaced snapshots this way.

### 2. Lock-Free Reads, Stripe-Locked Writes

Hash tables use hopscotch hashing with 8-byte `HashEntry` values (one atomic
load): a 5-bit neighborhood offset, an 8-bit tag, an 11-bit size in grains and
a 40-bit pba. As in uDepot, a slot is **unused** when its pba is all ones, and
an entry with size 0 is **deleted**: it keeps pointing at the key's tombstone,
so a later write is still ordered against the delete, and recovery and GC can
find the tombstone.

**Lock-free get path**: scan the bucket's 32-slot neighborhood, load entries
atomically, check tags, go to disk to confirm the key. No locks acquired.

**Stripe-locked put/del path** (uDepot's `uDepotMap`): at most 1024 stripe
locks per table. A write for a bucket searches for a free slot up to
`kMaxDisplace = 64` neighborhoods past it and moves entries forward to bring
that slot into the neighborhood, so it touches at most
`[bucket - 32, bucket + 32 * 65)`. Stripes are at least that long (the stripe
count is halved until they are), so a write takes one or two adjacent stripe
locks, in ascending order — no global lock, no deadlock.

Entries only ever move to a higher slot, written there before they are
cleared from the lower one, so a reader scanning upward sees each entry in at
least one position. The worst case for a concurrent reader is an extra disk
read for a tag match.

A put is lookup-before-write, as in uDepot: the key-verify reads run
unlocked, then the stripe lock is taken, the decision is re-checked against
the entries verified, and the directory is updated in the same critical
section. As in uDepot's `is_pba_order_equal_to_total_order`, a write only
replaces an entry (live or deleted) that is older in recovery order — segment
timestamp, then grain — otherwise it is rewritten, so recovery always
reproduces the order writes were acknowledged in.

**Directory.** As in uDepot's `hash_to_map`, a key's table is chosen by the
top bits of its tag and its bucket by the low bits of its hash. `grow()`
doubles the number of tables. Legacy excluded every operation for the grow
(`rwpflock.write_enter()` + `write_wait_readers()`); uDepot-ng keeps that for
writers only, using RCU:

1. Set the snapshot's `frozen` flag. A writer checks it under its stripe
   lock, inside its read section, and backs off if set.
2. `synchronize()`: every writer that missed the flag has finished, so its
   write is in the old tables, and none can start.
3. Copy live and deleted entries into the new tables, holding no lock.
4. Publish the new snapshot and wake the writers that backed off; they
   retry on it.
5. Free the old snapshot with `Rcu::call()` (call_rcu): once a grace period
   has passed, on the RCU reclaimer thread, off the grow path.

Readers never block: they keep reading the frozen tables during the copy.
Grow is rare, so ordinary writes pay only a flag load under their stripe lock.
Because `grow()` waits for a grace period it never runs inside a read section
or on an I/O poller thread (which in-flight reads may be waiting on): a
writer that finds its table full leaves its section and suspends, and the
store's waker thread runs the grow.

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

- **Completion polling** per async backend. AIO and io_uring run a poller
  thread that harvests completions and resumes the coroutines that completed.
  SPDK follows uDepot's TRT model instead: each thread has its own queue pair
  (`SpdkQpair`, no shared state), submits and suspends, and its own poll —
  driven by `run_sync()` through a thread-local hook, the way a TRT scheduler
  polled between tasks — harvests completions and resumes the coroutines.
  One thread per `SpdkIO` polls the controllers' admin queues (~10x/s), which
  keeps NVMe-oF keep-alives flowing.
- **Awaitable** that bridges I/O completion to coroutine resume.
- **Buffer allocation** utilities (tagged by backend).

`spdk_env_init` gets the process's cpu affinity as its core mask, as uDepot's
`spdk_init` did, so the EAL never claims a core outside it (by default it
takes core 0, which a co-located `nvmf_tgt` reactor may be spinning on — a
10 ms stall per I/O). The calling thread's affinity is restored afterwards:
uDepot's TRT pinned the threads it owned, but uDepot-ng runs on its caller's.

Where uDepot's tasks yielded to the TRT scheduler — waiting for salsa to stage
a segment — uDepot-ng suspends the coroutine on a waiter list and a waker
thread resumes it to retry (see *Space management*), so a poller thread is
never blocked.

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

### 9. Space Management (unchanged from uDepot)

Grains come from salsa, configured as uDepot's non-memcache store: relocating
GC (`init_local(14, 2, 4)`), 20% overprovisioning, one user and one relocation
stream.

- **Release.** Every allocation is released once its write is committed or
  invalidated; salsa seals a segment, making it a GC candidate, only when all
  its grains are released.
- **Segment metadata** is written in salsa's allocation callback, before any
  grain of the segment is handed out (uDepot's `persist_seg_md`). The
  callback can run on any thread, including an I/O poller, so it uses the
  backend's blocking `pwrite_sync`.
- **Waiting for space.** Allocation never blocks: when no segment is staged
  it returns `EAGAIN`, and the operation waits as described in §4, outside its
  read-side section.
- **GC** reads the victim segment in one pass (uDepot maps it), relocates every
  record the directory still points at, and waits a grace period before the
  segment is reused. A tombstone is dropped only when no live segment older
  than the victim remains — otherwise an older copy of its key could
  resurface on recovery — and is relocated like any record until then.
- **Recovery** reads each segment with valid metadata in one pass and replays
  its records; for each key the newest in recovery order (segment timestamp,
  then grain) wins, tombstones included. Grains holding no valid record are
  invalidated.

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
│   ├── rcu.h                    # per-thread-counter userspace RCU
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

**No thread-per-get.** Flywheel's old `ThreadPoolExecutor.submit(self.get, …)`
per key in `multi_get` is exactly the overhead this rewrite eliminates. All I/O
concurrency comes from the eager-start coroutine model — one thread, zero thread
creation.

The C ABI exposes both single-key and batch entry points:

```c
int udepot_get(udepot_t h, const void* key, uint32_t klen,
               void* val, uint32_t vlen, uint32_t* val_size);

int udepot_multi_get(udepot_t h, uint32_t n,
                     const udepot_iov* keys,
                     udepot_iov* vals,
                     int* results);
```

`udepot_multi_get` launches N eager-start coroutines — each submits its I/O
immediately — then drives the poller until all complete. On an async backend
(uring, SPDK) this is true I/O concurrency from a single call, single thread.
On the sync backend (posix) the coroutines never suspend, so it degrades to
sequential I/O — correct but not concurrent, same as today.

The Python `uDepot` class exposes `get`, `put`, `delete`, `exists`, and
`multi_get`. `multi_get` is a single ctypes call into `udepot_multi_get`, not a
Python-side thread pool.

An async Python API (asyncio-compatible, mapping each coroutine to an asyncio
future) is planned as a follow-up once the async backends are working.

## On-Disk Format

The on-disk format (segment layout, directory table layout, salsa metadata)
is **unchanged** from uDepot. A uDepot-ng store can read a uDepot store and
vice versa — the rewrite changes the in-memory concurrency model and runtime,
not the persistent format.

## Implementation Order

1. **`coro.h`** — CoroTask with eager start, `run_sync()`.
2. **`rcu.h`** — per-thread-counter RCU.
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
RCU directory, lock-free gets, hopscotch table, sync I/O. **v0 is not
complete until the perf regression test passes**: uDepot-ng must be strictly
equal to or faster than uDepot on every operation (put, get, exists, delete),
measured head-to-head in the same run. Everything after that adds backends and
protocol support.

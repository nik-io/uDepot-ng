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
segment back). As in uDepot, the caller orders `close()` after every
operation: none may race or follow it, so the store keeps no open flag on the
operation path. The exceptions are waiting for free space (see *Space management*)
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

A put writes its record first and then checks, as the paper (§4.5) and
uDepot do: the record goes to the log, the key-verify reads run unlocked,
then the stripe lock is taken, the decision is re-checked against the
entries verified, and the directory is updated in the same critical section.
A put that is rejected there (a conditional put, or a lost race) invalidates
its grains, but its record stays in the log: after a crash the log scan can
bring it back, which the paper accepts; after a clean shutdown the persisted
index decides, and it stays dead. As in uDepot's `is_pba_order_equal_to_total_order`, a write only
replaces an entry (live or deleted) that is older in recovery order — segment
timestamp, then grain — otherwise it is rewritten, so recovery always
reproduces the order writes were acknowledged in.

**Tag.** As in uDepot (`uDepotMap::hash_to_keytag_`), an entry's 8-bit tag
is the 8 hash bits just above its bucket's, so the bucket and the tag
together are the hash's low `index_bits + 8` bits: the paper's key
fingerprint. Until PR #9 the tag was the hash's top 8 bits, statistically
no different, but not uDepot's layout. A persisted table records its format
in the index header and footer (`format`, 1 for uDepot's tag); an index of
any other format is not restored, and the log scan rebuilds it.

**Directory.** As in uDepot's `hash_to_map`, a key's table is chosen by the
top bits of its tag and its bucket by the low bits of its hash. (The paper
says the tag's LSBs index the directory; uDepot's code, `hash_to_dir_idx_`,
takes its MSBs, and uDepot-ng follows the code.) `grow()`
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
or on a thread whose own I/O in-flight reads may be waiting on: a
writer that finds its table full leaves its section and suspends, and the
store's waker thread runs the grow.

Writers stall for the whole copy, which grows with the index (tens of ms
from 16 to 32 tables of 2^17 slots). The paper's incremental resize (§4.3)
would spread it over the writes; it was implemented and measured, and
dropped (`docs/udepot-paper.md`, §4.3). `udepot_ng_bench --latency --rate
--qd` measures put latency through grows (a small `--segment-size`, so small
tables, forces them).

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

- **Completion polling** per async backend, as uDepot's TRT: each thread
  has its own queue — an SPDK queue pair (`SpdkQpair`), an AIO context, an
  io_uring ring, as TRT kept a `thread_local` `AioState` and `UringState`
  per scheduler thread — shared by every store the thread uses, with no
  shared state between threads. A thread submits and suspends, and its own
  poll — driven by `run_sync()` through thread-local hooks (one per backend
  it has used), the way a TRT scheduler polled between tasks — harvests the
  completions and resumes the coroutines, so a coroutine stays on the thread
  that submitted its I/O. io_uring queues SQEs and pushes them in batches of
  up to 128, or at the thread's next poll, as uDepot's `UringState` does;
  a ring smaller than that fills first, and the next I/O pushes it rather
  than being refused. So an operation completes only when the thread that
  started it polls (waiting in `run_sync()`, or driving
  `poll_this_thread()`): waiting on it from another thread finishes only
  once the starting thread polls, as a TRT task finishes on its own
  scheduler thread. `store.h` states this on the API. A thread that
  resumes operations it did not start (the space waker, a
  memcache worker's epoll poller) polls its own queue too: their I/O is
  now its own. One thread per `SpdkIO` polls the controllers' admin queues
  (~10x/s), which keeps NVMe-oF keep-alives flowing.

  AIO and io_uring used to hand every completion from a poller thread to the
  waiting thread instead. Measured against uDepot (v0 gate, below), that
  made them 15–87% slower than uDepot; reaped on the submitting thread they
  are faster on every operation.

  An io_uring poll that finds nothing completed spins by default, as
  uDepot's poller does (it only yields to the TRT scheduler), for the
  lowest latency. `StoreConfig::poll_yields` (process-wide, as rings are
  per thread) makes it yield the CPU (`sched_yield`) instead. That is for
  I/O the kernel cannot do inline (every request on tmpfs, buffered
  writes, a device without nowait): it runs on an io-wq worker, which the
  scheduler wakes on the submitting thread's own CPU (traced: every
  wakeup), and a spinning thread holds it off until preemption. Measured
  on tmpfs, yielding once roughly doubled io_uring throughput on a 4-CPU
  Intel VM at queue depth 1, then made no consistent difference there
  (24-32 kops/s spinning, 24-28 yielding, 4 KiB values), and changed little
  on a CI EPYC 7763: measure before turning it on. AIO needs neither:
  `io_submit` does that work before returning.
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
class uDepot {
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
| `AioIO` | Async (per-thread context) | Submitting thread | aligned malloc | Linux AIO |
| `UringIO` | Async (per-thread ring) | Submitting thread | aligned malloc | io_uring |
| `SpdkIO` | Async (per-thread qpair) | Submitting thread | rte_malloc (DMA) | SPDK/NVMe, DPDK lcores |
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

**Queue depth.** The intended use: preallocate zero-copy buffers for the
depth you choose (`alloc_put_buffer`, `alloc_get_buffer`), issue that many
operations, wait for them, reuse the buffers. uDepot allocates no data
buffers for depth. `StoreConfig::queue_depth` sizes each backend's
per-thread queue for it: the opening thread's AIO context (`io_setup`) or
io_uring ring (recreated if it is idle), and the contexts, rings and SPDK
request pools of queue pairs that threads create afterwards; a thread's
busy queue keeps its size.

Per-thread queues cost kernel resources per thread, as in uDepot: an AIO
context reserves its depth against the system-wide `fs.aio-max-nr` (65536
by default, so 64 contexts of the default 1024 can exist at once across
the system). A thread that cannot get a context fails its I/O with
`-EAGAIN`. Many threads doing AIO at once need a smaller `queue_depth` or a
larger `aio-max-nr`; the memcache server takes one context per worker thread
(`MemcacheServer::Config::workers`), however many connections it serves. SPDK's own pool size
stays the minimum: a large I/O takes several requests (one per transfer-size
piece), so a pool sized to a small depth could never take it. Nothing in
uDepot caps the depth below what the backend takes. A depth the backend
cannot set up at all makes `open()` fail with `-EINVAL`.

When the backend cannot take another I/O, the operation fails with
`-EAGAIN`, on every backend, as uDepot failed it: AIO's `io_submit`
refuses (EAGAIN), SPDK has no free request (ENOMEM), or io_uring has no
free submission entry (only while the kernel refuses submissions; it
otherwise takes I/O past the ring's size, keeping the extra completions on
its overflow list). The caller can retry it once some of its outstanding
operations complete. A put or del refused after it wrote its record ends
like any failure after the write (paper section 4.5, "PUT writes before it
checks"): the directory is unchanged, so a clean restart, which restores
the index, never sees the record; but it stays in the log, and after a
crash the log scan restores it if it is the key's newest. Nothing waits or
spins for room: on EBUSY/EAGAIN from `io_uring_submit` the SQEs, already
published, are left for the thread's next poll to push after it reaps,
since the submitter may be that poll itself.

Only the caller's operations report `-EAGAIN`. uDepot's own synchronous
I/O (recovery, index persistence, GC) shares the queue, so it retries a
refused submission once some complete (`run_internal`), and SPDK's
`pwrite_sync` polls its queue pair and retries.

`QueueDepthTest` (`test/queue_depth_tests.h`, run per backend) sizes the
backend for 4, puts thousands of puts, gets and exists in flight while
nothing is reaped, requires each to succeed with its data or fail with
exactly `-EAGAIN` (AIO and SPDK must refuse some), and retries until all
are done. `LargeValuesAtTheSmallestQueueDepth` puts and gets values larger
than one transfer with a queue depth of 1.

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

Zero copy is uDepot's Mbuff interface, with the record laid out in one
contiguous buffer instead of a chain of nodes:

- **put**: `alloc_put_buffer(key_size, val_size)` returns a `PutBuffer`
  shaped like the on-disk record. The caller writes the value into
  `value()`, and `put(key, PutBuffer&)` fills in the header, key and checksum
  around it and writes the buffer as is. On SPDK the buffer is DMA memory.
- **get**: `get(key, GetBuffer*)` reads the record into the `GetBuffer`'s
  own buffer when it is large enough (`alloc_get_buffer(key_size,
  val_size)` preallocates one), and otherwise into a new one that replaces
  it; `value()` views the value inside it. A caller reusing its buffers
  does no allocation per get. (uDepot's Mbuff was likewise the caller's.)
- The span-based `put`/`get` copy the value into, or out of, such a record
  buffer. Both interfaces run the same coroutine (`put_record` and
  `get_record`), so they differ only in that copy. `scripts/perf-zerocopy.sh`
  gates on it: zero copy must not be slower, on any backend.

The memcache server and the JNI binding still use the copying interface;
the Python binding offers both (see "Python Bindings").

### 8. Network Backend and Memcache Server

The network backend (`NetIO`) handles socket I/O for the memcache-compatible
protocol server. It sits alongside the storage backends as another
`IoBackend` for accept/recv/send operations. The memcache protocol handler is
a layer above that uses both the network backend (for the connection) and the
storage backend (for the data).

The server is one accept thread and a fixed pool of worker threads
(`Config::workers`, default one per hardware thread). Each connection is a
coroutine, started and resumed on one worker, which runs that worker's socket
epoll and its store I/O completions side by side, as one of uDepot's TRT
scheduler threads does for its connection tasks (`trt-memcache.cc`
`task_mc_accept`). The accept thread hands each new fd to the next worker
round robin down a per-worker socketpair that the worker's epoll watches, so
the hand-off takes no userspace lock. Threads, and with them AIO contexts and
io_uring rings, are bounded by the pool, and a worker has the I/Os of all its
connections in flight at once on its one context.

Remote access for flywheel is planned — the memcache protocol provides a
ready-made wire format, and the zero-copy path through the protocol handler
(recv into storage-compatible buffer, send from storage buffer) preserves the
no-copy property end to end.

### 9. Space Management (unchanged from uDepot)

Grains come from salsa, configured as uDepot's non-memcache store: relocating
GC (`init_local(14, 2, 4)`), 20% overprovisioning, one user and one relocation
stream. (A memcache store differs: see below.)

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
  segment is reused.
  - **A relocated record goes to a segment newer than its victim.** Recovery
    orders a key's records by segment timestamp, and every older copy of the
    key is older than the victim's. The relocation segment stays open while
    the data stream moves on, so it can be older than the victim; GC then
    closes it and takes a fresh, newest one. uDepot does not check this.
  - **A tombstone is dropped only when no segment older than the victim may
    still hold records on disk** — otherwise an older copy of its key could
    resurface on recovery — and is relocated like any record until then. A
    segment GC has reclaimed still counts until it is reused: its metadata
    and old records stay on disk, and recovery replays them. uDepot drops
    tombstones unconditionally.
  - **Tombstone reclaim is lazy, by design.** GC usually frees older
    segments than it keeps, so a victim is rarely older than every freed,
    not yet reused one: tombstones go in windows and can linger on a busy
    device, but are reclaimed eventually. Do not make it prompt (say, by
    erasing a freed segment's metadata, a write per segment) without
    agreement. `TombstoneIsDroppedOnlyWhenNoOlderCopyIsOnDisk` writes until
    GC has had a window, rather than expecting one in a fixed workload.
- **Memcache mode (`StoreConfig::memcache`)** is uDepot's *MC runtimes
  (`udepot-lsa.cc:338-380`, `889-915`): salsa gets `init_local(1, 20, 40)`,
  the circular-buffer GC, which cleans the oldest sealed segment first and
  runs from 20 free segments to 40 (the watermarks count segments). Legacy
  also set overprovision 40 per mille (4%: its "// 1%" comment is wrong);
  uDepot-ng does not, by the maintainer's decision, and
  `StoreConfig::overprovision` is not used: see "Capacity" below. The mode
  is not on the device.
  - **GC evicts.** For every record the directory still points at, live or
    a tombstone, `gc_record` removes the entry and invalidates its grains
    under the key's stripe locks, as for a drop of a tombstone, and nothing
    is relocated or written. It still reads the victim segment, to learn
    its keys. The store keeps no live-entry or byte counters (a delete
    keeps none either), so the table entry and salsa's valid-grain count
    are what stay consistent. A reader that found an entry before it went
    keeps its segment for a grace period, as for relocation.
  - **Eviction speed.** Eviction reads each victim segment whole, so
    writers that outrun GC wait: see `docs/TODO-memcache-eviction.md`.
  - **Capacity.** Live data is capped at the device's grains (its tail,
    the part past the last whole segment, included) less half the low
    watermark's segments (10 with the defaults), exactly, in grains:
    `Scm::init` takes the cap directly (its only change; it used to derive
    it from a per-mille overprovision, which cannot say 10 segments on a
    large device). With legacy's 4% the cap was reached with more than the
    low watermark still free once the device had over ~500 segments, so no
    GC was due and puts failed with ENOSPC. Under the cap, live data
    reaching it means at least (N - 10) full segments, each holding at most
    a segment less its metadata grain, so under 10 are free, and GC is due
    from 20: GC is due for about 10 segments before the cap can bite.
    Partly filled stream segments and the index's segments (a table's
    segment is provisioned whole until retired) only raise the number of
    non-free segments for a given amount of live data, so GC is due
    earlier. GC being due is not GC having caught up: several writers can
    fill those 10 segments faster than it evicts them, so a refusal by the
    cap in a memcache store makes the put wait for GC (`-EAGAIN`, the space
    wait it already has for no free segment), not fail with ENOSPC. So does
    a directory grow, which keeps the tables it has already gathered
    (`Directory::grow`'s spare list); one whose peak (old tables plus twice
    as many new, and 4 data segments) could never fit under the cap fails
    with ENOSPC and a message. `capacity_waits()` counts both. A KV store
    keeps its overprovision and its ENOSPC.
  - **Sizing.** Salsa's spare-segment check (4 spare segments besides the 2
    GC reserves) passes at any size under this cap, which leaves 8. What
    binds is the cap's room: it must hold the first table and 4 data
    segments (one filling, one sealed for GC to evict, one filling after
    it, one staged), or a put refused for space could wait for a GC with
    nothing to evict. That is 5 net segments of cap: 15 whole segments
    always suffice (10 off, 5 of room), 14 do with a tail of nearly a
    segment, so 15 is a sufficient bound, not the exact one. Measured, a
    KV store of 14 segments of 256 grains (with a 4 KiB tail) is refused as
    memcache and one of 15 opens. The margin for growing is 3 tables' worth
    per table plus 4 data segments, so a store near the minimum cannot
    grow past its first table (the guard says so if the cap refuses it a
    segment); its 4 data segments hold several times fewer entries than a
    table does (measured: a table of a 128 KiB segment held ~5000 entries
    before growing, 4 such segments hold 1020 one-grain records), so it
    does not need to. A new store
    below that has its segment halved until it fits (the usual message
    names the cap); an *existing* store keeps the segment size in its
    device metadata, so its open fails (-EINVAL, with a message naming the
    real cause, the device untouched). GC runs from 20 free segments to
    40, and some segments are never free (the first table's, and two more
    salsa keeps: measured, with 128 KiB segments GC rests at 43 segments
    and not at 42, each table the directory grows raising it by one), so
    a device of fewer than 43 segments cannot let it rest and the open
    says so; with the default fitted segment (~512 MiB) that is a device
    over ~21 GiB. The open prints the mode, the GC parameters, the cap and
    the segment count once it has succeeded.
  - **A clean shutdown is exact; a crash starts empty.** A persisted index
    is restored as for any store. Without one, the log is *not* scanned:
    eviction removes entries without writing anything and freed segments
    are reused out of age order, so a scan could bring back an older value
    of a key whose newer copy's segment was reused, or a key whose
    tombstone was evicted. The open takes a new seed, writes new device
    metadata and a fresh directory, as for a new store, so no old record or
    segment metadata is taken for a new one, and prints why: no persisted
    index (not shut down cleanly), or an index it cannot restore (another
    format or geometry, incomplete, unreadable, inconsistent). A device
    with no valid device metadata is no store: it is made new only if it
    is a new or empty file or `force_destroy` asks, else the open fails
    with -EEXIST, as for any store. If close cannot
    persist the index, it says the entries are lost.
  - **The mode is not on the device (legacy's design), with three
    consequences.** A store that crashed as KV and is opened with
    `memcache = true` is wiped at open. A store that crashed as memcache
    and is opened as KV replays the log this mode calls unsafe (stale
    values, deleted keys back), with no message. A memcache store closed
    cleanly and opened as KV can hold more live data than a KV store's cap,
    so salsa refuses to restore it: the open fails (-EIO) with a message,
    and, as any open does, has cleared the index, so reopened as memcache
    it starts empty. Open a store in the mode it was created in.
  - **Index segments.** The circular GC frees a segment only when the
    relocation upcall returns 0 (nbin also frees at zero valid grains), so
    `IndexCtlr::gc_callback` returns 0 for a retired table's segment and
    keeps ENOSYS (uDepot's) for a live one, tracked by a per-segment atomic
    flag set when the table is retired: no lock. A KV store is unchanged.
    Salsa's circular GC puts a segment whose relocation upcall returned
    ENOSYS back on its queue and tries the next at once, so with only such
    segments left (live tables') it would spin on them. It now notices a
    lap of its queue (back at the first such segment, nothing freed or
    cleaned since) and waits on its watermark waitqueue for a kick: a
    generation counter (`kick_gen`) bumped, before a broadcast, by
    `gc_kick()`, which a segment entering the queue calls
    (`gc_base_seg_destaged`) and so does the store when it retires a
    table's segment (`Scm::gc_kick()`, after setting the retired flag; and on the error paths
    that retire a segment it allocated). The generation is read before each
    candidate is tried and the waiter compares under the waitqueue's mutex,
    so a kick is not missed. The empty-queue wait (it used to sleep a
    millisecond) is the same wait. Once per segment entering the queue or
    retired, never per put or get; no new lock. `gc_waits()` counts the
    waits (only actual sleeps; empty-queue and lap waits are not told
    apart). Only the circular GC's loop changed, in `gc-base.c`; the greedy
    loop is unchanged and only gets the extra wakeups, and a KV store's
    nbin GC does not use any of it. An error that persists (a segment whose
    read fails) takes part in the lap like ENOSYS, and its message prints
    once per run of failures.
  - **Restore order.** After a clean reopen the segments go back to GC
    oldest first (by timestamp), not in index order, so eviction stays
    oldest first.
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
│   ├── store.h                  # uDepot<IO> — the single KV implementation
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
│   ├── multi_get.h              # N gets on one thread, windowed by queue depth
│   └── io/
│       ├── backend.h            # IoBackend concept
│       ├── posix.h              # pread/pwrite (sync)
│       ├── o_direct.h           # pread/pwrite with O_DIRECT (sync)
│       ├── aio.h                # Linux AIO, per-thread context
│       ├── aio.cc
│       ├── uring.h              # io_uring, per-thread ring
│       ├── uring.cc
│       ├── spdk.h               # SPDK/NVMe, per-thread qpair, DMA buffers
│       ├── spdk.cc
│       ├── net.h                # socket I/O (epoll or io_uring)
│       ├── net.cc
│       └── poller.h             # shared poller infrastructure
├── src/net/
│   └── memcache.cc              # memcache protocol server
├── python/
│   ├── wrapper/
│   │   ├── pyudepot.h           # C ABI (24 functions)
│   │   └── pyudepot.cc          # bridges C ABI to uDepot via run_sync()
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

**No thread-per-get.** A `ThreadPoolExecutor.submit(get, …)` per key is exactly
the overhead this rewrite eliminates. All I/O concurrency comes from the
eager-start coroutine model: one thread, zero thread creation.

The C ABI is `src/python/pyudepot.h` (24 functions, `extern "C"`, over
`uDepot<AioIO>`; the binding is AIO-only), each call a `run_sync()` on the
calling thread except the batch below:

| group | functions |
|---|---|
| open/close | `uDepotOpen`, `uDepotOpenQueue` (+ `queue_depth`), `uDepotOpenGrain` (+ `grain_bytes`), `uDepotOpenSegment` (+ `segment_bytes`), `uDepotClose` |
| copying | `uDepotGet`, `uDepotPut`, `uDepotDel`, `uDepotExists` |
| zero copy | `uDepotAllocPutBuffer`, `uDepotPutBufferValue`, `uDepotPutBuf`, `uDepotFreePutBuffer`, `uDepotAllocGetBuffer`, `uDepotNewGetBuffer`, `uDepotGetBuf`, `uDepotGetBufferValue`, `uDepotFreeGetBuffer`, `uDepotThreadValueCopies` |
| batch | `uDepotMultiGetBuf` |
| geometry | `uDepotMaxValueBytes`, `uDepotGrainBytes`, `uDepotSegmentBytes` |

The Python `uDepot` class (`python/pyudepot/uDepot.py`) exposes `get`, `put`,
`delete`, `exists`, the zero-copy calls below, `multi_get_into`, and the
store's geometry as `open()` ended up with it: `max_value_bytes(key_size)` (the
longest value a put under such a key can store, see "Largest record"),
`grain_size()` and `segment_size()` in bytes, read from the open store in C++
(not echoed from the `grain_size=` argument). A put of a longer value is
refused. Keys are C-contiguous `uint8` numpy arrays.

### Grain size

`uDepot(..., grain_size=4096)` is `uDepotOpenGrain(..., grain_bytes)`:
`StoreConfig::grain_size`, the unit of allocation and I/O. `uDepotOpen`
and `uDepotOpenQueue` open with 4096 (`kDefaultGrainBytes`). A record is at most 2047 grains
(`HashEntry::kKvSizeMask`), so the grain bounds the value: ~8 MiB at 4096,
~64 MiB at 32768, in one I/O (flywheel raises the grain to store larger
tensors rather than split them). The cost is a grain per record: a small value
occupies a whole grain, and the segment must hold a record, so larger values
need larger segments (`segment_size`, which sets the tables) and a device that holds several segments.

The store (`uDepot::open`) checks nothing about the grain, so the binding does
(`grain_is_valid`, `pyudepot.cc`); anything else fails the open with a message
on stderr naming the value and the rule, never a fallback to 4096:

- **a power of two**: below 4096, `sanitize` in `uDepot::open` rounds a segment
  to whole 4096-byte pages by `4096 / grain`, which needs a divisor of 4096.
  At and above 4096 no requirement of the store was found; only powers of two
  (uDepot's grains) have ever been run, so nothing else is let in;
- **at least 512**: the file is opened O_DIRECT (`open_direct`), whose offsets
  and lengths are multiples of the device's logical block size, 512 at the
  smallest, and every record, segment and metadata I/O is a multiple of the
  grain. Where the backing says more, the grain is checked against it before
  the open (`direct_io_alignment`): a block device's logical sector size
  (`BLKSSZGET`), an existing regular file's `dio_offset_align` (`statx`
  `STATX_DIOALIGN`, Linux 6.1+, where the filesystem reports it). A grain that
  is not a multiple fails with a message naming both numbers (otherwise the
  open fails with only `Input/output error`, a 512 grain on a 4Kn device).
  The filesystem's block size is not used as a fallback: ext4 takes 512-byte
  O_DIRECT I/O on 4096-byte blocks, so it would refuse grains that work. What
  cannot be told is not checked: a file that does not exist yet, a kernel or
  filesystem without `STATX_DIOALIGN`. The store's open then reports what the
  device refuses;
- **at most 1 MiB**: a record is at most 2047 grains (`HashEntry::kKvSizeMask`)
  and is written with one `pwrite`, which Linux caps at `MAX_RW_COUNT` =
  `INT_MAX & PAGE_MASK` = 2147479552 bytes. 2047 grains of 1 MiB are
  2146435072 bytes; of 2 MiB they would be twice that, and the put would fail
  with `-EIO` and no message (`put_record`, `written != total`). 1 MiB was
  checked to open and round-trip a small value; a value near 2 GiB was not.

The Python wrapper also refuses (ValueError) a `grain_size` that is not an
int in `1 .. 2**32 - 1`, since ctypes would wrap `2**32 + 4096` to a valid
4096.

**Reopening.** A store must be reopened with the grain it was created with.
On a file that has only ever held a store of that grain, another grain fails
the open (`IOError`; `-EEXIST` from the store): the device metadata sits in the
last `align_up(sizeof(salsa_dev_md), grain)` bytes of the whole grains
(`dev_md_grain_offset`), so a different grain looks where there is none, finds
no store, and `uDepot::open` will not format a file that is not new
(`no store of N bytes found ...`). Nothing is read as another grain and nothing
written then (`ReopeningWithAnotherGrainFailsAndHarmsNothing`,
`test_reopening_with_another_grain_...`, every pair of 512, 4096, 32768 and
8192, each on a new file). The store's own grain-mismatch message
(`validate_dev_md`, "created with grains of %u bytes") is reached only if the
metadata is found, and the message the caller sees names the size, not the
grain, so the Python `IOError` names the `grain_size` it was given.

**`force_destroy` does not clear the old store, and this is not safe across
grains.** `force_destroy` keeps the file's data (`size_direct_file`) and
writes the new store's metadata at the new grain's offset, zero padded to one
grain. When the new grain is smaller than the old (or the size differs at the
same grain), that write does not cover the old store's metadata, which stays
valid where it is: a reopen at the old grain, without `force_destroy`, then
opens the old store, with its old data, and writes over the new one (seen:
4096 then 512, the new store cannot be opened any more; 32768 then 4096 or
512, and 8192 then 4096, the new store opens with its keys gone). A smaller
grain after a larger one is the dangerous direction; a larger one is not.
This is the store's, not the binding's, and not new here (the same happens
after a `force_destroy` at a different size and the same grain): `force_destroy`
should invalidate the earlier metadata. It is a follow-up PR of its own; until
it lands, change a store's grain (or size) on a new file. No test asserts the
current behaviour.

### Segment size

`uDepot(..., segment_size=None)` is `uDepotOpenSegment(..., grain_bytes,
segment_bytes)`: `StoreConfig::segment_size`, in bytes here (grains in the
store). The binding's open parameters are the device size, the grain size and
the segment size, as uDepot's (legacy's `python/wrapper/pyudepot.cc:23-51` takes fname, size,
force_destroy): there is no table size. Each directory table fills one index
segment, so the segment sets the tables (CLAUDE.md, rule 10: smaller tables
come from a smaller segment size, never from a separate setting). `segment_size`
0 or `None` is the store's own default, a segment for a 2^29-byte table halved
as a table until it fits the device (`fitted_segment_size`); any other is a
caller-set segment, used as given, of any size (legacy's behaviour, including
its least-waste search on a device with no tail). The binding used to take a
`table_size` and turn it into a caller-set segment, which on a device that is
an exact multiple of the segment took that search and gave arbitrary segments
whose power-of-two tables filled only part of them
(`ZeroIsTheStoresFittedDefaultAndAnExplicitSegmentIsAsGiven`). It is gone, and `table_size=` raises `TypeError`
naming `segment_size` (`test_table_size_is_refused_naming_the_replacement`).
The store takes the segment: a table is the largest power
of two that fits the segment's data grains (`HashTable::index_bits_for`), and
"its table may fill only part of it" (`uDepot::open`, where a caller-set
segment is halved as a size, not refitted). A segment of 1100 grains of 4096
has a table of 2^19 buckets (4 MiB), 93% of the segment
(`ThePowerOfTwoTableBelowANonPowerOfTwoSegment`). `uDepotOpenGrain`, `uDepotOpenQueue`
and `uDepotOpen` are `uDepotOpenSegment` with 0 and the defaults. The binding
changes the open parameters only: not the store, the on-disk format, or how
tables relate to segments.

The segment also bounds a value: a record lives whole in one segment, so
`max_value_bytes(key_size)` is `min(2047, segment / grain - 1)` grains (one
grain of each segment is its metadata, `seg_md_grains_`) less 14 + `key_size` +
2 bytes (`max_record_grains`). A value of exactly that fits and one more byte
fails (tested for 4096-, 512- and 32768-byte grains and a segment that is not a
power of two).

A caller that sizes a store for its largest record asks for the segment, not
the table: `pyudepot.segment_size_for(grain_size, record_grains=2047)`
(`uDepotSegmentForRecords(grain_bytes, record_grains)`, in bytes; 0 with a
message on stderr, and `ValueError` in Python, for a grain `uDepotOpenGrain`
would refuse or a `record_grains` outside 1 .. 2047). It is the smallest
*fitted* segment (rule 10) whose data grains, the segment less
`seg_md_grains_`, hold the record: the segment of the smallest power-of-two
table, from 4096 bytes, for which `fitted_segment_size(table, grain)` less the
metadata grain is at least `record_grains`
(`segment_bytes_for_records`, `src/bindings/segment_check.h`, which calls the
store's `fitted_segment_size`: no constant is copied). It is not a power of
two of bytes, because that leaves the table half the segment: at grain 4096 a
segment of 2048 grains gets a 4 MiB table (`index_bits` 19, in 8 MiB), the
fitted one of 2050 grains a full 8 MiB table. For the default 2047 grains the
segments are 2056 grains at 512, 2050 at 4096, 32768, 65536 and 1 MiB: 1028
KiB, 8200 KiB, 64.06 MiB, 128.1 MiB and 2050 MiB (a table of 2048 grains:
2 GiB at 1 MiB grains).
Whole pages and `check_segment_size` hold for it by construction and are
checked. The helper adds no setting and no table size: it is the segment size
a caller would otherwise compute with `fitted_segment_size`'s constants
(`TheDefaultRecordIsFittedAtEveryGrain`, `TheStoreOpensItUnshrunkWithTheRecord`
and `TestSegmentSizeFor`). The JNI binding has no such helper.

Each of these fails the open, never a fallback or a precedence:

- **a `table_size`** (any value, with or without a `segment_size`): `TypeError`
  in Python naming `segment_size`; there is no table size;
- **not an int in `1 .. 2**64 - 1`** (a `bool`, a float, 0, negative): `ValueError`
  (ctypes would wrap `2**64 + 4096`);
- **not a multiple of the grain and of 4096 bytes**: `uDepot::open`'s
  `sanitize` rounds a caller's segment down to whole 4096-byte pages (by
  `4096 / grain` grains below a 4096 grain), and a value the store would
  change is refused rather than rounded. That is a multiple of
  `max(grain, 4096)`. Message on stderr, then `IOError`;
- **too small**: the least the binding takes is the segment's metadata grain plus the grains a
  directory table needs (`create_directory`, `restore_index`:
  `index_bits_for(net) != 0` and at least two `align_up(512, grain)` bytes of
  net), rounded up to the unit above: 3 grains of 4096 (12288 bytes), 3 of
  32768 (98304), one 4096-byte page at 512-byte grains
  (`min_segment_grains`). `TheMinimumIsTheStoresMinimum` opens a store at it
  and checks the store itself refuses one unit under it, so the two checks
  cannot drift apart. The store's own refusal below it is not only the table
check: with the table-room checks removed the store still refuses 2 grains of
4096 (in salsa, after the device is set up), so the two floors coincide there;
the test pins the binding's minimum to the store's, whichever binds. At
512-byte grains the 4096-byte unit (8 grains) is above the table's 4 grains,
so no multiple of the unit is under the minimum (`At512ByteGrainsThePageIsTheMinimum`);
- **too large**: salsa takes at most `SALSA_MAX_SEGMENT_SIZE` = `INT_MAX - 1` =
  2147483646 grains (`extern/salsa/src/util/parse-storage-args.c`, "Invalid
  parameter segment_size"); above it salsa returns `EINVAL` and the store would
  halve the segment with a message that blames the device size. The binding
  refuses it first, naming the limit (stderr, then `IOError`;
  `ASegmentAboveWhatSalsaTakesIsRefusedAndTheLimitPasses` at 512, 4096 and
  32768-byte grains). A segment of exactly the limit passes the check and
  meets the device, which halves it, said.

**Reopening.** The segment of a store comes from its device metadata
(`dev_md.segment_size`, `uDepot::open`), so on a reopen `segment_size` is
checked as above and then ignored: a store created with
one segment, reopened asking for another or for none, opens with the one it
was created with and its data (`ReopeningKeepsTheCreatedSegmentWhateverIsAsked`,
`test_reopening_keeps_the_created_segment_...`). It is not refused. An invalid
`segment_size` fails the reopen like any open, the store untouched.

One consequence: `segment_size()` is not always a valid `segment_size`. On a
device too small for the segment the store halves it with `segment_size /= 2`
and no `sanitize` (`uDepot::open`, the Scm init retry), so at grains below 4096
the result can be a non-multiple of 4096: asking for 12288 bytes at 512-byte
grains on a device of 16 such segments gives 6144 (3072 on 8). Reopening with
`segment_size=kv.segment_size()` is then refused as not a multiple of 4096,
while `segment_size=None` (or the original 12288) opens the store as it is
(`test_a_halved_segment_is_not_always_a_valid_argument_on_reopen`). The store is
unchanged.

**A small device shrinks it.** The store still halves a segment the device
cannot hold: too few segments (measured: a 32 MiB device asked for
segments of 1 MiB opened with segments of 936 KiB, 35 of them), or no tail for the device metadata past the
last whole segment (a device that is an exact multiple of the segment has
none). It prints `the requested segments of N KiB ... Using segments of M KiB`
with the table it got, and `segment_size()` is the result, so check it
(`ASegmentTheDeviceCannotHoldIsHalvedAndSaid`). The tests give the device 64
segments and a two-grain tail for that reason.

### Closing

`kv.close()` is `uDepotClose` now: the store writes its index back, shuts
down and is freed, and the same file can be opened again in this process (the
data comes back; `test_data_survives_a_close_and_a_reopen_in_this_process`).
It is idempotent, and `with pyudepot.uDepot(...) as kv:` closes on exit, also
on an exception. Every other method then raises `ValueError("uDepot store is
closed")` (all of them go through `_handle()`; `test_every_method_raises_closed_after_close`
lists them, `multi_get_into` and the buffer calls included), rather than passing
the freed pointer to C. As in uDepot, no call on the store may be in progress
on another thread, or follow, when it is closed: `close()` is not a lock.

A store nobody closes is closed at interpreter exit by an `atexit` hook
(`atexit.register(self.close)`). `close()` takes the hook back, so the exit
does not close it a second time and the hook does not keep a closed store
alive until exit. PutBuffers, GetBuffers and the arrays taken from them stay
valid after the store is closed (see Zero-copy values below).

### multi_get_into: N gets, one thread, one call

```c
int uDepotMultiGetBuf(void* kv, uint32_t n, const uint8_t* const keys[],
                      const uint32_t key_sizes[], void* const get_bufs[],
                      int results[]);
```

`kv.multi_get_into(keys, bufs)` reads `keys[i]` into `bufs[i]` (`GetBuffer`s of
this store) and returns one `bool` per key, as `get_into`: `bufs[i].value` is
the value where `True`, empty where `False` (a missing key); any other failure
raises `IOError`, see Errors below. It is one ctypes call, so the GIL is released once
for the whole batch. `udepot::multi_get` (`src/udepot/multi_get.h`) launches
eager-start `get` coroutines, each submitting its I/O at once, keeps at most a
**window** of them in flight, starts the next as each completes, and drives
`poll_this_thread()` until all are done, writing each rc into `results[i]`. It
is the slot loop of `test/perf/udepot_ng_bench.cc` (`op_window()`, its `--qd`),
and uses no threads and no locks.

- **The window** is what the calling thread's AIO context takes: an I/O past
  a full context fails `-EAGAIN`, and a get holds one entry. The binding uses
  `min(store->io().queue_depth(), AioIO::thread_queue_room())`: the store's
  `StoreConfig::queue_depth` (1024, `AioIO::kDefaultQueueDepth`, unless opened
  with `uDepot(..., queue_depth=N)`, i.e. `uDepotOpenQueue`; the default is
  unchanged), and the thread's context depth less what it has in flight. **W
  follows the thread, not the store**, because the context belongs to the
  thread: every `AioIO` on a thread shares it, `open()` of any store resizes it
  if it is idle (`aio.cc`, `AioIO::open`), and a thread's first I/O creates its
  context at `new_ctx_depth_`, process-wide, the depth of whichever store was
  opened last. So a store opened with a deep queue and used after a shallow one
  was opened on the same thread runs in the shallow context. The kernel sizes
  a context for at least 8 I/Os per possible CPU whatever depth was asked for,
  so the window is a safe bound, not an exact one.
- **Errors.** A missing key (`-ENOENT`) is `False`. Any other failure
  (`-EAGAIN`, `-EINVAL`, an I/O error) raises `pyudepot.MultiGetError` (an
  `IOError`, so `errno`/`strerror` are those of the first failure; the message
  names it) after the other buffers have been filled; it is not reported as a
  missing key. Its `failed` attribute is the list of `(index, rc)` of every
  such key, as data: indices into the `keys` argument in batch order, `rc` a
  negative errno as the C layer returns it. It is a subclass so the attribute
  is always there. (`get_into` still returns `False` for every
  failure.)
- **Buffers** must be distinct objects (one buffer serves one get): a repeat
  raises `ValueError` before anything runs. The lifetime rule is `get_into`'s:
  a buffer with a live `.value` array reads into a fresh C buffer.
- **Backends.** On AIO the gets overlap. On `PosixIO` each get completes as it
  starts, so a batch is serial gets: correct, not concurrent.
- **Shared arrays.** The arrays from `.value` are writable, and one shared
  between threads is the caller's to synchronise.

### Zero-copy values from Python

`kv.alloc_put_buffer(key_size, val_size)` returns a `PutBuffer` and
`kv.alloc_get_buffer(key_size, val_size)` a `GetBuffer`: handles to the store's
`PutBuffer`/`GetBuffer` (C ABI `uDepotAllocPutBuffer`, `uDepotPutBuf`,
`uDepotAllocGetBuffer`, `uDepotGetBuf`, ...). `PutBuffer.value` is a writable
numpy `uint8` array over the record buffer's value region: fill it in place and
`kv.put_buffer(key, pb)` writes it, with no copy (`pb` is reusable once that
returns). `kv.get_into(key, gb)` reads into `gb`, and `gb.value` (also
writable) views the value as of that get. `pyudepot.thread_value_copies()` is
`uDepot::thread_value_copies()` for the calling thread; the tests assert it does
not move across a zero-copy put and get, and moves by one per copying call.

- **Alignment.** The record buffer is 4096-aligned (AIO's `alloc_buffer`), but
  `value` starts `sizeof(KvHeader) + key_size = 14 + key_size` bytes into it,
  so it is not aligned beyond that. The record's 2-byte checksum follows it.
- **Lifetime.** The C buffer is freed exactly once, when the last array over it
  is gone, not when the `PutBuffer`/`GetBuffer` object is: `.value` is a numpy
  array over a small `_Region` object (via `__array_interface__`), which numpy
  keeps as that array's base and which holds the owner of the C handle; a view
  holds the array it was made from, so it keeps the region alive too (on
  numpy 1.26 a slice's `.base` is the ndarray, not the region).
- **Get values are writable.** A get buffer is private to the arrays made
  from it: `get_into` never overwrites or frees a buffer a live array views
  (it reads into a fresh one), and the store keeps no reference to the
  buffer's memory (no cache, no index pointing into it), so writing into
  `gb.value` changes neither the stored value nor what a later get returns.
  Read-only would only force consumers (`torch.from_numpy`) to copy.
- **Replacement.** A get into a `GetBuffer` that is too small reads into a new
  buffer, freeing the old one (`GetBuffer::value()` can move). The binding never
  lets that, or a reuse of the buffer, change or free memory an earlier array
  still views: while one is alive, the next `get_into` reads into a new buffer
  instead (one allocation that reuse would have saved). Drop the array first
  to reuse the buffer.
- **Errors.** Sizes the store cannot hold raise `ValueError` naming them; a
  `put_buffer` key of another size than the buffer's, or a buffer of another
  store, raises `ValueError` (the C ABI returns `-EINVAL` for the key size); a
  closed store raises `ValueError`. The binding is AIO-only, so a buffer is `aligned_alloc`/`free` memory and
  stays valid after the store is closed (on SPDK it would be `rte_malloc` memory
  tied to the SPDK environment; not exposed). A `PutBuffer`/`GetBuffer` serves one call at a time, as the C++
  buffers do; using one buffer from two threads at once is undefined. Arrays
  taken from `.value` may be shared between threads: a later `get_into` never
  overwrites or frees a buffer that an array still views. A size over the store's 2047-grain record limit
  raises `ValueError`; a failed allocation raises `MemoryError`.

An async Python API (asyncio-compatible, mapping each coroutine to an asyncio
future) is planned as a follow-up once the async backends are working.

## On-Disk Format

The intent is uDepot's format (segment layout, directory table layout, salsa
metadata; see `docs/udepot-paper.md`, §4.4), so a uDepot-ng store and a uDepot
store can read each other. **Known divergences:**

- Index segment headers and footers extend uDepot's `dirmap_hdr` /
  `dirmap_ftr` (below) with the table's size, which restore checks against
  the size the segment gives.

### Record identity

A record is uDepot's (`uDepotSalsaStore`): a 14-byte header (key size,
value size, and the timestamp of the segment it was written into), the key,
the value and a 2-byte checksum. The paper describes a 6-byte header; uDepot's
code has always carried the timestamp, and uDepot-ng follows the code. As
uDepot's `checksum16(timestamp, md)`, the checksum is a CRC32 seeded with the
segment's timestamp, over the header, then over the device seed, truncated to
16 bits.

Recovery and GC accept a record only if its header carries its segment's
timestamp *and* the checksum matches; uDepot checks the checksum alone. The
timestamp check is what rejects records left from a segment's previous use:
CRC32 is linear, so for a given pair of old and new segment timestamps the
16-bit checksum of every stale record either passes or fails alike (about one
segment reuse in 65536 would let a whole stale tail through), while the
64-bit timestamp of a previous use never equals the current one. A store
created over an earlier one (`force_destroy`) restarts its timestamps from
the same values on the same segments; without the seed in the checksum, a
crash brought back the earlier store's records left past what the new one
had written. Each new store therefore needs its own seed: uDepot took the
monotonic clock's seconds, which repeat for two stores created within a
second or across reboots, so uDepot-ng draws it from `std::random_device`
mixed with the real-time clock.

A record's version (what `get` reports and conditional `put`/`del` take) is
its segment's timestamp and its grain within the segment, not its address:
an address comes back once the segment is reused, so a stale conditional
write could have acted on a different value (ABA). GC moving a record gives
it a new version, so a conditional write after that fails with `-ESTALE`
though the value is unchanged; that is safe, and costs no bytes on disk.

Each segment's timestamp is the allocation count salsa gave that segment,
passed to the metadata callback, so two streams staging segments at once
get different ones (reading the counter in the callback after both had
bumped it gave them the same, and recovery could not order their records).

### Device geometry

As uDepot: the device is divided into segments of `segment_size` grains; the
last grain(s) of every segment hold salsa's per-segment metadata, and the
device metadata lives in the tail past the last whole segment. There must be
such a tail (uDepot's `check_dev_size`): a fresh store whose size is an exact
multiple of the segment size gets the smaller segment size that leaves one
and wastes least, and a restored store without one fails to open. Without
the check, the device metadata sat inside the last segment, so filling the
device overwrote it and the next open started an empty store.

### Largest record

A record (header, key, value and checksum, 16 bytes besides key and value,
rounded up to whole grains) is written whole into one segment, and a hash
entry says its size in 11 bits of grains. Two limits follow, and the largest
record is the smaller of them:

- **the entry's**: 2047 grains (`HashEntry::kKvSizeMask`): 1 MiB - 512 B at
  512-byte grains, 8 MiB - 4 KiB at 4 KiB grains. This one is a divergence
  from uDepot, which has not been agreed: in uDepot the field *saturates*,
  it is not a cap. `uDepotMap` stores `min(kv_size, UDEPOT_SALSA_MAX_KV_SIZE)`
  (`udepot-map.hh:199,207`), and where the field equals the maximum, a
  record's true size is read from its header (`udepot-directory-map.cc:
  669-684`: "have to read from disk the KV metadata to figure out its full
  size"), the paper's "second IO" (`docs/udepot-paper.md`, the entry layout).
  So uDepot accepts records past 2047 grains, up to a segment. uDepot-ng
  refuses them; implementing the second IO is a low-priority todo, not part
  of this change, which keeps the cap as it was.
- **the segment's**: its data grains, the segment less its metadata grains
  (`seg_md_grains_`, one at 512 and 4096). Salsa refuses a longer record
  (`SalsaCtlr::allocate_grains_no_wait`, EINVAL: legacy's put returns that
  error to its caller too).
  Only a segment under 2048 grains meets this limit: a small device, which
  shrinks the segment, or a small table.

`uDepot::max_record_grains()` is that minimum, `max_record_bytes()` times the
grain, and `max_value_bytes(key_size)` the longest value under a key (valid
after `open()`). `check_sizes`, which `put`, `alloc_put_buffer` and
`alloc_get_buffer` call before allocating anything, uses the same function
and refuses a longer record with `-EINVAL`. Before this, `check_sizes` knew
only the entry's limit: a record over the segment but within it got `-ENOSPC`
from salsa, which reads as a full store.

`alloc_get_buffer` is sized as the record it will read, so on a store whose
segment holds fewer than 2047 data grains it now refuses a buffer larger than
any record that could be stored, where it used to hand one out. Nothing in
the repository relied on that: the bindings do not use it, and the tests and
the bench allocate for values they put.

`open()` says the segment, the grain, the largest record and which limit it
comes from; the Python binding reports them as `max_value_bytes(key_size)`,
`grain_size()` and `segment_size()`.

### Index segments (paper §4.4)

As uDepot's `uDepotDirectoryMap`, the directory's hash tables map to index
segments: each table has one of its own for its whole life, is as large as
the segment allows, and is written back to it at `close()`. An open after a
clean shutdown restores the tables from their segments instead of scanning
the log. After a crash the log scan decides, as the paper says: *"the
persistent source of truth is the log"*.

- **One segment per table.** The index has a salsa controller of its own
  (`IndexCtlr`), so its segments' metadata carry its type and the log scan
  skips them. A new table, at open or in a grow, takes a net-segment
  allocation (exactly one segment), and keeps it until a grow retires the
  table (`TableSource`); its grains stay valid meanwhile, so neither GC nor
  a data allocation can take it, and a store full of live data can still
  write its index back. Retired tables' grains are invalidated as the grow
  publishes the new snapshot; their memory waits out a grace period.
- **Size.** As `uDepotMap::restore`: the largest power of two of buckets,
  plus one neighborhood, that fits between the header and the footer
  (`HashTable::index_bits_for`). The segment size is the only knob; tests
  get small tables from small segments. The directory starts with one
  table, as uDepot's `init()` grows from none.
- **Fitted segments.** Besides its buckets a table needs its header and
  footer, the last neighborhood's overflow slots and, past the footer, the
  segment's salsa metadata: about 1.8 KiB. A segment of a power of two of
  bytes falls just that short of its next table size, so its table filled
  half of it — in uDepot as well (`tot-entries=2097184`, 2^21 buckets, in a
  32 MiB segment). `fitted_segment_size(table_bytes, grain)` is the segment
  whose table holds exactly `table_bytes` of buckets: the table plus that
  room, rounded up to a page (512 MiB + 4 KiB for a 2^26-bucket table at
  512 B grains). The default segment is the fitted one for a 2^29-byte
  table (uDepot's default, 2^29 bytes + 2 grains, meant this and fell
  short), and halving it for a small device halves the table and refits,
  including where a fitted size leaves no tail for the device metadata. A
  segment size the caller sets is used as given. A table's memory is all
  touched at open, so the default means up to 512 MiB per table. The
  Python and JNI bindings take no table size: their open parameters
  are the device size, the grain size and the segment size
  (`segment_size`, `initWithSegment`, YCSB's `udepot.segment-size`; 0 is the
  store's default segment), and the table follows the segment. A table
  maps on 4 KiB pages where huge pages would leave over 1/8 of the mapping
  unused (a 2 MiB table, say), and the store says so. A
  device too small for the segment asked for (or the default) makes it
  smaller, as uDepot does, and `open()` says so too, with both sizes and
  how much of the smaller segment its table fills: a caller's size is
  reduced as a size, not refitted as the default is.
- **Memory** (`TableRegion`). An anonymous mapping of the whole segment,
  uDepot's mapping on its AIO and SPDK backends: read from the segment on
  restore, written back at close. As uDepot's fix for the shutdown overrun
  (0e0b0a0), the mapping always spans the full segment
  (`get_seg_size() * grain`), so the footer at the end of the net region
  is always inside it, and the segment's salsa metadata past the footer is
  never written from it. A segment maps on huge pages, rounded up to a
  2 MiB multiple, when that leaves at most 1/8 of the mapping unused
  (`TableRegion::wants_huge_pages`): the mapping is anonymous, so it need
  not match the segment's size or where it sits on the device, and a
  fitted segment is never a 2 MiB multiple. (uDepot took huge pages only
  for a segment that is a 2 MiB multiple, which is what left its tables
  half full.) A fitted segment is a power of two plus a page or two, so
  rounding costs nearly 2 MiB per table and nothing on the device: under
  1/8 from a 16 MiB table up (the default's is 512 MiB, 0.4%), but half
  of a 2 MiB one, which would take 4 MiB of huge pages each,
  out of the pool SPDK's DMA buffers also come from, up to 512 MiB over a
  grown directory's 256 tables. So a small table (a 2 MiB one, say), and
  any segment under 2 MiB, map on 4 KiB pages, the segment exactly; so
  does any table when no huge page is free. The main cost is in grows: a
  grow faults each new table in 4 KiB at a time (512 times the faults), and the puts
  waiting on a full table wait for the whole grow; the copy and lookups
  also miss the TLB more, a second-order cost. Neither fallback is silent:
  `open()` prints a message whenever a segment of 2 MiB or more maps on
  4 KiB pages by this rule, and the store prints one, once per open, the
  first time a table that wants huge pages cannot have them (at open, or at
  the grow that finds none free), with the mmap error and how many huge
  pages a table takes. As uDepot's `KV_conf::sanitize_segment_size`, a store's
  segment size is whole pages. Verified: with huge pages reserved, a
  fitted 16 MiB + 8 KiB segment's table maps on huge pages as 18 MiB
  (`TableRegion.HugePagesRoundedUpTo2MiB`), and the store suites pass.
- **Layout**, as uDepot lays out a directory segment:
  `[IndexHdr 512 B | slots, 8 B each | ... | IndexFtr 512 B]`, the footer at
  the end of the net segment. Slots are `HashEntry`'s raw 64 bits, which use
  uDepot's bitfield layout.
- **Close.** GC stops first: it relocates records, updating the tables,
  which are then written back as they stand. Each table's header and every
  slot go to its segment (with grains over 512 B the last slots share the
  footer's grain, and are written with the rest as well), and only once
  every table is there its footer, which
  carries a checksum bound to the device seed and the flush's timestamp,
  newer than any index seen on the device. An index is complete when, for
  its timestamp, every table has a valid footer; only the newest index is
  considered. If the writes fail, the next open scans the log.
- **Restore.** Map each table over its segment and read it in; rebuild
  salsa's per-segment valid counts from the entries, as uDepot's
  `restore()` does (a live entry holds `kv_size` grains, a deleted one its
  tombstone's, read from the tombstone's header); keep the tables' segments
  for the session. Index segments that are not the current index's are
  free.
- **Invalidation.** Every valid footer found at open is cleared before
  anything is written, restored or not, as uDepot's `invalidate_ftr()`:
  otherwise a crash later in the session could restore an index older than
  the log. Only the footer's 512 bytes: its grain is read, cleared there and
  written back, so slots sharing it stay on the device (uDepot writes the
  mapped slots back with it).
- **Crash recovery** reads the whole log once (a crash should be rare, and
  then a full read is the price), building the index, and only then
  restores the segments to salsa, as uDepot's `restore()` does after a
  clean shutdown (its `validity_map`): each segment with the grains of the
  records the index points to as its valid grains, which salsa provisions
  (counts as used). Dead grains, and an open segment's unwritten tail, are
  never restored as valid, and a segment with none live stays free. Index segments are
  skipped: the index is rebuilt. Its tables live in memory alone until the
  walk is done (a segment taken earlier could be one whose records are
  still to be read), then take their segments once salsa's threads run.
  uDepot's crash path instead restored every segment with valid metadata in
  full (`prep_crash_recovery`), and invalidated the dead grains after; as
  a freed segment keeps its metadata until reused, on a churned device
  that is every segment, past the logical capacity (overprovisioning):
  salsa dropped the excess, checked by an assert only, and the
  invalidations underflowed its used count, so every write then failed.
  salsa's `restore_grain_range` now provisions first and refuses a range
  that does not fit.
- **More live data than the store may hold after a crash: open fails.**
  Rejected writes come back after a crash (§4.5), and can take the log
  past the logical capacity. The device is then full: `open()` fails with
  -ENOSPC, every time, with a message giving the live data and the
  capacity, left to an operator. The live data is summed and checked
  against the capacity user writes get, before anything is restored:
  `restore_grain_range` provisions as GC relocation does, which may overshoot
  the capacity by two segments, so it alone would let such a store through
  to fail later for want of index segments, under the wrong message.
- **A read error at open fails the open (-EIO).** Only valid metadata
  that says "not this" lets a segment be passed over. A segment whose
  metadata, or whose index header or footer, could not be read may hold
  live records or a valid footer: skipped, the records would be lost and
  the segment reused, or the footer left valid, so that a crash later in
  the session restored that old index over everything written since.
- **A segment whose metadata write fails is not used**, data or index
  alike: no grain of it is handed out (a put gets -EIO, a grow fails the
  writes that asked for it with -EIO). Until its new metadata is on the
  device a segment keeps its old metadata and records, and keeps its old
  timestamp and `seg_live_` too, so GC keeps every tombstone those records
  could need.
- **No room for the index after a crash: open fails, by decision.** Those
  same records can need more tables than the session had. If the device has
  no segment for every one, `open()` fails with -ENOSPC, every time, with a
  message saying so, and the store is left for an operator to give space.
  It does not open with an index it could not persist. (uDepot would not
  open either: its grow waits for a segment.)
- **A grow that finds no index segment free yet** (salsa: EAGAIN) is
  retried, as uDepot's `grow()` loops on EAGAIN, but by the space waker, not
  inline: between attempts it resumes every waiter, and the grow keeps the
  tables it did get for the next attempt. With no segment to be had
  (-ENOSPC), it fails, and the writes that asked for it get -ENOSPC, as
  uDepot's put gets its grow's return code. At open, where nothing else can
  go on, an EAGAIN is retried for 10 s.
- **No periodic flush, deliberately.** The paper also flushes *"periodically
  to speed recovery"*. uDepot gets that only from the kernel writing back
  its mmap'd tables, and its footers are valid only after a clean shutdown,
  so after a crash it scans the log whatever was written back. A periodic
  flush shortens recovery only with a recovery path that restores the last
  flushed index and then replays the log written since (dropping entries
  that point into segments reused after the flush), which neither has. That
  path is a design of its own, deferred until there is a need for it.

## Implementation Order

1. **`coro.h`** — CoroTask with eager start, `run_sync()`.
2. **`rcu.h`** — per-thread-counter RCU.
3. **`buffer.h`** — IoBuffer with allocator tags.
4. **`io/posix.h`** — synchronous pread/pwrite backend.
5. **`hash_entry.h`, `hash_table.h`** — hopscotch table with lock-free
   reads and stripe-locked writes.
6. **`directory.h`** — RCU-protected directory of hash tables.
7. **`segment.h`** — segment geometry and salsa GC.
8. **`store.h`** — `uDepot<PosixIO>`, the first working configuration.
9. **Tests** for each layer as it is built.
10. **`io/uring.h`**, **`io/aio.h`** — async backends with pollers.
11. **`io/spdk.h`** — SPDK backend.
12. **`python/`** — pyudepot bindings.
13. **`io/net.h`**, **`net/memcache.cc`** — network backend and protocol
    server.

The first milestone is `uDepot<PosixIO>` passing the existing test suite —
RCU directory, lock-free gets, hopscotch table, sync I/O. **v0 is not
complete until the perf regression test passes**: uDepot-ng must be strictly
equal to or faster than uDepot on every operation (put, get, exists, delete),
measured head-to-head in the same run. Everything after that adds backends and
protocol support.

# uDepot-ng Development Guidelines

## Agent Rules

1. **Never ignore user instructions.** Every instruction the user gives must
   be addressed — either acted on or explicitly acknowledged with a reason if
   it cannot be done.
2. **Never gaslight the user.** Do not claim something was done when it was
   not, do not fabricate results, and do not dismiss or reframe a user's
   concern as already handled when it has not been.
3. **Fix repeating problems at the root.** When you hit a bug or build problem
   for the second time, change something so it cannot recur: a test, a
   default, a build check, or a documented rule. A fix that only repairs the
   current instance is not finished.
4. **Never compromise a design choice to fix a bug.** The design principles in
   `docs/architecture.md` are constraints on the fix, not variables the fix
   may spend. If the only way you can see to fix something is to give up a
   documented property — zero copy, lock-free reads, the absence of a global
   lock, an amplification bound — that is a signal your fix is wrong, not that
   the property is negotiable.

   If you conclude the design choice itself is wrong, **stop and ask for
   explicit consent before changing it.** Say which property, what evidence
   makes you think it is wrong, and what the change costs. Wait for an answer.
5. **Keep responses focused, brief, and concise.** Spend most of the response
   on the main answer.
6. **Before your first tool call, say in one sentence what you're about to
   do.** When you finish, lead with the outcome.
7. **Deliver what was asked, at the scope intended.** Finish the whole task.
   Check in only when different readings would lead to materially different
   work.
8. **Delegate to a subagent only for large, genuinely independent tasks.**
9. **This is a refactoring of uDepot — preserve its design choices.** uDepot-ng
   must persist uDepot's architecture and implementation choices except for the
   specific changes identified at the beginning of the rewrite (userspace RCU
   replacing per-bucket mutexes, C++23 eager-start coroutines replacing TRT,
   simplified single-file build). When a question arises about how something
   should work — yielding, polling, I/O submission, buffer management, hash
   table layout, segment geometry — **check what uDepot does first** and match
   it unless there is an explicit, agreed-upon reason to diverge. When unsure
   whether a choice is covered by the rewrite plan or is a new divergence,
   **ask before implementing.**
10. **The uDepot paper is the design reference — read `docs/udepot-paper.md`
    before any design-level change** (index, resize, persistence/recovery, GC,
    put/del ordering, zero copy, Memcache). It summarises and quotes the FAST '19
    paper. Where the paper and legacy uDepot's code disagree, **follow the
    paper, or ask**; where both are silent, legacy is the reference. Notable
    consequences already decided: empty values are valid (tombstones need their
    own encoding); the index is flushed to index segments and restored on a
    clean start, with the log scan only after a crash; resize is incremental
    per lock region with a shadow directory; PUT writes before it checks, and
    Memcache paths may carry weaker durability than the store.

## Project Overview

uDepot-ng is a high-performance key-value store for NVMe storage. It is a
ground-up rewrite of [uDepot](https://www.usenix.org/system/files/fast19-kourtis.pdf)
with a modernized concurrency model (userspace RCU, lock-free reads, C++23
coroutines) and a simplified runtime (no separate TRT scheduler).

See `docs/architecture.md` for the full design, and `docs/udepot-paper.md`
for the paper it implements.

## Design Principles

These are foundational constraints. Every change must preserve them.

1. **Zero copy**: User buffers reach storage without intermediate copies.
   DMA buffers (`rte_malloc`) only at the SPDK boundary where hardware
   requires them.
2. **No global locking**: The directory uses userspace RCU (per-thread
   counters, zero shared-line atomic RMW on the read path). Hash tables use
   lock-free reads and 1024 stripe locks for writes.
3. **Minimal amplification**: No indirection layers, journaling, or metadata
   overhead beyond what the log-structured allocator needs.
4. **Enterprise-grade crash recovery only**: Either the recovery path is
   correct and complete, or it does not exist.
5. **No thread-per-operation**: I/O concurrency comes from the eager-start
   coroutine model (submit N I/Os, drive the poller, harvest completions),
   not from spawning threads. A `multi_get` of 100 keys uses one thread
   and 100 coroutines, not 100 threads. This is what the coroutine rewrite
   exists to deliver.

## Style Guide

Follow the [Google C++ Style Guide](https://google.github.io/styleguide/cppguide.html).

- C++23 (`-std=c++23`), compiled with `-Wall -Wextra -Werror`
- Member variables: `_` suffix (e.g., `directory_`, `epoch_`)
- `snake_case` for functions and variables, `PascalCase` for types/classes
- `#pragma once` for all headers
- No exceptions on the I/O hot path

## Commit Attribution

Commits are **authored by nik-io <nicioan@gmail.com>** and **co-authored by
Claude**. Claude is a co-author, not the author.

## Build

```bash
cmake -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build

# With SPDK. Build SPDK for a portable CPU target: its default,
# -march=native, produces binaries that refuse to start ("unsupported cpu
# type") when the container lands on a host without the same CPU features.
(cd extern/spdk && ./configure --target-arch=x86-64-v2 ... && make)
cmake -B build -DUDEPOT_BUILD_SPDK=ON
cmake --build build

# Run tests
ctest --test-dir build
```

## Testing

- Test framework: Google Test
- Tests live in `test/` mirroring the source structure
- Every new public API must have corresponding tests
- Test both success and error paths
- A failing test must fail the build — never silently exit 0
- SPDK tests require `UDEPOT_BUILD_SPDK=ON` and a configured SPDK environment
- Non-SPDK tests must always pass

### CI

`.github/workflows/ci.yml` runs on every push to main and every PR:

- **unit tests**, Debug and Release. Debug keeps asserts, salsa's included;
  the first Debug run found two recovery bugs that every Release run had
  compiled out. JNI and Python included. The job fails if a backend or
  binding test was not built: a missing liburing silently disables io_uring
  in CMake, which would otherwise pass with less coverage.
- **ThreadSanitizer**, the same tests minus the bindings, failing on any
  report. Debug with `-O1`, so asserts stay on. It has the same "every test
  built" check (`scripts/ci-check-tests-built.sh`).
- **SPDK backend**, `scripts/spdk-nvmef-test.sh build tests` against a
  loopback NVMe-oF software target, as uDepot's CI does, on a Debug build.
  SPDK is built with `--target-arch=x86-64-v2` and its tree cached per
  submodule revision.
- **zero-copy perf invariant**, below: one check for AIO and io_uring,
  and **zero-copy perf invariant (SPDK)**, a second entry of the
  SPDK job (`spdk-nvmef-test.sh build perf`) on a Release build.

### Zero-copy perf invariant

As in uDepot: `scripts/perf-zerocopy.sh <aio|uring>` (or
`cmake --build build --target run_perf_test` for all of them) compares the
copying and the zero-copy put/get on a `/dev/shm` store, and fails unless
zero copy is strictly faster than copy on PUT and GET: it runs less code
and copies nothing, so "as fast" is already a regression. (uDepot allowed
zero copy to be up to 5% slower; on the paired comparison below, zero copy
was ahead in every run measured, by medians of +3% to +28%.) It compares
one operation done two ways, so there is no stored baseline to drift.
Posix is not gated: `PosixIO` is buffered `pread`/`pwrite`, so the kernel
copies every value through the page cache and zero copy has nothing to
save there. Zero copy needs O_DIRECT (AIO, io_uring, as uDepot opens them)
or SPDK, the backends the store is built to perform on.

The comparison is paired, inside one process: `udepot_ng_bench --compare`
runs rounds of a copy batch and a zero-copy batch back to back on one store
(which goes first alternates), and reports the median of the rounds'
zero-copy/copy ratios; the gate is the median of 5 such runs. Zero copy is
uDepot's property, not the caller's: given a buffer it handed out
(`alloc_put_buffer()`, `alloc_get_buffer()`), put and get do their I/O on
it directly, and given other memory they copy through one of their own. So
the bench sets both modes up as a caller would, outside the timing: every
buffer allocated and every value written up front, plain memory for the
copying API and uDepot's buffers for the zero-copy one, the same values in
both. The timed loops only issue operations; gets are checked afterwards.
An earlier version allocated a `PutBuffer` per put inside the timed loop. The two
batches of a round share whatever drifts (the runner, GC, a network
target), so the ratio isolates the copies zero copy avoids. It used to
compare separate runs of each, by median: absolute throughput differs by up
to ~1.7x between processes, and that read as AIO's zero-copy GET 13.7%
slower in CI, and on SPDK as anything from 37% slower to 52% faster, on
code where every paired comparison has zero copy ahead. Never set
throughputs from different runs against each other.

**SPDK** is gated the same way, as its own CI check, "zero-copy perf
invariant (SPDK)": `scripts/spdk-nvmef-test.sh <build> perf` starts the
NVMe-oF target and runs `perf-zerocopy.sh spdk` against it, on a Release
build like the other backends. (It used to run inside the SPDK backend
test job, on its Debug build, where its result was easy to miss.) Plus an exact
check: a zero-copy run must bounce no I/O through a DMA copy (SPDK counts
them), since the device should transfer straight to and from the store's
buffers. A mutation handing out non-DMA buffers failed it with 75126
bounces. Paired, zero copy was faster in all 25 SPDK runs measured (PUT
+0.5 to +7%, GET +1 to +11%).

### Performance regression gate

**uDepot-ng must be strictly equal to or faster than uDepot on every
operation.** This is a v0 completion criterion, not a stretch goal. The perf
test builds both uDepot (from the submodule in flywheel) and uDepot-ng, runs
the same workload against each in the same process, and fails if uDepot-ng is
slower on any operation.

The test runs interleaved (uDepot, uDepot-ng, uDepot, uDepot-ng, …) to cancel
shared drift, measures median latency per operation (put, get, exists, delete),
and asserts `median_ng <= median_legacy` for each. A small tolerance (default
5%) absorbs per-pair noise; a real regression is far larger.

Both sides use the same I/O backend, same grain size, same store size, same
device (`/dev/shm` for deterministic cache-bound measurement — same rationale as
uDepot's own zero-copy perf test). The comparison is apples-to-apples: same
on-disk format, same hash function (CityHash64), same operations. Legacy uDepot
must be built at `BUILD_TYPE=PERFORMANCE` (`-O3 -DNDEBUG`) to match uDepot-ng's
cmake Release build; `perf-regression.sh` does this automatically.

The speed gap is genuine, not a benchmark artifact. With both at -O3, uDepot-ng
is 2-5x faster at 5000 ops, varying by host: PUT +127-182%, GET +174-227%,
EXISTS +345-408%, DEL +125-303% on the two cloud hosts measured. Each phase
lasts only a few milliseconds, so single runs swing by ~15%; compare medians
across runs, never two builds' runs on different hosts.
The overhead sources in legacy, per strace:

- **PUT**: Mbuff allocation + copy per operation, pwritev (scatter-gather) vs
  pwrite64 (flat buffer), TRT coroutine scheduling overhead, virtual dispatch
  through `uDepotIO_`. I/O counts are identical (one key-verify pread
  + one pwrite for data, on both sides).
- **GET/EXISTS**: Same I/O count (one pread each). Legacy takes a per-bucket
  mutex on every read — the architectural change RCU eliminates. Plus
  Mbuff/TRT/vtable overhead.
- **DEL**: Same I/O on both sides: a key-verify pread and a tombstone write
  per delete (crash recovery needs the tombstone to know the key was
  deleted). The gap is the same per-operation overhead as PUT.

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
8. **Delegate to a subagent only for large, genuinely independent tasks,**
   and for the independent review of rule 12.
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
    own encoding); each directory table maps to an index segment of its own
    for its life, as large as the segment allows (smaller tables come from a
    smaller segment size, never from a separate setting; the default
    segment and its halvings are fitted to a power-of-two table,
    `fitted_segment_size`, and a table maps on huge pages rounded up to
    2 MiB when that leaves at most 1/8 of the mapping unused, else on 4 KiB
    pages, never silently: a message at open, or when no huge page is free,
    since grows are slower then: agreed in
    PR #9, diverging from uDepot, whose power-of-two segments left every
    table half full), is written back
    there at close and restored from it on a clean start, with the log scan
    only after a crash; the directory grows
    by freeze-and-copy, not the paper's incremental resize (tried in PR #8,
    dropped: see `docs/udepot-paper.md`, §4.3); an entry's tag is the 8 hash
    bits above its bucket's, as uDepot's `hash_to_keytag_` (the top 8 bits
    until PR #9; persisted indexes carry a format number, and an older one is
    rebuilt from the log); an io_uring poll that finds nothing completed
    spins, as uDepot's, unless `StoreConfig::poll_yields` asks it to yield
    the CPU (agreed in PR #9: configurable, legacy's spin the default); the
    AIO and io_uring store file is preallocated (`size_direct_file`;
    uDepot ftruncates it sparse), so an O_DIRECT write never allocates
    (agreed in PR #9), never silently sparse; PUT
    writes before it checks, and
    Memcache paths may carry weaker durability than the store.

11. **One concern per PR.** A PR does what its title says and nothing
    else. Whatever turns up along the way, a bug, a missing message, a
    better default, goes on the PR's follow-up list (its description) for
    a PR of its own, unless it breaks this PR's own change. After a review,
    fix what the review found, and only that. PR #9 began as per-thread
    completion polling and absorbed fitted segments, huge pages, the tag
    layout, poll_yields, O_DIRECT, preallocation and the bindings' table
    size; each of its two independent reviews found real problems, and the
    second's were nearly all in code added in answer to the first.
12. **Pre-push checklist.** Before any push, and before telling the
    maintainer something is done:
    - **Tests bite.** Every new or changed test has been seen to fail with
      the fix undone (a mutant), and to fail rather than hang.
    - **Claims are checked.** Every statement in the commit message, PR
      description, comments, docs and CLAUDE.md is checked against the
      code or the CI workflow, not written from memory or intent.
    - **No silent paths.** Every new fallback or error path prints what
      happened and what it costs, and a test checks the message.
    - **Every CI configuration.** Release, Debug, Debug TSAN (as CI runs
      it), the bindings, and the SPDK build, all passing, no TSAN report;
      and `scripts/perf-zerocopy.sh` (and the SPDK one, over the NVMe-oF
      target) when the data path or the script changes.
    - **Adversarial re-read.** Read the whole diff once more asking what a
      reviewer or CI would reject: error paths, fd and memory leaks, test
      seams left set, devices too small for the geometry, stale text.
    - **Independent review.** For a PR-sized change, run a review agent
      on the diff and fix what it finds before reporting the work as done:
      the maintainer sees the reviewed result, not the draft.

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

## Settled decisions — do not re-argue

Agreed with the maintainer, mostly in PR #9. Details are in the sections
cited; change any of these only with explicit consent (rule 4).

**O_DIRECT and zero copy**
- AIO and io_uring open the store O_DIRECT (`open_direct`) and preallocate
  it (`size_direct_file`), as a device would be. tmpfs takes O_DIRECT with
  no device behind it: a toy case, never on its own evidence of O_DIRECT
  behaviour or performance.
- O_DIRECT correctness is the unit tests' job: they run on a real
  filesystem (`/tmp`, ext4 in CI, checked by the workflow) and assert the
  backends got O_DIRECT. SPDK's run over the NVMe-oF target. Short tests
  are enough; no long or perf runs on a disk in CI.
- The zero-copy perf check stays on `/dev/shm` (low noise), 32 operations
  in flight as uDepot's `--trt-ntasks 32`, copy and zero copy paired in
  one process, zero copy strictly faster on PUT and GET, plus the exact
  gate that zero copy copies no value. Never add a tolerance, never gate
  timing on a disk (device-bound: deltas flip sign), never move it off
  tmpfs. A failure is a bug to root-cause, in the code or in a bench that
  drifted from uDepot's setup, not a threshold to relax.
- No silent fallback, anywhere: buffered I/O, a sparse file, 4 KiB pages
  for a table, a segment shrunk by a small device, no AIO context. Each
  prints what happened and what it costs.

**Legacy first**
- uDepot's behaviour is the default. A divergence is agreed first, and
  where it is a trade-off it is an option with uDepot's choice the default
  (e.g. `StoreConfig::poll_yields`: spin by default). Read uDepot's code
  before deciding and cite file:line.

**Measuring**
- The dev container is a Firecracker VM: first touches of memory fault to
  the host, so absolute throughput swings 3-4x between runs, and a gap seen
  in one run (a "collapse", a "2x") is often the VM. Only paired,
  interleaved comparisons in one run count, repeated; and CI CPUs differ
  (EPYC 7763, 9V45). Do not report a speedup or slowdown from one local
  run, and do not push a speculative CI fix: reproduce or explain first.

**Tests**
- A new test is only coverage once it has been seen to fail with the fix
  undone (a mutant). Hollow tests have shipped here: a refusal test that
  never refused, an AIO TSAN test whose path only release builds take (CI's
  TSAN build is Debug), collision tests left searching for the old tag.
- A test's device must be large enough for the geometry it tests, or the
  store shrinks the segment and the test measures something else.
- The Python and JNI bindings' 2 MiB tables are a test-size default, not a
  limit: the table size is the caller's (`table_size`, `initWithTables`).

## Style Guide

Follow the [Google C++ Style Guide](https://google.github.io/styleguide/cppguide.html).

- C++23 (`-std=c++23`), compiled with `-Wall -Wextra -Werror`
- Member variables: `_` suffix (e.g., `directory_`, `epoch_`)
- `snake_case` for functions and variables, `PascalCase` for types/classes,
  except `uDepot` itself: always a lowercase u, the μ ("micro") in the name,
  never `UDepot` (the store class is `udepot::uDepot<IO>`)
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
- A test or benchmark that mirrors one of legacy uDepot's starts from the
  legacy one's setup -- buffers, concurrency, sizes, what is timed -- and
  differs only where a difference is agreed. The zero-copy bench went round
  twice by not doing this (see "Zero-copy perf invariant")

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
  on `/dev/shm`; and
  **zero-copy perf invariant (SPDK)**, a second entry of the
  SPDK job (`spdk-nvmef-test.sh build perf`) on a Release build.
- **O_DIRECT.** Every AIO and io_uring test makes its store in `/tmp` and
  opens it O_DIRECT, as uDepot does; the unit-test jobs first check that
  `/tmp` takes O_DIRECT (ext4 on the runners), and
  `OpensODirectWhereTheFilesystemTakesIt` that the backends got it. A
  filesystem that refuses it gets buffered I/O, never silently: the open
  says so (`open_direct`). SPDK's tests run over the NVMe-oF target, which
  transfers straight to and from the store's DMA buffers.

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
zero-copy/copy ratios; the gate is the median of 5 such runs. Each batch
keeps 32 operations in flight on one thread (`--qd 32`), as uDepot's script
runs `udepot-test --trt-ntasks 32`. The bench ran them one at a time until
the gate failed on a CI EPYC 7763: on tmpfs every io_uring operation is
handed to an io-wq kernel worker, ~20 us there against ~1 us for the copy
zero copy avoids, and one at a time that hand-off is all an operation is,
so io_uring's zero-copy GET read 11% slower. Zero copy is
uDepot's property, not the caller's: given a buffer it handed out
(`alloc_put_buffer()`, `alloc_get_buffer()`), put and get do their I/O on
it directly, and given other memory they copy through one of their own. So
the bench sets both modes up as legacy's `udepot-test` does: each
operation slot (a TRT task there) has one buffer per mode, allocated before
timing and reused for every operation of the slot, plain memory for copy
and uDepot's buffers for zero copy. Each put writes its key's
8-byte tag into the value and each get checks it.

Two earlier versions measured the wrong thing: one allocated a `PutBuffer`
per put inside the timed loop; the next gave every op of a batch its own
cache-cold buffer, which the copy path hides behind its own hot record
buffer and zero copy cannot, so on `/dev/shm` zero copy read 2-12% slower
on some CPUs and failed `main`. The two
batches of a round share whatever drifts (the runner, GC, a network
target), so the ratio isolates the copies zero copy avoids. It used to
compare separate runs of each, by median: absolute throughput differs by up
to ~1.7x between processes, and that read as AIO's zero-copy GET 13.7%
slower in CI, and on SPDK as anything from 37% slower to 52% faster, on
code where every paired comparison has zero copy ahead. Never set
throughputs from different runs against each other.

**O_DIRECT** is the unit tests' to cover, not this check's: the AIO and
io_uring tests run over it in `/tmp` (see "O_DIRECT" under CI). The
zero-copy check stays on `/dev/shm` for low noise; on a disk the I/O is
device-bound and zero copy's deltas flip sign run to run (ext4 here: PUT
-16% to +12%). Besides timing it gates one exact thing: the zero-copy
batches copied no value between the caller's memory and uDepot's record
buffers (`uDepot::thread_value_copies()`; the copying ones copy every
value). A zero-copy put made to count a copy fails it.

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

**Every backend.** `scripts/perf-regression.sh [ops] [iters] [tol] [backend]
[qd]` takes posix (default), aio, uring or spdk, and the operations each side
keeps in flight. uDepot runs aio, uring and spdk on TRT as its `udepot-test`
does: `--qd` TRT tasks per phase (`--trt-ntasks`, default 128), each a slice
of the ops; uDepot-ng keeps `--qd` operations in flight. SPDK runs through
`scripts/spdk-nvmef-test.sh <spdk-release-build> regression`, both stores on
the target's whole namespace. The gate is not in CI (it needs uDepot built).
Until 5d9c4e9 it only ever ran posix. On its first run on every backend, aio
and io_uring failed it (15–87% slower than uDepot, on main as well): each
handed every completion from a poller thread to the waiting one. They now
reap on the submitting thread, as uDepot's TRT and the SPDK backend do
(per-thread AIO context and io_uring ring, io_uring submissions batched as
uDepot's `UringState`). Since then (5000 ops, 9 runs, uDepot-ng vs uDepot,
`/dev/shm` except SPDK):

| backend | qd | PUT | GET | EXISTS | DEL |
|---|---|---|---|---|---|
| posix | 1 | +80% | +145% | +265% | +92% |
| aio | 1 | +131% | +192% | +326% | +139% |
| aio | 128 | +137% | +188% | +356% | +37% |
| uring | 1 | +33% | +0.1% | +43% | +13% |
| uring | 128 | +48% | +125% | +175% | +40% |
| spdk (NVMe-oF) | 1 | +54% | +65% | +80% | +41% |
| spdk (NVMe-oF) | 128 | +152% | +113% | +278% | +69% |

io_uring GET at queue depth 1 is level with uDepot: on tmpfs an io_uring
read costs ~30 µs either way.

The posix speed gap is genuine, not a benchmark artifact. With both at -O3, uDepot-ng
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

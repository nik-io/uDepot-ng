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

## Project Overview

uDepot-ng is a high-performance key-value store for NVMe storage. It is a
ground-up rewrite of [uDepot](https://www.usenix.org/system/files/fast19-kourtis.pdf)
with a modernized concurrency model (userspace RCU, lock-free reads, C++23
coroutines) and a simplified runtime (no separate TRT scheduler).

See `docs/architecture.md` for the full design.

## Design Principles

These are foundational constraints. Every change must preserve them.

1. **Zero copy**: User buffers reach storage without intermediate copies.
   DMA buffers (`rte_malloc`) only at the SPDK boundary where hardware
   requires them.
2. **No global locking**: The directory uses userspace RCU (per-thread
   epoch, zero shared-line atomic RMW on the read path). Hash tables use
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

# With SPDK
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
on-disk format, same hash function (CityHash64), same operations.

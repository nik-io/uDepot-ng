# The uDepot paper: design reference

Kourtis, Ioannou, Koltsidas, *Reaping the performance of fast NVM storage with
uDepot*, FAST '19 (https://www.usenix.org/system/files/fast19-kourtis.pdf).

This is the design uDepot-ng implements. **Precedence:** where the paper and
legacy uDepot's code disagree, follow the paper, or ask. Where both are silent,
legacy's code is the reference. The rewrite's agreed changes (CLAUDE.md, rule 9)
still apply on top of both.

Quotes below are from the paper; section numbers are the paper's.

## Goals and scope (§4)

- GET, PUT, DELETE on variable-size keys and values: *"The maximum key and value
  sizes are 64 KiB and 4 GiB, respectively, with no minimum size for either."*
  **Empty values are valid**, so a tombstone must not be encoded as
  `val_size == 0`. Legacy's restore does exactly that; the paper wins.
- One IO per operation when there is no hash collision. No range queries.
- *"uDepot does not cache data and is persistent: when a PUT (or DELETE)
  operation returns, the data are stored in the device (not in OS cache) and
  will be recovered in case of a crash."*
- Three uses: embedded store, network server (§4.7), Memcache (§4.8).

## Space management (§4.1)

- Log structured: space is allocated sequentially; GC deals with fragmentation.
  It uses a userspace port of SALSA.
- The device is split into segments (default 1 GiB), and segments into grains
  (typically the device block size).
- **Two kinds of segment:** *KV segments* for records, and *index segments* *"for
  flushing the index structure to speed up startup"* (§4.4).
- *"SALSA performs GC and upcalls uDepot to relocate specific grains to free
  segments."* The GC policy generalizes greedy and circular buffer: greedy, plus
  an aging factor.

## Index (§4.2)

- *"an in-memory two-level mapping directory… The directory is implemented as an
  atomic pointer to a read-only array of pointers to hash tables."*
- **Hopscotch** tables, with neighbourhood H = 32 by default. Two
  modifications:
  - **Power-of-two entries, indexed by the fingerprint's LSBs**, so a resize can
    rebuild the fingerprint without IO.
  - **No per-neighbourhood bitmap or list**: linear probe over the entries.
- **Entry: 8 bytes**, `neigh_off:5, key_fp_tag:8, kv_size:11, pba:40` (in that
  declaration order). On top of that layout:
  - `pba` all ones means a free entry.
  - A valid entry with `kv_size == 0` is a **deleted** entry.
  - `kv_size` is in grains (up to 8 MiB at 4 KiB grains); larger records need a
    second IO.
- **Fingerprint:** the 35 LSBs of a 64-bit CityHash:
  - a 27-bit index into the table, and an 8-bit tag;
  - the tag's LSBs index the directory (up to 2^8 tables); up to 5 more
    fingerprint LSBs can extend it (2^13 tables);
  - fingerprint = `key_fp_tag : (λ − neigh_off)` for an entry at location λ.
- **Synchronization:** an array of locks per table; each lock covers a region
  *"strictly larger than the neighborhood size (8192 entries by default)"*.
  - A neighbourhood spanning two regions takes the second lock, in order.
    Neighbourhoods don't wrap around, which keeps lock order.
  - *"to avoid inserts spanning more than two lock regions, we do not displace
    entries further than two regions apart. Hence, operations take two locks at
    maximum."*
- **Insert:** linear probe. If no entry matches the tag, return the first free
  entry. Otherwise displace within the neighbourhood. If that fails, return an
  error and the caller triggers a resize. If tag matches exist, return them;
  *"the caller decides whether to update an entry in-place or continue the
  search for a free entry where they left off."*

## Resize (§4.3): incremental, no IO (not adopted)

- The directory grows in powers of two. Only fingerprints are needed to place
  entries, so there is no IO.
- *"During the resize phase, both the new and the old structures are
  maintained. We migrate entries from the old to the new structure at the
  granularity of the lock regions."*
- A per-lock **"migration" bit**, and an atomic **"resize" counter**
  initialized to the total number of locks.
- *"Migration is triggered by an insertion operation that fails to find a free
  entry. The first such failure triggers a resize operation, and sets up a new
  shadow directory. Subsequent insertion operations migrate all the entries
  under the locks they hold (one or two) to the new structure, setting the
  'migration' bit for each lock, and decrementing the 'resize' counter (by one
  or two)."*
- *"Hash tables are pre-allocated during the resize operation in a separate
  thread to avoid delays."*
- When the counter reaches zero, the old structure's memory is released.
  uDepot-ng does that with `Rcu::call`, after a grace period.
- *"During the resize operation, lookups need to check either the new or the
  old structure, depending on the lookup region's 'migration' status."* An
  operation must read the directory/shadow pair and the region's status as one
  snapshot (see PR #3 review, finding 4).
- Figure 4: lock region r of old table ht0 migrates to the same region r of the
  two new tables ht'00 and ht'10.
- uDepot-ng status: **not implemented, by decision.** uDepot-ng grows with
  freeze-and-copy (`docs/architecture.md`, "Directory"), as legacy grows with
  a stop-the-world `uDepotDirectoryMap::grow()`; legacy never finished the
  incremental resize either (`uDepotDirMapOR`'s shadow directory is the
  started half). PR #8 implemented it and measured put tail latency while the
  directory grew 1 -> 32 tables, open loop: incremental won with two writers
  on posix, but lost to the freeze with one writer (p99 83-89 ms against
  2-8 ms) and on aio. Two causes: the new tables' first-touch page faults
  (~216 us per stripe on a cloud VM, ten times the slot copy) land on the
  writer's put path, where the freeze takes them on the waker before it
  freezes; and on aio a put's commit, so its migrations, runs on the single
  completion poller. Fixing both needed more machinery (next tables prepared
  ahead, migration moved off the poller) for an uncertain benefit, so it was
  dropped. Revisit only with a measured need.

## Metadata and persistence (§4.4)

- **Three levels of metadata:**
  - **Device** (128 B): configuration, a unique seed, a checksum.
  - **Segment** header (64 B): configuration (owning allocator, geometry), a
    timestamp, and a checksum matching the device metadata.
  - **KV record:** *"prepends to each KV pair 6B of metadata containing the key
    size (2B) in bytes, and value size (4B) in bytes, and appends (to avoid the
    torn page problem) a 2B checksum matching the segment metadata (not computed
    over the data)."* Its order comes from its segment's timestamp, which the
    checksum binds it to. **The paper and uDepot's code disagree here:** the
    code's header (`uDepotSalsaStore`) is 14 bytes, carrying the segment's
    timestamp too, and the checksum covers it. uDepot-ng follows the code
    (decided by the project owner), and also checks that timestamp against
    the segment's in recovery and GC: see `docs/architecture.md`, "Record
    identity".
- **The index is flushed, but the log is the source of truth.** *"in-memory
  index tables are flushed to persistent storage, but they are not guaranteed
  to be up-to-date: the persistent source of truth is the log. Flushing to
  storage occurs in normal shutdown, but also periodically to speed recovery."*
- **Restore:** *"Upon initialization, uDepot iterates index segments, restores
  the index tables, and reconstructs the directory. If uDepot was cleanly shut
  down (we check this using checksums and unique session identifiers), the
  index is up to date. Otherwise, uDepot reconstructs the index from KV records
  found in KV segments. KV records for the same key (new values or tombstones)
  are disambiguated using segment version information."*
  - After a clean shutdown, only what the index references exists. A record
    written but never committed to the index, such as a rejected conditional
    put or a lost GC relocation race, stays dead.
  - After a crash, the log scan decides, by segment timestamp.
- Legacy implements this with tables mmap'd onto index segments, footers written
  and `msync`ed at shutdown, `restore()` first, `crash_recovery()` as fallback,
  and footers invalidated right after a successful restore. uDepot-ng flushes
  with explicit writes at `close()` instead of mmap (SPDK has no mmap), in
  legacy's layout. Status: implemented (`docs/architecture.md`, "Index
  segments"), except the periodic flush, deferred: see there for why.

## KV operations (§4.5)

- **GET:**
  - lock the region, look up, unlock;
  - read the record for each tag match, *without* the lock, until the full key
    matches.
- **PUT:**
  - *"we first write a KV record in the log out-of-place. Subsequently, we
    perform an operation similar to GET… to determine whether the key already
    exists"*;
  - insert a new entry, or trigger a resize if there is no room;
  - if the key exists, invalidate the old grains and update the entry in place;
  - reads for tag matches run without the lock; then *"PUT re-acquires the lock
    if the record is found, and repeats the lookup to detect concurrent
    mutation(s) on the same key: if such a concurrent mutation is detected, then
    the operation that updated the hash table entry first, wins. If the PUT
    fails, then it invalidates the grains it wrote before the lookup, and
    returns an appropriate error."*
  - Conditional PUT (only if the key exists, or only if it does not) is part of
    the design.
  - **Consequence:** write-then-check is the paper's order. After a crash, a
    rejected conditional put's record can come back from the log scan. That is
    accepted: the Memcache use case, which uses these modes, tolerates it.
    Clean shutdowns are covered by the persisted index.
- **DELETE:** like PUT, but writes a tombstone. *"Tombstone entries are used to
  identify deleted entries on a restore from the log, and are recycled during
  GC."*

## IO backends and runtime (§3, §4.6)

- O_DIRECT by default. A synchronous backend exists for unmodified applications
  (the JNI interface uses it).
- Asynchronous IO and user-space IO go through TRT, over Linux AIO or SPDK.
- *"pollers of all backends running on different cores use separate endpoints:
  Linux AIO pollers use different IO contexts, SPDK pollers use different device
  queues"*: no shared state between cores on the IO path.

## Server and zero copy (§4.7)

- **Two embedded interfaces:**
  - *"one where operations take arbitrary (contiguous) user buffers, and one
    where operations take a data structure that holds a linked list of buffers
    allocated from uDepot. The former… is inherently inefficient… for many IO
    backends it requires a data copy."*
  - The server uses the zero-copy interface, doing IO straight from and to its
    receive and send buffers.
  - uDepot-ng's `PutBuffer`/`GetBuffer` are that interface, contiguous.

## Memcache (§4.8)

- Memcache metadata (expiry, flags) is appended to the value. Expiration is
  lazy, checked on lookup.
- **GC merged with eviction:** *"a GC LRU-policy is employed at the segment
  level: on a cache hit the segment containing the KV is updated as the most
  recently accessed; when running low on free segments the least recently used
  one is chosen for cleanup, its valid KV entries (both expired and unexpired)
  are invalidated (i.e., evicted) in the uDepot directory, and the segment is
  now free to be re-filled"*. That means zero IO amplification for GC.
- Cache semantics are weaker than the store's. Evicting or losing a cached
  entry is acceptable, so Memcache paths need not carry the store's strongest
  durability guarantees (decided with the author).
- uDepot-ng status: Memcache uses relocating GC today. Eviction-in-GC is a
  follow-up.

## Evaluation defaults worth matching (§5)

- 512 MiB tables (2^26 entries), 8192 locks per table.
- Index evaluation: 1B inserts with four grow operations.

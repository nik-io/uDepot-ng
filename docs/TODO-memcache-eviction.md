# TODO: faster memcache eviction (key back-pointers)

Status: not started. This diverges from uDepot, so it needs the maintainer's
agreement first.

## Today

A memcache store (`StoreConfig::memcache`) evicts in GC: `gc_callback` reads a
victim segment and `gc_record` removes the directory entry of every record in
it that the directory still points at (live or tombstone), with no relocation
and no writes. The read is only there to learn which keys the segment holds.

- uDepot-ng reads the whole net segment of the victim in one go
  (`read_segment`, in chunks of up to 4 MiB), keys *and values*, then walks
  it in memory (header, key, value, suffix CRC per record).
- Eviction therefore costs about one segment read per segment written. GC
  frees one segment at a time, while writers fill segments, so a writer that
  outruns it hits the live-data cap and waits for GC (`-EAGAIN`, counted in
  `capacity_waits()`). The maintainer's target is eviction at least an order
  of magnitude faster than the writers; nothing has been measured against it.

## Versus legacy

uDepot's GC maps the victim read-only (`udepot-lsa.cc:754-756`, `MAP_SHARED`)
and walks it header to key, jumping over each value by the record's size
(`:765-790`), so only the pages the walk touches are faulted in. For records
smaller than a page every page is touched anyway: the same I/O as
uDepot-ng's one read. For records larger than a page legacy skips the pages
inside the value (kernel readahead may win some of that back; not measured).
uDepot-ng does not skip them: it reads the net segment whole, and SPDK has no
mmap to fall back on. Even a mapped walk of uDepot-ng's would touch the page
of each record's suffix, since it checks the CRC (`gc_callback`). So this is a
gap versus legacy for large values, and not one for small ones.

## Queue depth

GC reads are effectively at queue depth 1. There is one GC thread
(`gc_thread_nr=1` in the salsa arguments, `store.cc` open(), as uDepot's,
`udepot-lsa.cc:232`), cleaning one victim at a time. `read_segment` reads the
net segment in chunks of up to 4 MiB, one `pread_internal` after another, each
run to completion (`run_internal` -> `run_sync`) before the next starts, and
the directory removals of the segment follow serially in the same callback.
For a small segment (128 KiB is one chunk) that is one I/O in flight; the
kernel can split a large chunk into several device requests, so large
segments should be less affected (expected, not measured). Whether this is
the bottleneck is not measured. uDepot is similar: one GC thread, and serial
page faults on the mapped segment, which readahead may help (also not
measured).

Independent of back-pointers, and with no format change: issue all of a
segment's chunk reads at once, and prefetch the next victim's read while the
current one's directory removals run.

## Improvement

Key back-pointers: per record, the key's hash and its grain offset in the
segment, kept in the segment's metadata space at its end, so eviction reads
that small region only and removes the directory entries from it, never
reading a value.

Things the design has to settle:

- **Where, and how big.** The last `seg_md_grains_` grain(s) of each segment
  hold salsa's per-segment metadata, today one grain (512 B by default) of
  which `salsa_seg_md` uses 53 bytes (packed). A segment of 128 KiB with
  2-grain records holds 127 of them, so even 4 bytes of hash and 2 of offset
  per record is ~760 bytes: more grains of metadata per segment, which shrink
  the net segment (a table fills exactly one net segment, so that changes
  table sizes too) and change the on-disk layout.
- **When written.** At segment seal (one write, but the record offsets must be
  collected from concurrent writers, and a crash before seal loses them) or
  per record (a second small write per put: write amplification, which the
  design principles bound). The metadata is written when the segment is
  staged (`persist_seg_md`) and before any grain is handed out, so a seal-time
  write is a new write.
- **Crash consistency.** The back-pointers are a hint, never the truth: a
  record may have been overwritten, deleted or never committed, so eviction
  must still match each entry by its pba, as `gc_record` does
  (`entry_at(hash, grain)`), and a segment whose back-pointers are missing or
  fail a checksum falls back to reading it whole. A memcache store does not
  replay its log after a crash (it opens empty), so nothing else reads them.
- **Divergence.** Neither legacy nor the paper has this; the paper's eviction
  is a segment LRU that invalidates the valid entries of the chosen segment.

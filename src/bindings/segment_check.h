// Copyright (c) 2024-2026 Nikolas Ioannou
// SPDX-License-Identifier: BSD-3-Clause

// The checks of a caller-set segment size that the Python and JNI bindings
// share: the store would round a segment down to whole pages, or fail its
// open without a word for one too small, so a binding refuses it first, with
// a message on stderr. The bindings' only segment-size policy lives here.

#pragma once

#include <cstdint>
#include <cstdio>

#include "sto-ctlr/sto-capacity-mgr-parameters.h"
#include "udepot/store.h"

namespace udepot_binding {

// The grains of a segment that are its salsa metadata, not data
// (seg_md_grains_, uDepot::open: align_up(sizeof(salsa_seg_md), grain) /
// grain).
inline uint64_t seg_md_grains(uint32_t grain) {
    return (sizeof(salsa::salsa_seg_md) + grain - 1) / grain;
}

// The least a caller-set segment can be, in grains: the segment's metadata
// grains (seg_md_grains_, store.cc) plus the data grains a directory table
// needs, which create_directory and restore_index (store.cc) refuse a
// segment without: index_bits_for(net bytes) != 0 (a header, a footer and
// more than one neighborhood of slots) and net bytes of at least two
// align_up(kIndexMdBytes, grain). That is also room for one record grain.
// The store has no function for it; this follows its checks, and
// TheMinimumIsTheStoresMinimum (pyudepot_abi_test) opens a store at the
// minimum and one unit under it to keep the two in step.
inline uint64_t min_segment_grains(uint32_t grain) {
    const uint64_t md_grains = seg_md_grains(grain);
    const uint64_t index_md =
        (udepot::TableRegion::kMdBytes + grain - 1) / grain * grain;
    uint64_t net = 1;
    while (net * grain < 2 * index_md ||
           udepot::HashTable::index_bits_for(
               static_cast<size_t>(net * grain)) == 0)
        ++net;
    return md_grains + net;
}

// The unit a caller-set segment is a multiple of: the grain, and the whole
// 4096-byte pages uDepot::open's `sanitize` would otherwise round it down
// to (a segment of 4096 / grain grains below a 4096 grain).
inline uint64_t segment_unit_bytes(uint32_t grain) {
    return grain < 4096 ? 4096 : grain;
}

// Whether segment_bytes (not 0, which is the store's default) is a segment
// the store takes at this grain: a multiple of the grain and of 4096, at
// most salsa's limit, at least the least that holds a table. If not, says
// why on stderr and returns false.
inline bool check_segment_size(uint64_t segment_bytes, uint32_t grain_bytes) {
    const uint64_t unit = segment_unit_bytes(grain_bytes);
    if (segment_bytes % unit != 0) {
        fprintf(stderr, "uDepot-ng: segment size %llu is not a multiple "
                "of %llu bytes: a segment is whole grains of %u bytes "
                "and whole 4096-byte pages, and the store would round "
                "any other down\n",
                static_cast<unsigned long long>(segment_bytes),
                static_cast<unsigned long long>(unit), grain_bytes);
        return false;
    }
    const uint64_t min_bytes =
        (min_segment_grains(grain_bytes) * grain_bytes + unit - 1) / unit *
        unit;
    // Salsa refuses a segment of more than SALSA_MAX_SEGMENT_SIZE grains
    // (INT_MAX - 1: extern/salsa/src/util/parse-storage-args.c, "Invalid
    // parameter segment_size"), and the store would then halve it and
    // blame the device, so it is refused here, naming the limit.
    if (segment_bytes / grain_bytes > SALSA_MAX_SEGMENT_SIZE) {
        fprintf(stderr, "uDepot-ng: segment size %llu is too large for "
                "grains of %u bytes: salsa takes at most %llu grains "
                "(SALSA_MAX_SEGMENT_SIZE), %llu bytes\n",
                static_cast<unsigned long long>(segment_bytes),
                grain_bytes,
                static_cast<unsigned long long>(SALSA_MAX_SEGMENT_SIZE),
                static_cast<unsigned long long>(
                    uint64_t{SALSA_MAX_SEGMENT_SIZE} * grain_bytes));
        return false;
    }
    if (segment_bytes < min_bytes) {
        fprintf(stderr, "uDepot-ng: segment size %llu is too small for "
                "grains of %u bytes: it must hold the segment's "
                "metadata grain and a directory table's header, footer "
                "and buckets, at least %llu bytes\n",
                static_cast<unsigned long long>(segment_bytes),
                grain_bytes,
                static_cast<unsigned long long>(min_bytes));
        return false;
    }
    return true;
}

// The smallest segment, in bytes, that holds a record of record_grains
// grains and is fitted: the segment of the smallest power-of-two table (from
// 4096 bytes) whose data grains (the segment less seg_md_grains) are at
// least record_grains. A record lives whole in one segment's data grains
// (uDepot::max_record_grains), so a caller sizing a store for its largest
// record needs this, and the fitted size rather than a power of two of
// bytes: the table fills a fitted segment, and half of a power-of-two one
// (CLAUDE.md, rule 10; fitted_segment_size). At grain 4096 a segment of 2048
// grains gets a 4 MiB table in its 8 MiB, the fitted one of 2050 a full 8
// MiB table. grain must be a valid grain (the caller checks). Returns 0,
// with a message on stderr, for record_grains outside 1 .. 2047 (a hash
// entry's size field, HashEntry::kKvSizeMask, bounds a record) or for a
// result the store would not take (never, but check_segment_size is the
// bindings' one authority on that).
inline uint64_t segment_bytes_for_records(uint32_t grain_bytes,
                                          uint32_t record_grains) {
    if (record_grains < 1 || record_grains > udepot::HashEntry::kKvSizeMask) {
        fprintf(stderr, "uDepot-ng: a record of %u grains is not possible: "
                "a record is 1 to %u grains (a hash entry's size field)\n",
                record_grains,
                static_cast<unsigned>(udepot::HashEntry::kKvSizeMask));
        return 0;
    }
    const uint64_t md = seg_md_grains(grain_bytes);
    uint64_t table = 4096;
    uint64_t grains = udepot::fitted_segment_size(table, grain_bytes);
    // Terminates: record_grains <= 2047, so a table of 2047 grains of the
    // largest grain (2^31 bytes) is enough.
    while (grains - md < record_grains) {
        table <<= 1;
        grains = udepot::fitted_segment_size(table, grain_bytes);
    }
    const uint64_t bytes = grains * grain_bytes;
    if (!check_segment_size(bytes, grain_bytes)) return 0;
    return bytes;
}

}  // namespace udepot_binding

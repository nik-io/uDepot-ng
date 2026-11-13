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
    const uint64_t md_bytes = sizeof(salsa::salsa_seg_md);
    const uint64_t md_grains = (md_bytes + grain - 1) / grain;
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

}  // namespace udepot_binding

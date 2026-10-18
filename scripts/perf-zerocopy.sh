#!/usr/bin/env bash
#
# Zero-copy invariant, after uDepot's scripts/perf-zerocopy.sh: the
# zero-copy put/get interface (PutBuffer/GetBuffer) must be strictly faster
# than the copying one (spans), on PUT and GET -- it runs less code and
# copies nothing. (uDepot allowed zero copy to be up to 5% slower.)
#
# Why this shape:
#   - Copy and zero copy are compared inside one process, against one store:
#     udepot_ng_bench --compare runs rounds of a copy batch and a zero-copy
#     batch back to back (which goes first alternates) and reports the median
#     of the rounds' zero-copy/copy ratios. The two batches of a round share
#     whatever drifts -- the runner, GC, the NVMe-oF target -- so the ratio
#     isolates the value copies zero copy avoids. Comparing separate runs did
#     not: their absolute throughput differed by up to ~1.7x, and AIO's GET
#     once read 13.7% slower with zero copy in CI, on code where every paired
#     comparison has it faster.
#   - Every buffer is allocated and filled before any timing, uDepot's own
#     (alloc_put_buffer/alloc_get_buffer) for zero copy and plain memory for
#     copy, so a batch times only the operations.
#   - The gate is the median of RUNS such per-run deltas, never throughputs
#     from different runs set against each other.
#   - The store lives on /dev/shm (tmpfs: RAM-backed, buffered), so both
#     modes are cache-bound and the avoided copy is a real, consistent win.
#   - It compares one operation done two ways, in one run: no baselines.
#
# SPDK (backend spdk) runs against the namespace UDEPOT_NVMEF names, as
# scripts/spdk-nvmef-test.sh sets up (it runs this gate), not /dev/shm.
# Zero copy there also means the device transfers straight to and from the
# store's own DMA buffers, which SPDK counts: a zero-copy run must copy no
# I/O through a bounce buffer (exact).
#
# O_DIRECT on a real device: ZC_DIR puts the store on another filesystem
# (default /dev/shm), as CI does on the runner's disk. tmpfs takes O_DIRECT
# but has no device behind it; on a disk the I/O is real, and so bound by
# the device that timing cannot gate (zero copy's deltas there flip sign
# run to run: measured on ext4, PUT -16% to +12% with a sparse file, still
# one GET at -4% preallocated). So off tmpfs the file is preallocated, as a
# device would be, and timing is reported, not gated (ZC_GATE_TIMING=1 gates
# it anyway); what is gated is exact:
#   - O_DIRECT is in effect (the backend reports it; a filesystem that
#     refuses it gets buffered I/O, which copies every value);
#   - zero-copy batches copy no value between the caller's memory and
#     uDepot's record buffers, and copying ones copy every value;
# both on every filesystem, tmpfs included.
#
# Usage: perf-zerocopy.sh <backend> [build_dir] [batch] [runs] [min_gain%]
#   <backend>  aio, uring or spdk. Not posix: PosixIO is buffered
#              pread/pwrite, so the kernel copies every value through the page
#              cache and there is no zero copy to gate. Zero copy needs
#              O_DIRECT (AIO, io_uring) or SPDK.
#   [batch]    ops per batch (default 500; 200 on SPDK, whose namespace is
#              513 MiB); a run is 2 x ROUNDS batches per phase
set -uo pipefail

BACKEND="${1:?usage: perf-zerocopy.sh <aio|uring|spdk> [build_dir] [batch] [runs] [min_gain%]}"
case "$BACKEND" in
    aio|uring|spdk) ;;
    posix) echo "FATAL: posix is buffered I/O, which cannot be zero copy; gate aio, uring or spdk" >&2
           exit 2 ;;
    *) echo "FATAL: unknown backend $BACKEND (aio, uring or spdk)" >&2; exit 2 ;;
esac
BUILD_DIR="${2:-build}"
DEFAULT_BATCH=500
[ "$BACKEND" = "spdk" ] && DEFAULT_BATCH=200
BATCH="${3:-$DEFAULT_BATCH}"
RUNS="${4:-5}"
# Zero copy must beat copy by more than this: it runs less code and copies
# nothing, so it is strictly faster, never merely as fast.
MIN_GAIN="${5:-0}"
ROUNDS="${ROUNDS:-15}"    # 2 x 15 x 500 x 32 KiB fits the 1 GiB store
# Operations in flight, as uDepot's script runs udepot-test with
# --trt-ntasks 32 on one thread. One at a time, every io_uring operation on
# tmpfs waited out a hand-off to an io-wq kernel worker (~20 us on a CI
# EPYC 7763, against ~1 us for the copy zero copy avoids), so the gate
# measured the scheduler: zero-copy GET read 11% slower there.
QD="${QD:-32}"
# Large enough that a value copy is a visible share of an operation: at
# 1 KiB a copy costs ~30 ns of a ~1.6 us put, and a zero-copy path that
# copied twice stayed inside the old 5% tolerance.
VAL="${VAL_SIZE:-32768}"
SIZE=1077936129           # (1048576+4096)*1024+1, as uDepot's
HERE="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
case "$BUILD_DIR" in
    /*) BIN="$BUILD_DIR/udepot_ng_bench" ;;
    *)  BIN="$HERE/$BUILD_DIR/udepot_ng_bench" ;;
esac
ZC_DIR="${ZC_DIR:-/dev/shm}"
FILE="${ZC_DIR}/udepot-ng-zc-${BACKEND}.store"
trap 'rm -f "$FILE"' EXIT   # do not leave a ~1GB store behind
FSTYPE="$(findmnt -n -o FSTYPE -T "$ZC_DIR" 2>/dev/null || stat -f -c %T "$ZC_DIR" 2>/dev/null || echo unknown)"
GATE_TIMING=1
PREALLOCATE=0
if [ "$BACKEND" != "spdk" ] && [ "$FSTYPE" != "tmpfs" ]; then
    GATE_TIMING="${ZC_GATE_TIMING:-0}"
    PREALLOCATE=1
fi

[ -x "$BIN" ] || { echo "FATAL: $BIN not found" >&2; exit 2; }
WHERE="on ${ZC_DIR} (${FSTYPE})"
if [ "$BACKEND" = "spdk" ]; then
    if [ -z "${UDEPOT_NVMEF:-}" ]; then
        echo "FATAL: spdk needs UDEPOT_NVMEF; run it through scripts/spdk-nvmef-test.sh" >&2
        exit 2
    fi
    WHERE="on SPDK (${UDEPOT_NVMEF%%:*})"
fi

# The machine, so a result can be read against the hardware it ran on: CI
# runners differ, and the copy zero copy avoids costs what the caches and
# memory make it cost.
{
    echo "machine: $(uname -r), $(nproc) cpus"
    if command -v lscpu >/dev/null; then
        lscpu | grep -E '^(Model name|L1d cache|L2 cache|L3 cache):' | sed 's/^/  /'
    fi
    if [ "$BACKEND" != "spdk" ]; then
        echo "  store: ${ZC_DIR} (${FSTYPE})$([ "$PREALLOCATE" = 1 ] && echo ', preallocated')"
        command -v findmnt >/dev/null && \
            findmnt -n -o SOURCE,FSTYPE -T "$ZC_DIR" | sed 's/^/  device: /'
    fi
} >&2

# One run; echo "put_delta get_delta bounced put_copy put_zc get_copy get_zc
# copied_copy copied_zc direct" (bounced: SPDK only, else 0; throughputs in
# Mops/s as the bench prints them, the medians of the run's batches; direct:
# 1 or 0, "-" on SPDK).
run_compare() {
    rm -f "$FILE"
    if [ "$PREALLOCATE" = 1 ]; then
        fallocate -l "$SIZE" "$FILE" \
            || { echo "FATAL: cannot preallocate $FILE" >&2; exit 2; }
    fi
    local out
    out=$("$BIN" --backend "$BACKEND" -f "$FILE" --size "$SIZE" -w "$BATCH" \
        --val-size "$VAL" --grain-size 512 --compare "$ROUNDS" --qd "$QD" \
        2>/dev/null)
    local dp dg b pc pz gc gz
    dp=$(grep '^CMP PUT' <<<"$out" | grep -oE 'delta=[-+0-9.]+' | cut -d= -f2)
    dg=$(grep '^CMP GET' <<<"$out" | grep -oE 'delta=[-+0-9.]+' | cut -d= -f2)
    b=$(grep '^BOUNCED' <<<"$out" | awk '{print $2}')
    [ "$BACKEND" = "spdk" ] || b="${b:-0}"
    pc=$(grep '^CMP PUT' <<<"$out" | grep -oE ' copy=[0-9.]+' | cut -d= -f2)
    pz=$(grep '^CMP PUT' <<<"$out" | grep -oE 'zero_copy=[0-9.]+' | cut -d= -f2)
    gc=$(grep '^CMP GET' <<<"$out" | grep -oE ' copy=[0-9.]+' | cut -d= -f2)
    gz=$(grep '^CMP GET' <<<"$out" | grep -oE 'zero_copy=[0-9.]+' | cut -d= -f2)
    local cc cz d
    cc=$(grep '^COPIES' <<<"$out" | grep -oE ' copy=[0-9]+' | cut -d= -f2)
    cz=$(grep '^COPIES' <<<"$out" | grep -oE 'zero_copy=[0-9]+' | cut -d= -f2)
    d=$(grep '^DIRECT' <<<"$out" | awk '{print $2}')
    [ "$BACKEND" = "spdk" ] && d=-
    # sort -n does not take a leading +
    echo "${dp#+} ${dg#+} $b $pc $pz $gc $gz ${cc:-x} ${cz:-x} ${d:-x}"
}

median() { printf '%s\n' "$@" | sort -n | \
    awk '{a[NR]=$1} END{n=NR; if(n==0){print 0} else if(n%2){print a[(n+1)/2]} else {printf "%.6f",(a[n/2]+a[n/2+1])/2}}'; }

dps=(); dgs=(); bounced=0; copied_copy=0; copied_zc=0; not_direct=0
for it in $(seq 1 "$RUNS"); do
    read -r dp dg b pc pz gc gz cc cz d <<<"$(run_compare)"
    { [ -n "$dp" ] && [ -n "$dg" ] && [ -n "$b" ] && [ -n "$gz" ] && \
      [ "$cc" != x ] && [ "$cz" != x ] && [ "$d" != x ]; } \
        || { echo "a udepot_ng_bench --compare run produced no result" >&2; exit 2; }
    copied_copy=$((copied_copy + cc)); copied_zc=$((copied_zc + cz))
    [ "$d" = 0 ] && not_direct=$((not_direct + 1))
    awk -v it="$it" -v dp="$dp" -v dg="$dg" -v pc="$pc" -v pz="$pz" \
        -v gc="$gc" -v gz="$gz" 'BEGIN{
        printf("  run %s: PUT delta=%s%% (copy %.1f, zero copy %.1f kops/s)" \
               "  GET delta=%s%% (copy %.1f, zero copy %.1f kops/s)\n",
               it, dp, pc * 1000, pz * 1000, dg, gc * 1000, gz * 1000) > "/dev/stderr" }'
    dps+=("$dp"); dgs+=("$dg")
    bounced=$((bounced + b))
done

echo "${BACKEND}: median of ${RUNS} runs, each the median of ${ROUNDS} paired batches of ${BATCH} ops of ${VAL} B, ${QD} in flight, ${WHERE}:" >&2

# Gate each phase: fail unless zero copy is faster than copy by > MIN_GAIN%.
check_delta() {  # delta% phase-name
    awk -v d="$1" -v min="$MIN_GAIN" -v ph="$2" 'BEGIN{
        printf("  %s delta=%+.1f%% (fail unless above %+.1f%%)\n", ph, d, min) > "/dev/stderr";
        if (d <= min) { printf("FAIL: %s zero-copy is not faster than copy\n", ph) > "/dev/stderr"; exit 1; }
        exit 0;
    }'
}

rc=0
timing_rc=0
if [ "$GATE_TIMING" = 1 ]; then
    check_delta "$(median "${dps[@]}")" PUT || timing_rc=1
    check_delta "$(median "${dgs[@]}")" GET || timing_rc=1
else
    awk -v p="$(median "${dps[@]}")" -v g="$(median "${dgs[@]}")" 'BEGIN{
        printf("  PUT delta=%+.1f%%, GET delta=%+.1f%% (reported, not gated: device-bound)\n", p, g) > "/dev/stderr" }'
fi
rc=$timing_rc
# Exact: the zero-copy path copied no value, the copying one every value.
echo "  values copied: zero copy ${copied_zc} (fail if any), copy ${copied_copy}" >&2
if [ "$copied_zc" -ne 0 ]; then
    echo "FAIL: the zero-copy path copied ${copied_zc} values" >&2
    rc=1
fi
if [ "$copied_copy" -eq 0 ]; then
    echo "FAIL: the copying path copied nothing, so nothing was compared" >&2
    rc=1
fi
if [ "$BACKEND" != "spdk" ]; then
    echo "  runs without O_DIRECT: ${not_direct} of ${RUNS} (fail if any)" >&2
    if [ "$not_direct" -ne 0 ]; then
        echo "FAIL: ${ZC_DIR} did not take O_DIRECT: buffered I/O copies every value" >&2
        rc=1
    fi
fi
if [ "$BACKEND" = "spdk" ]; then
    echo "  zero-copy I/Os bounced through a DMA copy: ${bounced} (fail if any)" >&2
    if [ "$bounced" -ne 0 ]; then
        echo "FAIL: the zero-copy path copied ${bounced} I/Os through a bounce buffer" >&2
        rc=1
    fi
fi
if [ "$rc" -eq 0 ]; then
    [ "$GATE_TIMING" = 1 ] && echo "OK: zero-copy faster than copy on PUT and GET" >&2
    echo "OK: zero copy copied no value$([ "$BACKEND" != spdk ] && echo ', over O_DIRECT')" >&2
    [ "$BACKEND" = "spdk" ] && \
        echo "OK: zero-copy put/get transferred straight from the store's DMA buffers" >&2
fi
exit "$rc"

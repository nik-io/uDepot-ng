#!/usr/bin/env bash
#
# Zero-copy invariant, as uDepot's scripts/perf-zerocopy.sh: the zero-copy
# put/get interface (PutBuffer/GetBuffer) must not be slower than the
# copying one (spans), on PUT and GET.
#
# Why this shape (same reasoning as uDepot's):
#   - The two runs differ only in the value copies the zero-copy path avoids;
#     udepot_ng_bench --zero-copy changes nothing else.
#   - The store lives on /dev/shm (tmpfs: RAM-backed, buffered), so both
#     phases are cache-bound and the avoided copy is a real, consistent win.
#     On an O_DIRECT device the ops are I/O bound, the saving sits below
#     device noise, and the delta flips sign from run to run.
#   - Copy and zero-copy runs are interleaved and compared by median, which
#     cancels a shared runner's slow throughput drift. A small tolerance
#     absorbs per-pair noise; a real zero-copy regression is far larger.
#   - It compares one operation done two ways, in one run: no baselines.
#
# SPDK (backend spdk) runs against the namespace UDEPOT_NVMEF names, as
# scripts/spdk-nvmef-test.sh sets up (it runs this gate), not /dev/shm.
# Zero copy there means the device transfers straight to and from the
# store's own DMA buffers, which SPDK counts: the gate is that a zero-copy
# run copies no I/O through a bounce buffer. That check is exact. The
# throughput comparison is reported but not gated on SPDK: over the NVMe-oF
# loopback target the ops are I/O bound and share the host's CPUs with the
# target, and on unchanged code the GET delta measured -20% to +52% (PUT -5%
# to +12%) across runs of 15 pairs, while a 32 KiB copy is ~5% of an op --
# the same reason the other backends run on /dev/shm. SPDK_PERF_GATE_TIME=1
# gates it anyway.
#
# Usage: perf-zerocopy.sh <backend> [build_dir] [ops] [iters] [tolerance%]
#   <backend>  aio, uring or spdk (posix also works)
set -uo pipefail

BACKEND="${1:?usage: perf-zerocopy.sh <aio|uring|spdk> [build_dir] [ops] [iters] [tol%]}"
BUILD_DIR="${2:-build}"
OPS="${3:-10000}"
ITERS="${4:-9}"
TOL="${5:-5}"             # zero-copy may be at most TOL% slower (noise band)
# Large enough that a value copy is a visible share of an operation: at
# 1 KiB a copy costs ~30 ns of a ~1.6 us put, and a zero-copy path that
# copied twice stayed inside the tolerance.
VAL="${VAL_SIZE:-32768}"
SIZE=1077936129           # (1048576+4096)*1024+1, as uDepot's
HERE="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
case "$BUILD_DIR" in
    /*) BIN="$BUILD_DIR/udepot_ng_bench" ;;
    *)  BIN="$HERE/$BUILD_DIR/udepot_ng_bench" ;;
esac
FILE="/dev/shm/udepot-ng-zc-${BACKEND}.store"
trap 'rm -f "$FILE"' EXIT   # do not leave a ~1GB store on /dev/shm

[ -x "$BIN" ] || { echo "FATAL: $BIN not found" >&2; exit 2; }
if [ "$BACKEND" = "spdk" ] && [ -z "${UDEPOT_NVMEF:-}" ]; then
    echo "FATAL: spdk needs UDEPOT_NVMEF; run it through scripts/spdk-nvmef-test.sh" >&2
    exit 2
fi
WHERE="on /dev/shm"
[ "$BACKEND" = "spdk" ] && WHERE="on SPDK (${UDEPOT_NVMEF%%:*})"

# Run once; echo "PUT_mops GET_mops bounced" (bounced: SPDK only, else 0).
run_one() {  # $1 = extra flags ("" for copy, "--zero-copy" for zero-copy)
    rm -f "$FILE"
    local out
    out=$("$BIN" --backend "$BACKEND" -f "$FILE" -w "$OPS" --size "$SIZE" \
        --grain-size 512 --val-size "$VAL" $1 2>/dev/null)
    local p g b
    p=$(grep '^PUTs' <<<"$out" | grep -oE 'Mops/sec=[0-9.]+' | cut -d= -f2)
    g=$(grep '^GETs' <<<"$out" | grep -oE 'Mops/sec=[0-9.]+' | cut -d= -f2)
    b=$(grep '^BOUNCED' <<<"$out" | awk '{print $2}')
    if [ "$BACKEND" = "spdk" ] && [ -z "$b" ]; then b=""; else b="${b:-0}"; fi
    echo "$p $g $b"
}

median() { printf '%s\n' "$@" | sort -n | \
    awk '{a[NR]=$1} END{n=NR; if(n==0){print 0} else if(n%2){print a[(n+1)/2]} else {printf "%.6f",(a[n/2]+a[n/2+1])/2}}'; }

copy_p=(); copy_g=(); zc_p=(); zc_g=(); zc_bounced=0
for it in $(seq 1 "$ITERS"); do
    # Alternate which goes first, so a pair's position cancels out too.
    if [ $((it % 2)) -eq 1 ]; then
        read -r cp cg _ <<<"$(run_one "")"
        read -r zp zg zb <<<"$(run_one "--zero-copy")"
    else
        read -r zp zg zb <<<"$(run_one "--zero-copy")"
        read -r cp cg _ <<<"$(run_one "")"
    fi
    { [ -n "$cp" ] && [ -n "$cg" ] && [ -n "$zp" ] && [ -n "$zg" ] && [ -n "$zb" ]; } \
        || { echo "a udepot_ng_bench run produced no PUT/GET throughput" >&2; exit 2; }
    copy_p+=("$cp"); copy_g+=("$cg"); zc_p+=("$zp"); zc_g+=("$zg")
    zc_bounced=$((zc_bounced + zb))
done

cpm=$(median "${copy_p[@]}"); zpm=$(median "${zc_p[@]}")
cgm=$(median "${copy_g[@]}"); zgm=$(median "${zc_g[@]}")

{
  echo "${BACKEND} Mops/sec over ${ITERS} interleaved runs @ ${OPS} ops of ${VAL} B ${WHERE}:"
  echo "  PUT copy median=${cpm}  zero-copy median=${zpm}"
  echo "  GET copy median=${cgm}  zero-copy median=${zgm}"
} >&2

# Gate each phase: fail if zero-copy is more than TOL% slower than copy.
TIME_LABEL="FAIL"
if [ "$BACKEND" = "spdk" ] && [ "${SPDK_PERF_GATE_TIME:-0}" != "1" ]; then
    TIME_LABEL="note (not gated on SPDK)"
fi
check() {  # copy_median zero_median phase-name
    awk -v c="$1" -v z="$2" -v tol="$TOL" -v ph="$3" -v lbl="$TIME_LABEL" 'BEGIN{
        d = (c>0)? (z-c)/c*100 : 0;
        printf("  %s delta=%+.1f%% (fail if worse than -%.1f%%)\n", ph, d, tol) > "/dev/stderr";
        if (d < -tol) { printf("%s: %s zero-copy is >%.1f%% slower than copy\n", lbl, ph, tol) > "/dev/stderr"; exit 1; }
        exit 0;
    }'
}

rc=0
check "$cpm" "$zpm" PUT || rc=1
check "$cgm" "$zgm" GET || rc=1
if [ "$BACKEND" = "spdk" ] && [ "${SPDK_PERF_GATE_TIME:-0}" != "1" ]; then
    rc=0  # throughput is I/O bound here; see the header
fi
if [ "$BACKEND" = "spdk" ]; then
    echo "  zero-copy I/Os bounced through a DMA copy: ${zc_bounced} (fail if any)" >&2
    if [ "$zc_bounced" -ne 0 ]; then
        echo "FAIL: the zero-copy path copied ${zc_bounced} I/Os through a bounce buffer" >&2
        rc=1
    fi
fi
if [ "$rc" -eq 0 ]; then
    if [ "$BACKEND" = "spdk" ]; then
        echo "OK: zero-copy put/get transferred straight from the store's DMA buffers" >&2
    else
        echo "OK: zero-copy within tolerance of copy on PUT and GET" >&2
    fi
fi
exit "$rc"

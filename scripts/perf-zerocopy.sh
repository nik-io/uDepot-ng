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
# Usage: perf-zerocopy.sh <backend> [build_dir] [ops] [iters] [tolerance%]
#   <backend>  aio or uring (posix also works)
set -uo pipefail

BACKEND="${1:?usage: perf-zerocopy.sh <aio|uring> [build_dir] [ops] [iters] [tol%]}"
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

# Run once; echo "PUT_mops GET_mops".
run_one() {  # $1 = extra flags ("" for copy, "--zero-copy" for zero-copy)
    rm -f "$FILE"
    local out
    out=$("$BIN" --backend "$BACKEND" -f "$FILE" -w "$OPS" --size "$SIZE" \
        --grain-size 512 --val-size "$VAL" $1 2>/dev/null)
    local p g
    p=$(grep '^PUTs' <<<"$out" | grep -oE 'Mops/sec=[0-9.]+' | cut -d= -f2)
    g=$(grep '^GETs' <<<"$out" | grep -oE 'Mops/sec=[0-9.]+' | cut -d= -f2)
    echo "$p $g"
}

median() { printf '%s\n' "$@" | sort -n | \
    awk '{a[NR]=$1} END{n=NR; if(n==0){print 0} else if(n%2){print a[(n+1)/2]} else {printf "%.6f",(a[n/2]+a[n/2+1])/2}}'; }

copy_p=(); copy_g=(); zc_p=(); zc_g=()
for _ in $(seq 1 "$ITERS"); do
    read -r cp cg <<<"$(run_one "")"
    read -r zp zg <<<"$(run_one "--zero-copy")"
    { [ -n "$cp" ] && [ -n "$cg" ] && [ -n "$zp" ] && [ -n "$zg" ]; } \
        || { echo "a udepot_ng_bench run produced no PUT/GET throughput" >&2; exit 2; }
    copy_p+=("$cp"); copy_g+=("$cg"); zc_p+=("$zp"); zc_g+=("$zg")
done

cpm=$(median "${copy_p[@]}"); zpm=$(median "${zc_p[@]}")
cgm=$(median "${copy_g[@]}"); zgm=$(median "${zc_g[@]}")

{
  echo "${BACKEND} Mops/sec over ${ITERS} interleaved runs @ ${OPS} ops of ${VAL} B on /dev/shm:"
  echo "  PUT copy median=${cpm}  zero-copy median=${zpm}"
  echo "  GET copy median=${cgm}  zero-copy median=${zgm}"
} >&2

# Gate each phase: fail if zero-copy is more than TOL% slower than copy.
check() {  # copy_median zero_median phase-name
    awk -v c="$1" -v z="$2" -v tol="$TOL" -v ph="$3" 'BEGIN{
        d = (c>0)? (z-c)/c*100 : 0;
        printf("  %s delta=%+.1f%% (fail if worse than -%.1f%%)\n", ph, d, tol) > "/dev/stderr";
        if (d < -tol) { printf("FAIL: %s zero-copy is >%.1f%% slower than copy\n", ph, tol) > "/dev/stderr"; exit 1; }
        exit 0;
    }'
}

rc=0
check "$cpm" "$zpm" PUT || rc=1
check "$cgm" "$zgm" GET || rc=1
if [ "$rc" -eq 0 ]; then echo "OK: zero-copy within tolerance of copy on PUT and GET" >&2; fi
exit "$rc"

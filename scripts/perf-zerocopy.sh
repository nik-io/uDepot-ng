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
# store's own DMA buffers, which SPDK counts: a zero-copy run must copy no
# I/O through a bounce buffer (exact). The throughput gate is the same as
# on the other backends, but measured inside one process (see run_compare).
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

# SPDK: copy and zero copy are compared inside one process, batch by batch
# (udepot_ng_bench --compare), not as separate runs: a separate process is
# a new connection to the NVMe-oF target, and the drift between processes
# was far larger than the copy measured (one ordering read as a 22-37%
# zero-copy loss). Each of ITERS processes reports its medians.
# The gate is on the per-run delta (median of its rounds' zero-copy/copy
# ratios), never on throughputs from different runs: runs differ by up to
# ~1.7x in absolute speed, so medians taken across them pair a fast run's
# copy with a slow run's zero copy.
run_compare() {  # echo "put_delta get_delta bounced"
    local out
    out=$("$BIN" --backend spdk -w "$SPDK_BATCH" --val-size "$VAL" \
        --grain-size 512 --compare "$SPDK_ROUNDS" 2>/dev/null)
    local dp dg b
    dp=$(grep '^CMP PUT' <<<"$out" | grep -oE 'delta=[-+0-9.]+' | cut -d= -f2)
    dg=$(grep '^CMP GET' <<<"$out" | grep -oE 'delta=[-+0-9.]+' | cut -d= -f2)
    b=$(grep '^BOUNCED' <<<"$out" | awk '{print $2}')
    echo "${dp#+} ${dg#+} $b"  # sort -n does not take a leading '+'
}
SPDK_BATCH="${SPDK_BATCH:-200}"    # ops per batch
SPDK_ROUNDS="${SPDK_ROUNDS:-15}"   # 2 x 15 x 200 x 32 KiB, well inside the namespace

copy_p=(); copy_g=(); zc_p=(); zc_g=(); zc_bounced=0
spdk_dp=(); spdk_dg=()
if [ "$BACKEND" = "spdk" ]; then
    for it in $(seq 1 "$ITERS"); do
        read -r dp dg zb <<<"$(run_compare)"
        { [ -n "$dp" ] && [ -n "$dg" ] && [ -n "$zb" ]; } \
            || { echo "a udepot_ng_bench --compare run produced no result" >&2; exit 2; }
        echo "  run $it: PUT delta=${dp}% GET delta=${dg}%" >&2
        spdk_dp+=("$dp"); spdk_dg+=("$dg")
        zc_bounced=$((zc_bounced + zb))
    done
fi
for it in $(seq 1 "$ITERS"); do
    [ "$BACKEND" = "spdk" ] && break
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

# Gate each phase: fail if zero-copy is more than TOL% slower than copy.
check_delta() {  # delta% phase-name
    awk -v d="$1" -v tol="$TOL" -v ph="$2" 'BEGIN{
        printf("  %s delta=%+.1f%% (fail if worse than -%.1f%%)\n", ph, d, tol) > "/dev/stderr";
        if (d < -tol) { printf("FAIL: %s zero-copy is >%.1f%% slower than copy\n", ph, tol) > "/dev/stderr"; exit 1; }
        exit 0;
    }'
}
delta() {  # copy zero -> percent
    awk -v c="$1" -v z="$2" 'BEGIN{ printf("%.4f", (c>0)? (z-c)/c*100 : 0) }'
}

rc=0
if [ "$BACKEND" = "spdk" ]; then
    echo "spdk: median of ${ITERS} runs, each the median of ${SPDK_ROUNDS} paired batches of ${SPDK_BATCH} ops of ${VAL} B, ${WHERE}:" >&2
    check_delta "$(median "${spdk_dp[@]}")" PUT || rc=1
    check_delta "$(median "${spdk_dg[@]}")" GET || rc=1
else
    cpm=$(median "${copy_p[@]}"); zpm=$(median "${zc_p[@]}")
    cgm=$(median "${copy_g[@]}"); zgm=$(median "${zc_g[@]}")
    {
      echo "${BACKEND} Mops/sec over ${ITERS} interleaved runs @ ${OPS} ops of ${VAL} B ${WHERE}:"
      echo "  PUT copy median=${cpm}  zero-copy median=${zpm}"
      echo "  GET copy median=${cgm}  zero-copy median=${zgm}"
    } >&2
    check_delta "$(delta "$cpm" "$zpm")" PUT || rc=1
    check_delta "$(delta "$cgm" "$zgm")" GET || rc=1
fi
if [ "$BACKEND" = "spdk" ]; then
    echo "  zero-copy I/Os bounced through a DMA copy: ${zc_bounced} (fail if any)" >&2
    if [ "$zc_bounced" -ne 0 ]; then
        echo "FAIL: the zero-copy path copied ${zc_bounced} I/Os through a bounce buffer" >&2
        rc=1
    fi
fi
if [ "$rc" -eq 0 ]; then
    echo "OK: zero-copy within tolerance of copy on PUT and GET" >&2
    [ "$BACKEND" = "spdk" ] && \
        echo "OK: zero-copy put/get transferred straight from the store's DMA buffers" >&2
fi
exit "$rc"

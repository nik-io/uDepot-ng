#!/usr/bin/env bash
#
# Zero-copy invariant, as uDepot's scripts/perf-zerocopy.sh: the zero-copy
# put/get interface (PutBuffer/GetBuffer) must not be slower than the
# copying one (spans), on PUT and GET.
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
# Usage: perf-zerocopy.sh <backend> [build_dir] [batch] [runs] [tolerance%]
#   <backend>  posix, aio, uring or spdk
#   [batch]    ops per batch (default 500; 200 on SPDK, whose namespace is
#              513 MiB); a run is 2 x ROUNDS batches per phase
set -uo pipefail

BACKEND="${1:?usage: perf-zerocopy.sh <posix|aio|uring|spdk> [build_dir] [batch] [runs] [tol%]}"
BUILD_DIR="${2:-build}"
DEFAULT_BATCH=500
[ "$BACKEND" = "spdk" ] && DEFAULT_BATCH=200
BATCH="${3:-$DEFAULT_BATCH}"
RUNS="${4:-5}"
TOL="${5:-5}"             # zero copy may be at most TOL% slower (noise band)
ROUNDS="${ROUNDS:-15}"    # 2 x 15 x 500 x 32 KiB fits the 1 GiB store
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
WHERE="on /dev/shm"
if [ "$BACKEND" = "spdk" ]; then
    if [ -z "${UDEPOT_NVMEF:-}" ]; then
        echo "FATAL: spdk needs UDEPOT_NVMEF; run it through scripts/spdk-nvmef-test.sh" >&2
        exit 2
    fi
    WHERE="on SPDK (${UDEPOT_NVMEF%%:*})"
fi

# One run; echo "put_delta get_delta bounced" (bounced: SPDK only, else 0).
run_compare() {
    rm -f "$FILE"
    local out
    out=$("$BIN" --backend "$BACKEND" -f "$FILE" --size "$SIZE" -w "$BATCH" \
        --val-size "$VAL" --grain-size 512 --compare "$ROUNDS" 2>/dev/null)
    local dp dg b
    dp=$(grep '^CMP PUT' <<<"$out" | grep -oE 'delta=[-+0-9.]+' | cut -d= -f2)
    dg=$(grep '^CMP GET' <<<"$out" | grep -oE 'delta=[-+0-9.]+' | cut -d= -f2)
    b=$(grep '^BOUNCED' <<<"$out" | awk '{print $2}')
    [ "$BACKEND" = "spdk" ] || b="${b:-0}"
    echo "${dp#+} ${dg#+} $b"  # sort -n does not take a leading +
}

median() { printf '%s\n' "$@" | sort -n | \
    awk '{a[NR]=$1} END{n=NR; if(n==0){print 0} else if(n%2){print a[(n+1)/2]} else {printf "%.6f",(a[n/2]+a[n/2+1])/2}}'; }

dps=(); dgs=(); bounced=0
for it in $(seq 1 "$RUNS"); do
    read -r dp dg b <<<"$(run_compare)"
    { [ -n "$dp" ] && [ -n "$dg" ] && [ -n "$b" ]; } \
        || { echo "a udepot_ng_bench --compare run produced no result" >&2; exit 2; }
    echo "  run $it: PUT delta=${dp}% GET delta=${dg}%" >&2
    dps+=("$dp"); dgs+=("$dg")
    bounced=$((bounced + b))
done

echo "${BACKEND}: median of ${RUNS} runs, each the median of ${ROUNDS} paired batches of ${BATCH} ops of ${VAL} B, ${WHERE}:" >&2

# Gate each phase: fail if zero copy is more than TOL% slower than copy.
check_delta() {  # delta% phase-name
    awk -v d="$1" -v tol="$TOL" -v ph="$2" 'BEGIN{
        printf("  %s delta=%+.1f%% (fail if worse than -%.1f%%)\n", ph, d, tol) > "/dev/stderr";
        if (d < -tol) { printf("FAIL: %s zero-copy is >%.1f%% slower than copy\n", ph, tol) > "/dev/stderr"; exit 1; }
        exit 0;
    }'
}

rc=0
check_delta "$(median "${dps[@]}")" PUT || rc=1
check_delta "$(median "${dgs[@]}")" GET || rc=1
if [ "$BACKEND" = "spdk" ]; then
    echo "  zero-copy I/Os bounced through a DMA copy: ${bounced} (fail if any)" >&2
    if [ "$bounced" -ne 0 ]; then
        echo "FAIL: the zero-copy path copied ${bounced} I/Os through a bounce buffer" >&2
        rc=1
    fi
fi
if [ "$rc" -eq 0 ]; then
    echo "OK: zero-copy within tolerance of copy on PUT and GET" >&2
    [ "$BACKEND" = "spdk" ] && \
        echo "OK: zero-copy put/get transferred straight from the store's DMA buffers" >&2
fi
exit "$rc"

#!/usr/bin/env bash
#
# Performance regression gate: uDepot-ng vs uDepot.
#
# Runs both stores interleaved on /dev/shm, compares median Mops/sec for
# PUT and GET, and fails if uDepot-ng is slower than uDepot on either
# operation (within a tolerance band).
#
# Usage: perf-regression.sh [ops] [iters] [tolerance-percent]
#
# Requires:
#   - uDepot-ng benchmark: build/udepot_ng_bench (cmake --build build)
#   - uDepot test binary:  $UDEPOT_ROOT/bin/udepot-test (make -C $UDEPOT_ROOT)
#     UDEPOT_ROOT defaults to ../uDepot
set -uo pipefail

OPS="${1:-10000}"
ITERS="${2:-9}"
TOL="${3:-5}"

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
NG_ROOT="$(cd "$SCRIPT_DIR/.." && pwd)"
NG_BIN="${NG_ROOT}/build/udepot_ng_bench"
UDEPOT_ROOT="${UDEPOT_ROOT:-${NG_ROOT}/../uDepot}"
UDEPOT_BIN="${UDEPOT_ROOT}/bin/udepot-test"

# uDepot needs cityhash on LD_LIBRARY_PATH.
CITYHASH_LIB="${UDEPOT_ROOT}/external/cityhash/src/.libs"
export LD_LIBRARY_PATH="${CITYHASH_LIB}${LD_LIBRARY_PATH:+:$LD_LIBRARY_PATH}"

NG_FILE="/dev/shm/udepot-ng-perf.store"
LEGACY_FILE="/dev/shm/udepot-legacy-perf.store"
SIZE=1077936129  # (1048576+4096)*1024+1
GRAIN=512
VAL=1024

trap 'rm -f "$NG_FILE" "$LEGACY_FILE"' EXIT

# Verify binaries exist.
for bin in "$NG_BIN" "$UDEPOT_BIN"; do
    if [ ! -x "$bin" ]; then
        echo "FATAL: binary not found: $bin" >&2
        exit 2
    fi
done

# Run uDepot-ng once; echo "PUT_mops GET_mops".
run_ng() {
    rm -f "$NG_FILE"
    local out
    out=$("$NG_BIN" -w "$OPS" -f "$NG_FILE" --size "$SIZE" \
        --grain-size "$GRAIN" --val-size "$VAL" 2>/dev/null)
    local p g
    p=$(grep 'PUTs aggregate' <<<"$out" | grep -oE 'Mops/sec=[0-9.]+' | cut -d= -f2)
    g=$(grep 'GETs aggregate' <<<"$out" | grep -oE 'Mops/sec=[0-9.]+' | cut -d= -f2)
    echo "$p $g"
}

# Run legacy uDepot once; echo "PUT_mops GET_mops".
# Uses uring backend (-u 6) with --thin for deterministic key gen, 1 thread.
run_legacy() {
    rm -f "$LEGACY_FILE"
    local out
    out=$("$UDEPOT_BIN" -u 6 -f "$LEGACY_FILE" -w "$OPS" -r "$OPS" \
        --size "$SIZE" -t 1 --trt-ntasks 32 --force-destroy \
        --grain-size "$GRAIN" --val-size "$VAL" --thin 2>/dev/null)
    local p g
    p=$(grep 'PUTs aggregate' <<<"$out" | grep -oE 'Mops/sec=[0-9.]+' | cut -d= -f2)
    g=$(grep 'GETs aggregate' <<<"$out" | grep -oE 'Mops/sec=[0-9.]+' | cut -d= -f2)
    echo "$p $g"
}

median() {
    printf '%s\n' "$@" | sort -n | \
        awk '{a[NR]=$1} END{
            n=NR;
            if(n==0){print 0}
            else if(n%2){print a[(n+1)/2]}
            else {printf "%.6f",(a[n/2]+a[n/2+1])/2}
        }'
}

ng_p=(); ng_g=(); leg_p=(); leg_g=()
for iter in $(seq 1 "$ITERS"); do
    # Interleave: legacy first, then ng, to cancel drift.
    read -r lp lg <<<"$(run_legacy)"
    read -r np ng_ <<<"$(run_ng)"
    { [ -n "$lp" ] && [ -n "$lg" ] && [ -n "$np" ] && [ -n "$ng_" ]; } \
        || { echo "a run produced no PUT/GET throughput (iter $iter)" >&2; exit 2; }
    leg_p+=("$lp"); leg_g+=("$lg"); ng_p+=("$np"); ng_g+=("$ng_")
done

lpm=$(median "${leg_p[@]}"); npm=$(median "${ng_p[@]}")
lgm=$(median "${leg_g[@]}"); ngm=$(median "${ng_g[@]}")

{
  echo "Mops/sec over ${ITERS} interleaved runs @ ${OPS} ops on /dev/shm:"
  echo "  PUT  uDepot median=${lpm}  uDepot-ng median=${npm}"
  echo "  GET  uDepot median=${lgm}  uDepot-ng median=${ngm}"
} >&2

# Gate: fail if uDepot-ng is more than TOL% slower than uDepot.
check() {  # legacy_median ng_median phase-name
    awk -v leg="$1" -v ng="$2" -v tol="$TOL" -v ph="$3" 'BEGIN{
        d = (leg>0)? (ng-leg)/leg*100 : 0;
        printf("  %s delta=%+.1f%% (fail if worse than -%.1f%%)\n", ph, d, tol) > "/dev/stderr";
        if (d < -tol) { printf("FAIL: %s uDepot-ng is >%.1f%% slower than uDepot\n", ph, tol) > "/dev/stderr"; exit 1; }
        exit 0;
    }'
}

rc=0
check "$lpm" "$npm" PUT || rc=1
check "$lgm" "$ngm" GET || rc=1
if [ "$rc" -eq 0 ]; then
    echo "OK: uDepot-ng within tolerance of uDepot on PUT and GET" >&2
fi
exit "$rc"

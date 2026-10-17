#!/usr/bin/env bash
#
# Performance regression gate: uDepot-ng vs uDepot (legacy).
#
# Runs both stores interleaved on /dev/shm, compares median Mops/sec for
# PUT, GET, EXISTS, and DEL, and fails if uDepot-ng is slower than uDepot
# on any operation (within a tolerance band).
#
# Usage: perf-regression.sh [ops] [iters] [tolerance-percent] [backend] [qd]
#
# backend: posix (default), aio, uring or spdk. uDepot runs the last three
# on TRT, as its udepot-test does; spdk uses the NVMe-oF namespace that
# UDEPOT_NVMEF names (scripts/spdk-nvmef-test.sh starts one), on both sides.
# qd: operations each side keeps in flight (default 1); uDepot's TRT tasks,
# udepot-test's --trt-ntasks, whose default is 128.
#
# Requires:
#   - build/udepot_ng_bench     (cmake --build build)
#   - build/udepot_legacy_bench (cmake --build build, needs UDEPOT_ROOT)
#
# Legacy uDepot must be built at BUILD_TYPE=PERFORMANCE (-O3 -DNDEBUG) so
# the comparison is apples-to-apples with uDepot-ng's cmake Release build.
# This script rebuilds it automatically when UDEPOT_ROOT is set or defaults
# to ../uDepot.
set -uo pipefail

OPS="${1:-5000}"
ITERS="${2:-9}"
TOL="${3:-5}"
BACKEND="${4:-posix}"
QD="${5:-1}"

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
NG_ROOT="$(cd "$SCRIPT_DIR/.." && pwd)"
# SPDK needs uDepot-ng built with -DUDEPOT_BUILD_SPDK=ON (Release), and
# uDepot built with BUILD_SPDK=1; NG_BUILD overrides the build directory.
LEGACY_SPDK=0
if [ "$BACKEND" = "spdk" ]; then
    LEGACY_SPDK=1
    NG_BUILD="${NG_BUILD:-${NG_ROOT}/build-spdk-rel}"
    [ -n "${UDEPOT_NVMEF:-}" ] || {
        echo "FATAL: spdk needs UDEPOT_NVMEF (run via scripts/spdk-nvmef-test.sh <build> regression)" >&2
        exit 2
    }
fi
NG_BUILD="${NG_BUILD:-${NG_ROOT}/build}"
NG_BIN="${NG_BUILD}/udepot_ng_bench"
LEGACY_BIN="${NG_BUILD}/udepot_legacy_bench"

# Ensure legacy uDepot is built at -O3 -DNDEBUG (BUILD_TYPE=PERFORMANCE).
UDEPOT_ROOT="${UDEPOT_ROOT:-${NG_ROOT}/../uDepot}"
if [ -d "$UDEPOT_ROOT" ]; then
    echo "Building legacy uDepot at BUILD_TYPE=PERFORMANCE BUILD_SPDK=$LEGACY_SPDK..." >&2
    make -C "$UDEPOT_ROOT" BUILD_TYPE=PERFORMANCE BUILD_SPDK="$LEGACY_SPDK" \
        -j"$(nproc)" 2>&1 | tail -1 >&2
    # Reconfigure (the legacy bench links SPDK only if uDepot was built
    # with it), then rebuild both benchmarks against the fresh library.
    cmake "$NG_BUILD" > /dev/null
    cmake --build "$NG_BUILD" --target udepot_legacy_bench udepot_ng_bench \
        2>&1 | tail -1 >&2
fi

NG_FILE="/dev/shm/udepot-ng-perf.store"
LEGACY_FILE="/dev/shm/udepot-legacy-perf.store"
SIZE=1077936129
GRAIN=512
VAL=1024

trap 'rm -f "$NG_FILE" "$LEGACY_FILE"' EXIT

for bin in "$NG_BIN" "$LEGACY_BIN"; do
    if [ ! -x "$bin" ]; then
        echo "FATAL: binary not found: $bin" >&2
        exit 2
    fi
done

extract() {
    local phase="$1" output="$2"
    grep "${phase}s Aggregate" <<<"$output" | grep -oE 'Mops/sec=[0-9.]+' | cut -d= -f2
}

run_ng() {
    rm -f "$NG_FILE"
    local out
    out=$("$NG_BIN" -w "$OPS" -f "$NG_FILE" --size "$SIZE" \
        --grain-size "$GRAIN" --val-size "$VAL" \
        --backend "$BACKEND" --qd "$QD" 2>/dev/null)
    echo "$(extract PUT "$out") $(extract GET "$out") $(extract EXIST "$out") $(extract DEL "$out")"
}

run_legacy() {
    rm -f "$LEGACY_FILE"
    local out
    out=$("$LEGACY_BIN" -w "$OPS" -f "$LEGACY_FILE" --size "$SIZE" \
        --grain-size "$GRAIN" --val-size "$VAL" \
        --backend "$BACKEND" --qd "$QD" 2>/dev/null)
    echo "$(extract PUT "$out") $(extract GET "$out") $(extract EXIST "$out") $(extract DEL "$out")"
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

ng_p=(); ng_g=(); ng_e=(); ng_d=()
leg_p=(); leg_g=(); leg_e=(); leg_d=()

echo "Running $ITERS interleaved iterations @ $OPS ops each, backend=$BACKEND qd=$QD..." >&2
for iter in $(seq 1 "$ITERS"); do
    read -r lp lg le ld <<<"$(run_legacy)"
    read -r np ng_ ne nd <<<"$(run_ng)"
    for v in "$lp" "$lg" "$le" "$ld" "$np" "$ng_" "$ne" "$nd"; do
        if [ -z "$v" ]; then
            echo "FATAL: a run produced no throughput (iter $iter)" >&2
            exit 2
        fi
    done
    leg_p+=("$lp"); leg_g+=("$lg"); leg_e+=("$le"); leg_d+=("$ld")
    ng_p+=("$np"); ng_g+=("$ng_"); ng_e+=("$ne"); ng_d+=("$nd")
    printf "  iter %d/%d done\n" "$iter" "$ITERS" >&2
done

lpm=$(median "${leg_p[@]}"); npm=$(median "${ng_p[@]}")
lgm=$(median "${leg_g[@]}"); ngm=$(median "${ng_g[@]}")
lem=$(median "${leg_e[@]}"); nem=$(median "${ng_e[@]}")
ldm=$(median "${leg_d[@]}"); ndm=$(median "${ng_d[@]}")

{
  echo ""
  echo "Mops/sec medians over ${ITERS} interleaved runs @ ${OPS} ops, backend=${BACKEND} qd=${QD}:"
  printf "  %-7s uDepot=%-12s uDepot-ng=%-12s\n" PUT "$lpm" "$npm"
  printf "  %-7s uDepot=%-12s uDepot-ng=%-12s\n" GET "$lgm" "$ngm"
  printf "  %-7s uDepot=%-12s uDepot-ng=%-12s\n" EXISTS "$lem" "$nem"
  printf "  %-7s uDepot=%-12s uDepot-ng=%-12s\n" DEL "$ldm" "$ndm"
} >&2

check() {
    awk -v leg="$1" -v ng="$2" -v tol="$TOL" -v ph="$3" 'BEGIN{
        d = (leg>0)? (ng-leg)/leg*100 : 0;
        printf("  %s delta=%+.1f%% (fail if worse than -%.1f%%)\n", ph, d, tol) > "/dev/stderr";
        if (d < -tol) { printf("FAIL: %s uDepot-ng is >%.1f%% slower than uDepot\n", ph, tol) > "/dev/stderr"; exit 1; }
        exit 0;
    }'
}

rc=0
check "$lpm" "$npm" PUT    || rc=1
check "$lgm" "$ngm" GET    || rc=1
check "$lem" "$nem" EXISTS || rc=1
check "$ldm" "$ndm" DEL    || rc=1

echo "" >&2
if [ "$rc" -eq 0 ]; then
    echo "OK: uDepot-ng within tolerance of uDepot on all operations" >&2
else
    echo "FAILED: uDepot-ng regressed on one or more operations" >&2
fi
exit "$rc"

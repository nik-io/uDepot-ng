#!/usr/bin/env bash
#
# Put latency while the directory grows: incremental resize (paper §4.3)
# against the freeze-and-copy grow it replaced, with a no-grow baseline.
#
# The point of the incremental resize is the tail. A freeze stalls writers
# for the whole copy (tens of ms at 16 -> 32 tables of 2^17 buckets); an
# incremental resize spreads the work over the writes that touch each
# stripe. This fails unless the incremental resize's median p95 put latency
# is under a quarter of the freeze's.
#
# Why open loop at a calibrated rate, and why p95:
#   - A closed loop (each writer waits for its put, then issues the next)
#     hides a stall: a writer stuck for 100 ms has one slow put, and the
#     puts it would have issued meanwhile are never measured (coordinated
#     omission). Every percentile below the max then looks the same in all
#     modes. So writers issue puts at a fixed rate and a put's latency counts
#     from when it was due (udepot_ng_bench --rate).
#   - A stall then delays every put due during it, and the backlog after.
#     That reaches p95 only if it is over 5% of the puts, which takes a
#     load the freeze cannot hide in: LOAD (default 0.6) of the capacity
#     a closed-loop no-grow run measures first. At ~0.35 p95 was ~11 us in
#     every mode; the freeze showed only from p99 up.
#   - Two writers on /dev/shm, leaving CPUs to salsa's allocator and the
#     space waker: at four writers on four CPUs even the no-grow baseline's
#     p99 was milliseconds.
#
# Each run: 2 writers, 2M puts, the directory growing from 1 to 32 tables
# (no-grow starts at 32). Runs are interleaved across the modes.
#
# Usage: perf-resize-latency.sh [build_dir] [runs]
set -uo pipefail

BUILD_DIR="${1:-build}"
RUNS="${2:-5}"
LOAD="${LOAD:-0.6}"
HERE="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
case "$BUILD_DIR" in
    /*) BIN="$BUILD_DIR/udepot_ng_bench" ;;
    *)  BIN="$HERE/$BUILD_DIR/udepot_ng_bench" ;;
esac
[ -x "$BIN" ] || { echo "FATAL: $BIN not found" >&2; exit 2; }
FILE="/dev/shm/udepot-ng-resize-latency.store"
trap 'rm -f "$FILE"' EXIT
COMMON=(--backend posix -f "$FILE" -w 1000000 --threads 2 --grain-size 64
        --val-size 16 --index-bits 17)

mode_args() {
    case "$1" in
        nogrow) echo "--initial-tables 32" ;;
        *)      echo "--initial-tables 1 --resize-mode $1" ;;
    esac
}

# Capacity: closed-loop puts/s per writer with no grow.
rm -f "$FILE"
mops=$("$BIN" "${COMMON[@]}" $(mode_args nogrow) 2>/dev/null |
       awk '/^PUTs Aggregate/ { for (i = 1; i <= NF; ++i)
                                  if ($i ~ /^Mops\/sec=/) { split($i, kv, "=");
                                                           print kv[2] } }')
[ -n "$mops" ] || { echo "FATAL: the calibration run produced no throughput" >&2; exit 2; }
RATE=$(awk -v m="$mops" -v l="$LOAD" 'BEGIN { printf "%d", m * 1e6 / 2 * l }')
echo "capacity ${mops} Mops/s (2 writers); open loop at ${RATE} puts/s per writer (load ${LOAD})" >&2

run() {  # mode -> "p95 p99 max"
    rm -f "$FILE"
    "$BIN" "${COMMON[@]}" $(mode_args "$1") --latency --rate "$RATE" \
        2>/dev/null | awk '/^PUT latency/ {
            for (i = 1; i <= NF; ++i) { split($i, kv, "="); v[kv[1]] = kv[2] }
            print v["p95"], v["p99"], v["max"] }'
}

median() { printf '%s\n' "$@" | sort -n | \
    awk '{a[NR]=$1} END{print (NR%2) ? a[(NR+1)/2] : (a[NR/2]+a[NR/2+1])/2}'; }

declare -A p95 p99 max
for r in $(seq 1 "$RUNS"); do
    for m in incremental freeze nogrow; do
        read -r a b c <<<"$(run "$m")"
        [ -n "$c" ] || { echo "FATAL: a $m run produced no latency" >&2; exit 2; }
        echo "  run $r $m: p95=${a}us p99=${b}us max=${c}us" >&2
        p95[$m]+="$a "; p99[$m]+="$b "; max[$m]+="$c "
    done
done

echo "put latency, median of $RUNS runs (us):" >&2
for m in incremental freeze nogrow; do
    printf '  %-11s p95=%s p99=%s max=%s\n' "$m" "$(median ${p95[$m]})" \
        "$(median ${p99[$m]})" "$(median ${max[$m]})" >&2
done
inc=$(median ${p95[incremental]}); frz=$(median ${p95[freeze]})
if awk -v i="$inc" -v f="$frz" 'BEGIN{exit !(i * 4 < f)}'; then
    echo "OK: the incremental resize's p95 ($inc us) is under a quarter of the freeze's ($frz us)" >&2
    exit 0
fi
echo "FAIL: the incremental resize's p95 ($inc us) is not under a quarter of the freeze's ($frz us)" >&2
exit 1

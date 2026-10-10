#!/usr/bin/env bash
#
# Put latency while the directory grows: incremental resize (paper §4.3)
# against the freeze-and-copy grow it replaced, with a no-grow baseline.
#
# The point of the incremental resize is the tail. A freeze stalls every
# writer that meets it for the whole copy (~100 ms at 16 -> 32 tables of
# 2^18 buckets); an incremental resize spreads the work over the writes that
# touch each stripe, so its worst put should look like the no-grow
# baseline's. This fails unless the median (over runs) worst put of the
# incremental resize is under a quarter of the freeze's. "Merely below" is
# too weak: when the space waker initialized the new tables up front, the
# writers that met a full table waited ~250 ms for it, which was still
# below the freeze's worst (~320 ms) in most runs.
#
# Each run: 4 writers, 4M puts on /dev/shm, the directory growing from 1 to
# 32 tables (no-grow starts at 32). Runs are interleaved across the modes.
#
# Usage: perf-resize-latency.sh [build_dir] [runs]
set -uo pipefail

BUILD_DIR="${1:-build}"
RUNS="${2:-5}"
HERE="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
case "$BUILD_DIR" in
    /*) BIN="$BUILD_DIR/udepot_ng_bench" ;;
    *)  BIN="$HERE/$BUILD_DIR/udepot_ng_bench" ;;
esac
[ -x "$BIN" ] || { echo "FATAL: $BIN not found" >&2; exit 2; }
FILE="/dev/shm/udepot-ng-resize-latency.store"
trap 'rm -f "$FILE"' EXIT

run() {  # mode -> "max p9999 p999 over_1ms"
    local args
    case "$1" in
        nogrow) args="--initial-tables 32" ;;
        *)      args="--initial-tables 1 --resize-mode $1" ;;
    esac
    rm -f "$FILE"
    "$BIN" --backend posix -f "$FILE" -w 1000000 --threads 4 \
        --grain-size 64 --val-size 16 --index-bits 18 $args --latency \
        2>/dev/null | awk '/^PUT latency/ {
            for (i = 1; i <= NF; ++i) { split($i, kv, "="); v[kv[1]] = kv[2] }
            print v["max"], v["p99.99"], v["p99.9"], v["over_1ms"] }'
}

median() { printf '%s\n' "$@" | sort -n | \
    awk '{a[NR]=$1} END{print (NR%2) ? a[(NR+1)/2] : (a[NR/2]+a[NR/2+1])/2}'; }

declare -A max p9999 p999 over
for r in $(seq 1 "$RUNS"); do
    for m in incremental freeze nogrow; do
        read -r a b c d <<<"$(run "$m")"
        [ -n "$d" ] || { echo "FATAL: a $m run produced no latency" >&2; exit 2; }
        echo "  run $r $m: max=${a}us p99.99=${b}us p99.9=${c}us over_1ms=$d" >&2
        max[$m]+="$a "; p9999[$m]+="$b "; p999[$m]+="$c "; over[$m]+="$d "
    done
done

echo "put latency, median of $RUNS runs (us):" >&2
for m in incremental freeze nogrow; do
    printf '  %-11s max=%s p99.99=%s p99.9=%s over_1ms=%s\n' "$m" \
        "$(median ${max[$m]})" "$(median ${p9999[$m]})" \
        "$(median ${p999[$m]})" "$(median ${over[$m]})" >&2
done
inc=$(median ${max[incremental]}); frz=$(median ${max[freeze]})
if awk -v i="$inc" -v f="$frz" 'BEGIN{exit !(i * 4 < f)}'; then
    echo "OK: the incremental resize's worst put ($inc us) is under a quarter of the freeze's ($frz us)" >&2
    exit 0
fi
echo "FAIL: the incremental resize's worst put ($inc us) is not under a quarter of the freeze's ($frz us)" >&2
exit 1

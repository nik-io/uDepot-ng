#!/usr/bin/env bash
#
# End-to-end test of the SPDK backend against a software NVMe-over-Fabrics
# target -- no NVMe hardware required.
#
# It starts an SPDK nvmf_tgt that exports a RAM-backed malloc bdev as an NVMe
# namespace over TCP loopback, then runs the SPDK store test as a fabrics
# initiator and checks that PUTs/GETs complete and the store shuts down cleanly.
#
# The SPDK backend learns the target from the UDEPOT_NVMEF environment variable
# (traddr:trsvcid:subnqn), so the test binary needs no command-line change.
#
# Requirements: a build with -DUDEPOT_BUILD_SPDK=ON, a built SPDK tree under
# extern/spdk (configured with --target-arch=x86-64-v2, not the default
# -march=native, so it runs on whatever host the container lands on), and
# root (for the target). Hugepages are used when they can be
# reserved; otherwise both the target and the initiator run with --no-huge
# (UDEPOT_SPDK_NO_HUGE=1), e.g. in containers without hugetlbfs. CI runs it
# under sudo.
#
# Usage: scripts/spdk-nvmef-test.sh [build_dir]
set -uo pipefail

BUILD_DIR="${1:-build}"
HERE="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
SPDK_DIR="${SPDK_DIR:-$HERE/extern/spdk}"
RPC="$SPDK_DIR/scripts/rpc.py"
TGT_BIN="$SPDK_DIR/build/bin/nvmf_tgt"
DPDK_LIB="$SPDK_DIR/dpdk/build/lib"
# The SPDK tests to run, as CMake lists them (the `spdk_tests` target).
TEST_LIST="$HERE/$BUILD_DIR/spdk_tests.txt"

NQN="nqn.2016-06.io.spdk:cnode1"
TADDR="127.0.0.1"
TPORT="4420"
# 513 MiB, deliberately NOT a multiple of the segment size: uDepot puts device
# metadata in the tail after align_down(dev, seg*grain), so an exactly divisible
# size leaves no room and init fails with "Not enough spare capacity".
BDEV_MB="513"
SECTOR="512"

TGT_LOG="$(mktemp /tmp/nvmf_tgt.XXXXXX.log)"
TGT_PID=""
HUGE_PREEXISTING="no"
# SPDK_NVMEF_NO_HUGE=1 forces the --no-huge path even where hugepages work.
NO_HUGE="no"
[ "${SPDK_NVMEF_NO_HUGE:-0}" = "1" ] && NO_HUGE="yes"

log() { echo "[spdk-nvmef-test] $*"; }
fail() { echo "[spdk-nvmef-test] FAIL: $*" >&2; exit 1; }

cleanup() {
    local rc=$?
    [ -n "$TGT_PID" ] && kill "$TGT_PID" 2>/dev/null
    for _ in 1 2 3 4 5; do kill -0 "$TGT_PID" 2>/dev/null || break; sleep 0.3; done
    # By pid only: a pattern kill would also hit any shell whose command
    # line mentions the target binary.
    [ -n "$TGT_PID" ] && kill -9 "$TGT_PID" 2>/dev/null
    if [ "$HUGE_PREEXISTING" = "no" ] && [ "$NO_HUGE" = "no" ]; then
        echo 0 > /proc/sys/vm/nr_hugepages 2>/dev/null || true
    fi
    [ $rc -ne 0 ] && [ -f "$TGT_LOG" ] && { echo "--- target log tail ---" >&2; tail -20 "$TGT_LOG" >&2; }
    rm -f "$TGT_LOG"
    exit $rc
}
trap cleanup EXIT INT TERM

[ -f "$TEST_LIST" ] || fail "$TEST_LIST not found -- configure with -DUDEPOT_BUILD_SPDK=ON"
TESTS=()
while IFS= read -r name; do
    [ -n "$name" ] || continue
    t="$HERE/$BUILD_DIR/$name"
    [ -x "$t" ] || fail "$t not found -- build the spdk_tests target"
    TESTS+=("$t")
done < "$TEST_LIST"
[ ${#TESTS[@]} -gt 0 ] || fail "$TEST_LIST lists no tests"
BENCH="$HERE/$BUILD_DIR/udepot_ng_bench"
[ -x "$BENCH" ] || fail "$BENCH not found -- build the spdk_tests target"
[ -x "$TGT_BIN" ]    || fail "$TGT_BIN not found -- build SPDK first"

# ── hugepages ────────────────────────────────────────────────────────────────
CUR_HUGE="$(cat /proc/sys/vm/nr_hugepages 2>/dev/null || echo 0)"
if [ "$NO_HUGE" = "yes" ]; then
    :
elif [ "$CUR_HUGE" -ge 512 ]; then
    HUGE_PREEXISTING="yes"
else
    log "reserving hugepages"
    echo 1024 > /proc/sys/vm/nr_hugepages 2>/dev/null || true
    [ "$(cat /proc/sys/vm/nr_hugepages 2>/dev/null || echo 0)" -ge 512 ] || NO_HUGE="yes"
fi
if [ "$NO_HUGE" = "no" ] && ! mount | grep -q 'hugetlbfs'; then
    mkdir -p /dev/hugepages
    mount -t hugetlbfs nodev /dev/hugepages 2>/dev/null || NO_HUGE="yes"
fi
TGT_ARGS=(-m 0x1)
INIT_ENV=()
if [ "$NO_HUGE" = "yes" ]; then
    log "no hugepages available: running target and initiator with --no-huge"
    # The malloc bdev lives in the target's memory pool, so size it to fit.
    TGT_ARGS+=(--no-huge -s 2048)
    INIT_ENV=(UDEPOT_SPDK_NO_HUGE=1)
fi

# ── start the target (pinned to core 0) ─────────────────────────────────────
log "starting nvmf_tgt"
"$TGT_BIN" "${TGT_ARGS[@]}" > "$TGT_LOG" 2>&1 &
TGT_PID=$!
# Wait until the RPC server actually answers.
ready="no"
for i in $(seq 1 40); do
    if ! kill -0 "$TGT_PID" 2>/dev/null; then
        grep -q "unsupported cpu type" "$TGT_LOG" &&
            fail "SPDK was built for another CPU (-march=native): rebuild it with ./configure --target-arch=x86-64-v2"
        fail "nvmf_tgt exited during startup"
    fi
    if test -S /var/tmp/spdk.sock && "$RPC" spdk_get_version >/dev/null 2>&1; then
        ready="yes"; break
    fi
    sleep 1
done
[ "$ready" = "yes" ] || fail "nvmf_tgt RPC did not become ready"

# ── configure: TCP transport, malloc bdev, subsystem, listener ──────────────
log "configuring target ($BDEV_MB MiB malloc bdev over TCP $TADDR:$TPORT)"
"$RPC" nvmf_create_transport -t TCP                              || fail "create_transport"
"$RPC" bdev_malloc_create "$BDEV_MB" "$SECTOR" -b Malloc0        || fail "bdev_malloc_create"
"$RPC" nvmf_create_subsystem "$NQN" -a -s SPDK00000000000001    || fail "create_subsystem"
"$RPC" nvmf_subsystem_add_ns "$NQN" Malloc0                      || fail "add_ns"
"$RPC" nvmf_subsystem_add_listener "$NQN" -t tcp -a "$TADDR" -s "$TPORT" || fail "add_listener"

# ── run the SPDK store test as the fabrics initiator ────────────────────────
NCPU="$(nproc)"
PIN=()
if [ "$NCPU" -ge 3 ]; then
    PIN=(taskset -c "1-$((NCPU-1))")
fi
for t in "${TESTS[@]}"; do
    log "running $(basename "$t") on ${NCPU} cpus"
    # Bounded: a run normally takes well under a minute. A per-I/O stall
    # (the initiator's EAL once pinned it onto the target's core, costing
    # ~10 ms per I/O) shows up as a timeout, not as a slow pass.
    env "${INIT_ENV[@]}" UDEPOT_NVMEF="$TADDR:$TPORT:$NQN" \
        LD_LIBRARY_PATH="$DPDK_LIB" timeout "${TEST_TIMEOUT:-300}" \
        "${PIN[@]}" "$t"
    rc=$?
    [ $rc -eq 0 ] || fail "$(basename "$t") failed (rc=$rc)"
done

# ── zero-copy gate over SPDK ────────────────────────────────────────────────
# scripts/perf-zerocopy.sh spdk: zero copy must not be slower than copy on
# PUT or GET (compared batch by batch inside each run, against the target),
# and must bounce no I/O through a DMA copy. Each run starts a fresh store.
log "zero-copy gate over SPDK"
env "${INIT_ENV[@]}" UDEPOT_NVMEF="$TADDR:$TPORT:$NQN" \
    LD_LIBRARY_PATH="$DPDK_LIB" timeout "${PERF_TIMEOUT:-900}" \
    "${PIN[@]}" "$HERE/scripts/perf-zerocopy.sh" spdk "$HERE/$BUILD_DIR" \
    "" "${SPDK_PERF_ITERS:-5}"
rc=$?
[ $rc -eq 0 ] || fail "SPDK zero-copy gate failed (rc=$rc)"

# Keep-alive timeouts on the target mean the initiator stopped polling the
# admin queue: the fabrics connection was dropped. A correct run has none.
if grep -q "keep alive timeout" "$TGT_LOG"; then
    fail "target reported a keep-alive timeout (initiator stopped polling)"
fi

log "OK: SPDK backend tests and zero-copy gate passed over the soft NVMe-oF target"

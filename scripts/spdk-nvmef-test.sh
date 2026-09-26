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
# extern/spdk, hugepages, and root (for hugepages and the target). CI runs
# it under sudo.
#
# Usage: scripts/spdk-nvmef-test.sh [build_dir]
set -uo pipefail

BUILD_DIR="${1:-build}"
HERE="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
SPDK_DIR="${SPDK_DIR:-$HERE/extern/spdk}"
RPC="$SPDK_DIR/scripts/rpc.py"
TGT_BIN="$SPDK_DIR/build/bin/nvmf_tgt"
DPDK_LIB="$SPDK_DIR/dpdk/build/lib"
STORE_TEST="$HERE/$BUILD_DIR/spdk_store_test"

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

log() { echo "[spdk-nvmef-test] $*"; }
fail() { echo "[spdk-nvmef-test] FAIL: $*" >&2; exit 1; }

cleanup() {
    local rc=$?
    [ -n "$TGT_PID" ] && kill "$TGT_PID" 2>/dev/null
    for _ in 1 2 3 4 5; do kill -0 "$TGT_PID" 2>/dev/null || break; sleep 0.3; done
    kill -9 "$TGT_PID" 2>/dev/null
    pkill -9 -f "nvmf_tgt" 2>/dev/null
    if [ "$HUGE_PREEXISTING" = "no" ]; then
        echo 0 > /proc/sys/vm/nr_hugepages 2>/dev/null || true
    fi
    [ $rc -ne 0 ] && [ -f "$TGT_LOG" ] && { echo "--- target log tail ---" >&2; tail -20 "$TGT_LOG" >&2; }
    rm -f "$TGT_LOG"
    exit $rc
}
trap cleanup EXIT INT TERM

[ -x "$STORE_TEST" ] || fail "$STORE_TEST not found -- build with -DUDEPOT_BUILD_SPDK=ON"
[ -x "$TGT_BIN" ]    || fail "$TGT_BIN not found -- build SPDK first"

# ── hugepages ────────────────────────────────────────────────────────────────
CUR_HUGE="$(cat /proc/sys/vm/nr_hugepages 2>/dev/null || echo 0)"
if [ "$CUR_HUGE" -ge 512 ]; then
    HUGE_PREEXISTING="yes"
else
    log "reserving hugepages"
    echo 1024 > /proc/sys/vm/nr_hugepages || fail "cannot reserve hugepages (need root)"
fi
if ! mount | grep -q 'hugetlbfs'; then
    mkdir -p /dev/hugepages
    mount -t hugetlbfs nodev /dev/hugepages || fail "cannot mount hugetlbfs"
fi

# ── start the target (pinned to core 0) ─────────────────────────────────────
log "starting nvmf_tgt"
"$TGT_BIN" -m 0x1 > "$TGT_LOG" 2>&1 &
TGT_PID=$!
# Wait until the RPC server actually answers.
ready="no"
for i in $(seq 1 40); do
    kill -0 "$TGT_PID" 2>/dev/null || fail "nvmf_tgt exited during startup"
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
log "running spdk_store_test on ${NCPU} cpus"
UDEPOT_NVMEF="$TADDR:$TPORT:$NQN" LD_LIBRARY_PATH="$DPDK_LIB" \
    "${PIN[@]}" "$STORE_TEST"
rc=$?
[ $rc -eq 0 ] || fail "spdk_store_test failed (rc=$rc)"

# Keep-alive timeouts on the target mean the initiator stopped polling the
# admin queue: the fabrics connection was dropped. A correct run has none.
if grep -q "keep alive timeout" "$TGT_LOG"; then
    fail "target reported a keep-alive timeout (initiator stopped polling)"
fi

log "OK: SPDK backend PUT/GET/shutdown succeeded over the soft NVMe-oF target"

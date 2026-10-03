#!/usr/bin/env bash
#
# Fail if a test CI expects was not built. A missing dependency silently
# drops a backend or binding (CMake disables io_uring when liburing is
# absent), and the run would pass while covering less.
#
# Usage: scripts/ci-check-tests-built.sh <build_dir> [extra test...]
set -euo pipefail

BUILD_DIR="$1"
shift
EXPECTED=(aio_store_test uring_store_test concurrent_uring_store_test
          store_gc_test store_recovery_test memcache_test
          rcu_test_reader_fence aio_queue_depth_test uring_queue_depth_test
          "$@")

listed="$(ctest --test-dir "$BUILD_DIR" -N)"
missing=0
for t in "${EXPECTED[@]}"; do
    if ! grep -q " ${t}\$" <<<"$listed"; then
        echo "::error::${t} was not built"
        missing=1
    fi
done
exit "$missing"

#!/usr/bin/env bash
# Usage: ci-check-no-gtest-download.sh <build-dir>
#
# Run after configure in CI: fails if CMake downloaded googletest instead of
# using the copy from scripts/ci-fetch-googletest.sh
# (-DFETCHCONTENT_SOURCE_DIR_GOOGLETEST). Without this, a dropped flag or a
# renamed variable would quietly bring back the configure-time download that a
# GitHub outage turned into a CI failure.
#
# The signal: FetchContent populates a download through a sub-build in
# <build-dir>/_deps/googletest-subbuild. With a source dir given it skips
# population, and that directory is never created. (The configure log does
# not say "Performing download step"; that text only shows in the sub-build.)
set -euo pipefail

if [ $# -ne 1 ]; then
    echo "usage: $0 <build-dir>" >&2
    exit 2
fi
build=$1

if [ ! -d "$build/_deps/googletest-build" ]; then
    echo "error: $build has no googletest build; was it configured with tests?" >&2
    exit 1
fi
if [ -e "$build/_deps/googletest-subbuild" ]; then
    echo "error: CMake downloaded googletest at configure time" >&2
    echo "  pass -DFETCHCONTENT_SOURCE_DIR_GOOGLETEST after running scripts/ci-fetch-googletest.sh" >&2
    exit 1
fi
echo "googletest came from FETCHCONTENT_SOURCE_DIR_GOOGLETEST, not a download"

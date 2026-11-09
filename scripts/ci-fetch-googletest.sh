#!/usr/bin/env bash
# Usage: ci-fetch-googletest.sh <dest-dir>
#
# Fetches the pinned googletest source into <dest-dir> for CI, which then
# passes it to CMake as -DFETCHCONTENT_SOURCE_DIR_GOOGLETEST=<dest-dir>.
#
# Why: CMakeLists.txt fetches googletest from a GitHub release URL at
# configure time, with no retry and no cache. GitHub answered that download
# with HTTP 5xx on PR #11 and killed three jobs before they built anything. A
# transient outage should not fail CI. Here the download retries, is checked
# against a SHA-256 pin, and is cached by the workflow (actions/cache), so a
# cache hit needs no network at all. CMakeLists.txt is unchanged: local builds
# still download as before. googletest is still built from source with each
# job's own flags (ThreadSanitizer needs every library instrumented), so no
# prebuilt or system gtest is used.
#
# Keep VERSION and SHA256 in step with CMakeLists.txt and with the cache key
# in .github/workflows/ci.yml.
#
# Idempotent: a <dest-dir> that already holds an unpacked tree (a
# CMakeLists.txt) is left as it is.
set -euo pipefail

VERSION=1.15.2
SHA256=7b42b4d6ed48810c5362c265a17faebe90dc2373c885e5216439d37927f02926
URL="https://github.com/google/googletest/releases/download/v${VERSION}/googletest-${VERSION}.tar.gz"

if [ $# -ne 1 ]; then
    echo "usage: $0 <dest-dir>" >&2
    exit 2
fi
dest=$1

if [ -f "$dest/CMakeLists.txt" ]; then
    echo "googletest ${VERSION} already present in $dest"
    exit 0
fi

work=$(mktemp -d)
trap 'rm -rf "$work"' EXIT

echo "downloading googletest ${VERSION}"
curl -fsSL --retry 5 --retry-all-errors --retry-delay 5 \
    -o "$work/googletest.tar.gz" "$URL"

actual=$(sha256sum "$work/googletest.tar.gz" | cut -d' ' -f1)
if [ "$actual" != "$SHA256" ]; then
    echo "error: googletest ${VERSION} tarball SHA-256 mismatch" >&2
    echo "  expected $SHA256" >&2
    echo "  actual   $actual" >&2
    exit 1
fi

# Unpack beside the destination and rename, so an interrupted run never
# leaves a dest that looks complete.
mkdir -p "$(dirname "$dest")"
stage=$(mktemp -d "$(dirname "$dest")/.googletest-stage.XXXXXX")
trap 'rm -rf "$work" "$stage"' EXIT
tar -xzf "$work/googletest.tar.gz" -C "$stage" --strip-components=1
test -f "$stage/CMakeLists.txt"
rm -rf "$dest"
mv "$stage" "$dest"
echo "googletest ${VERSION} unpacked into $dest"

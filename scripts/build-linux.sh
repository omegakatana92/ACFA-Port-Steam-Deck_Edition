#!/usr/bin/env bash
# Build the Linux tree and report the tail of the log. Usage: scripts/build-linux.sh <build-dir> [target]
set -u
cd "$(dirname "$0")/.."
build=${1:-/tmp/acfa-linux}
shift || true
cmake --build "$build" -j"$(nproc)" "$@" > /tmp/acfa-build.log 2>&1
rc=$?
tail -80 /tmp/acfa-build.log
echo "build exit: $rc"
exit $rc

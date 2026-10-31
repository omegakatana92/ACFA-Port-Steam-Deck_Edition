#!/usr/bin/env bash
# Configure the Linux build (from a clean directory by default).
set -u
cd "$(dirname "$0")/.."
build=${1:-/tmp/acfa-linux}
shift || true
cmake -S . -B "$build" -G Ninja -DCMAKE_BUILD_TYPE=Release "$@" > /tmp/acfa-configure.log 2>&1
rc=$?
tail -40 /tmp/acfa-configure.log
echo "configure exit: $rc"
exit $rc

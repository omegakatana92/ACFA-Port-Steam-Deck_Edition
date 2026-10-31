#!/usr/bin/env bash
# Syntax-check runtime translation units on Linux without generated PPU code.
# Usage: scripts/check-linux.sh src/runtime/foo.cpp ...
set -u
cd "$(dirname "$0")/.."
CXX=${CXX:-g++}
flags=(-std=c++20 -Isrc -D_GNU_SOURCE -D_FILE_OFFSET_BITS=64 -DACFA_VERSION=\"0.7.4\" -march=haswell -fsyntax-only -Wall -Wextra -Wno-unused-parameter)
if [ -d /tmp/SDL3-3.4.18/include ]; then
  flags+=(-I/tmp/SDL3-3.4.18/include)
fi
if [ -d /tmp/Vulkan-Headers/include ]; then
  flags+=(-I/tmp/Vulkan-Headers/include)
fi
status=0
for f in "$@"; do
  echo "=== $f"
  out=$("$CXX" "${flags[@]}" "$f" 2>&1)
  rc=$?
  printf '%s\n' "$out" | head -40
  if [ $rc -ne 0 ]; then status=1; fi
done
exit $status

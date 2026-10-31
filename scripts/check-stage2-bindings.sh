#!/usr/bin/env bash
# Assert the descriptor bindings DXC emitted for the Stage 2 shaders.
# Usage: check-stage2-bindings.sh <stage2_vs.spv> <stage2_ps.spv>
set -eu
vs=$1
ps=$2
command -v spirv-dis >/dev/null || { echo "spirv-dis not found"; exit 77; }

vs_dis=$(spirv-dis "$vs")
ps_dis=$(spirv-dis "$ps")

fail=0
need() {
  local text=$1
  local file=$2
  local pattern=$3
  if ! printf '%s\n' "$text" | grep -q -- "$pattern"; then
    echo "missing in $file: $pattern"
    fail=1
  fi
}

need "$vs_dis" "vs" "OpDecorate %vb Binding 1000"
need "$vs_dis" "vs" "OpDecorate %type_ByteAddressBuffer BufferBlock"
need "$vs_dis" "vs" "OpDecorate %Params2 Binding 1"
need "$vs_dis" "vs" "Uniform"
need "$ps_dis" "ps" "OpDecorate %tex Binding 1001"
need "$ps_dis" "ps" "OpTypeImage"
need "$ps_dis" "ps" "OpDecorate %smp Binding 2000"
need "$ps_dis" "ps" "OpTypeSampler"
need "$ps_dis" "ps" "OpDecorate %Params Binding 0"

if [ "$fail" -ne 0 ]; then
  echo "stage 2 binding reflection: FAILED"
  exit 1
fi

printf 'stage 2 binding reflection: OK\n'
printf '  VS: b1=1, ByteAddressBuffer t0=1000\n'
printf '  PS: b0=0, Texture2D t1=1001, SamplerState s0=2000\n'

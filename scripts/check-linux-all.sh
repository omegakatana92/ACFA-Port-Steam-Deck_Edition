#!/usr/bin/env bash
# Syntax-check every Linux runtime translation unit (Windows-only GPU backends excluded).
set -u
cd "$(dirname "$0")/.."
mapfile -t files < <(find src/runtime -name '*.cpp' | grep -v -E 'gpu/(renderer|submit|d3d11_device|window|display|rsx_texture|shader_warm|png|vp_hlsl|fp_hlsl)\.cpp|ui/overlay\.cpp' | sort)
./scripts/check-linux.sh "${files[@]}"

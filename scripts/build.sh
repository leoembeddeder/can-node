#!/usr/bin/env bash
# Configure and build one or more CMake presets (default: debug).
#   scripts/build.sh                 # debug
#   scripts/build.sh release asan    # several
# Presets: debug, asan, coverage, release (see CMakePresets.json).
source "$(dirname "${BASH_SOURCE[0]}")/_common.sh"

mkdir -p "$FCN_BUILD_ROOT"
presets=("$@")
[[ ${#presets[@]} -eq 0 ]] && presets=(debug)

for preset in "${presets[@]}"; do
    echo "==> building preset '$preset' in $FCN_BUILD_ROOT/$preset"
    fcn_build "$preset"
done

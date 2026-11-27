#!/usr/bin/env bash
# Reformat all project C sources in place with clang-format (.clang-format at the repo root).
source "$(dirname "${BASH_SOURCE[0]}")/_common.sh"

mapfile -t files < <(fcn_project_sources)
clang-format -i --style=file "${files[@]}"
echo "formatted ${#files[@]} files"

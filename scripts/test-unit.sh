#!/usr/bin/env bash
# Build a preset and run the CTest suite: Unity unit tests and the platform self-tests.
#   scripts/test-unit.sh           # asan preset: AddressSanitizer + UndefinedBehaviorSanitizer
#   scripts/test-unit.sh debug
# A JUnit report is written to <build>/test-results/.
#
# Every test has to pass on its first run. The one exception is decided inside the test, not
# here: a platform self-test labelled "timing" that misses its timing tolerance because the
# host did not run it well exits with status 75 and is repeated once by its own CTest command
# (tests/platform/run_selftest.sh). A crash, a sanitizer report or a wrong result is never
# repeated.
source "$(dirname "${BASH_SOURCE[0]}")/_common.sh"

preset="${1:-asan}"
dir="$FCN_BUILD_ROOT/$preset"
mkdir -p "$FCN_BUILD_ROOT"
fcn_build "$preset"
mkdir -p "$dir/test-results"

ctest --test-dir "$dir" --output-on-failure --output-junit "$dir/test-results/ctest.xml"

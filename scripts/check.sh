#!/usr/bin/env bash
# The checks that have to pass before a commit, in the order CI runs them.
#   scripts/check.sh
#
# CI (.github/workflows/ci.yml) runs these same scripts and, around them, the steps that only
# make sense on the runner: it checks the executable bits in the git index, creates and
# verifies the SocketCAN interface vcan0, and runs the timing benchmark for the record.
source "$(dirname "${BASH_SOURCE[0]}")/_common.sh"
here="$FCN_ROOT/scripts"

step() {
    echo
    echo "################################################################ $*"
}

step "generated code: app/gen and the test table are what dbc/fcn.dbc gives"
"$here/codegen-check.sh"

step "static analysis"
"$here/static-analysis.sh"

step "build: debug and release, warnings as errors"
"$here/build.sh" debug release

step "unit tests and platform self-tests under ASan + UBSan"
"$here/test-unit.sh" asan

step "unit tests that start threads, under ThreadSanitizer"
"$here/test-tsan.sh"

step "coverage of lib/"
"$here/coverage.sh"

step "system tests"
"$here/test-system.sh"

step "bug log: every entry can be re-created and is still caught"
"$here/buglog-verify.sh"

echo
echo "check: all steps passed"

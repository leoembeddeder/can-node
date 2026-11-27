#!/usr/bin/env bash
# Python tests: unit tests of the tools (tests/python) and system tests (tests/system), which
# start the node and observe it from outside.
#   scripts/test-system.sh                 # all tests
#   scripts/test-system.sh -k smoke -x     # extra arguments go to pytest
# Output in $FCN_BUILD_ROOT/system-test/: JUnit reports and logs-*/<test>/ with the node's log,
# statistics and data files of every test.
#
# Three runs:
#  1. everything not marked "timing", against the release build of the node;
#  2. the same again against the asan build (AddressSanitizer and UndefinedBehaviorSanitizer),
#     so that the application and node_main.c run under the sanitizers too, and the bootloader
#     and the flash tool; a sanitizer report ends the node with a non-zero status or writes to
#     stderr, and either fails the test;
#  3. the tests marked "timing", which assert on the host's wall clock. If they fail, they are
#     repeated once, but only when every failure was a missed timing tolerance
#     (fcn_tools.timing); any other failure, a crash or a wrong value, fails at once.
#
# Exit status: 0 when everything passed; 75 when the only failures were timing tolerances
# missed twice (the host did not keep time, or the timing regressed: repeat on an otherwise
# idle machine to tell); 1 for any other failure.
source "$(dirname "${BASH_SOURCE[0]}")/_common.sh"

# The tools of this checkout, not whatever copy the virtual environment has installed (an
# editable install points at one checkout only).
export PYTHONPATH="$FCN_ROOT/tools${PYTHONPATH:+:$PYTHONPATH}"
python="$(fcn_python)"
if ! tools="$("$python" -c 'import pytest, fcn_tools; print(fcn_tools.__file__)' 2>/dev/null)"; then
    echo "pytest or fcn_tools missing in $FCN_VENV (run scripts/setup-python.sh)" >&2
    exit 1
fi
if [[ "$(realpath "$tools")" != "$(realpath "$FCN_ROOT/tools/fcn_tools/__init__.py")" ]]; then
    echo "fcn_tools is imported from $tools, not from this checkout ($FCN_ROOT/tools)" >&2
    exit 1
fi

mkdir -p "$FCN_BUILD_ROOT"
fcn_build release
fcn_build asan
out="$FCN_BUILD_ROOT/system-test"
mkdir -p "$out"
cd "$FCN_ROOT"

# The pytest command line of one run; {tag} names its JUnit report and its log directory.
pytest_command=("$python" -m pytest tests -o cache_dir="$out/.pytest_cache"
    --junitxml="$out/pytest-{tag}.xml" --artifacts "$out/logs-{tag}")

# run_pytest <tag> <pytest arguments...>; "no tests selected" (exit status 5) is not a failure.
run_pytest() {
    local tag="$1"
    shift
    local placeholder='{tag}'
    local rc=0
    "${pytest_command[@]//"$placeholder"/$tag}" "$@" || rc=$?
    [[ "$rc" -eq 5 ]] && rc=0
    return "$rc"
}

status=0
run_pytest functional -m "not timing" "$@" || status=1

# The bootloader and the flash tool of the asan build too; the images they start are packed
# from the release build of the node (the asan node is larger than a slot, docs/update.md).
FCN_NODE="$FCN_BUILD_ROOT/asan/bin/fcn_node" \
    FCN_BOOT="$FCN_BUILD_ROOT/asan/bin/fcn_boot" \
    FCN_FLASH_TOOL="$FCN_BUILD_ROOT/asan/bin/fcn_flash" \
    ASAN_OPTIONS="detect_stack_use_after_return=1:strict_string_checks=1:detect_leaks=1" \
    UBSAN_OPTIONS="print_stacktrace=1:halt_on_error=1" \
    run_pytest functional-asan -m "not timing" "$@" || status=1

# The decision whether to repeat is made, and tested, in Python (tests/python/test_harness.py).
timing=0
"$python" -m fcn_tools.timing --report "$out/timing-failures.txt" -- \
    "${pytest_command[@]}" -m timing "$@" || timing=$?
if [[ "$timing" -ne 0 ]]; then
    if [[ "$status" -eq 0 && "$timing" -eq 75 ]]; then
        status=75
    else
        status=1
    fi
fi
exit "$status"

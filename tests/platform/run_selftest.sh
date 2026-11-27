#!/bin/sh
# Runs one platform self-test for CTest and decides its verdict (tests/platform/CMakeLists.txt).
#
#   run_selftest.sh [--retry-timing] [--stderr <regex>] [--file <path> <regex>]
#                   <expected exit status> -- <command> [<argument>...]
#
# The test passes if the command exits with the expected status and, where given, its stderr
# and the named file each contain a line that matches the extended regular expression.
#
# --retry-timing: exit status 75 (EX_TEMPFAIL) is what a timing self-test returns when the
# host did not keep time well enough for its tolerance (plat_selftest.c, TIMING_MISSED). Only
# that status is repeated, once. Any other status, a crash, a sanitizer report or a wrong
# result included, fails at once: a retry must never turn a defect into a pass.
#
# stderr is copied to this script's stderr, so CTest shows it with the test's output.
set -u

retry_timing=0
stderr_regex=""
file_path=""
file_regex=""

while [ $# -gt 0 ]; do
    case "$1" in
    --retry-timing)
        retry_timing=1
        shift
        ;;
    --stderr)
        stderr_regex="$2"
        shift 2
        ;;
    --file)
        file_path="$2"
        file_regex="$3"
        shift 3
        ;;
    *)
        break
        ;;
    esac
done

if [ $# -lt 3 ] || [ "$2" != "--" ]; then
    echo "usage: run_selftest.sh [--retry-timing] [--stderr <regex>] [--file <path> <regex>]" \
        "<status> -- <command> [<argument>...]" >&2
    exit 2
fi
expected="$1"
shift 2

TIMING_MISSED=75
err="$(mktemp)" || exit 2
trap 'rm -f "$err"' EXIT

attempt=1
while :; do
    # A file left by an earlier run must not satisfy this one.
    [ -n "$file_path" ] && rm -f "$file_path"
    "$@" 2>"$err"
    status=$?
    cat "$err" >&2
    if [ "$status" -eq "$TIMING_MISSED" ] && [ "$expected" -ne "$TIMING_MISSED" ] &&
        [ "$retry_timing" -eq 1 ] && [ "$attempt" -eq 1 ]; then
        echo "run_selftest: attempt 1 missed its timing tolerance (status 75); one retry" >&2
        attempt=2
        continue
    fi
    break
done

verdict=0
if [ "$status" -ne "$expected" ]; then
    echo "run_selftest: exit status $status, expected $expected (attempt $attempt)" >&2
    verdict=1
fi
if [ -n "$stderr_regex" ] && ! grep -Eq -- "$stderr_regex" "$err"; then
    echo "run_selftest: no line of stderr matches: $stderr_regex" >&2
    verdict=1
fi
if [ -n "$file_path" ] && ! grep -Eq -- "$file_regex" "$file_path"; then
    echo "run_selftest: no line of $file_path matches: $file_regex" >&2
    verdict=1
fi
exit "$verdict"

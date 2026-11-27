#!/usr/bin/env bash
# Line and branch coverage of the portable code under lib/ from the unit tests.
#   scripts/coverage.sh
# Output in <build>/coverage/coverage/: index.html, summary.json, cobertura.xml.
#
# The report is restricted to lib/ on purpose: that is the code that would ship on a
# microcontroller, and the unit tests are the evidence for it. The application and platform
# code are exercised by the platform self-tests and the system tests instead.
source "$(dirname "${BASH_SOURCE[0]}")/_common.sh"

dir="$FCN_BUILD_ROOT/coverage"
out="$dir/coverage"
python="$(fcn_python)"
status=0

# The gate: the unit tests have to reach at least this much of lib/.
min_line=95
min_branch=85

if ! "$python" -m gcovr --version >/dev/null 2>&1; then
    echo "gcovr is not installed in $FCN_VENV (run scripts/setup-python.sh)" >&2
    exit 1
fi

mkdir -p "$FCN_BUILD_ROOT"

# Always from a clean tree: counters (.gcda) accumulate across runs, and notes (.gcno) of a
# source file that was since moved or deleted would make gcov fail on a file that is gone.
if [[ -f "$dir/CMakeCache.txt" ]]; then
    rm -rf "$dir"
fi
fcn_build coverage >"$dir.build.log" 2>&1 || {
    tail -40 "$dir.build.log" >&2
    exit 1
}
ctest --test-dir "$dir" -L unit --output-on-failure

mkdir -p "$out"
# Root and filter are given relative to the repository: gcovr resolves symbolic links in every
# path it compares, so an absolute filter written through a symlink would match nothing.
cd "$FCN_ROOT"
"$python" -m gcovr \
    --root . \
    --filter 'lib/' \
    --object-directory "$dir" \
    --txt "$out/summary.txt" \
    --html-details "$out/index.html" \
    --json-summary "$out/summary.json" --json-summary-pretty \
    --cobertura "$out/cobertura.xml" \
    --print-summary \
    --fail-under-line "$min_line" --fail-under-branch "$min_branch" \
    "$dir" || status=$?
cat "$out/summary.txt"
echo "coverage report: $out/index.html"
if [[ "$status" -ne 0 ]]; then
    echo "coverage of lib/ is below the gate: lines $min_line %, branches $min_branch %" >&2
fi
exit "$status"

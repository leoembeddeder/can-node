#!/usr/bin/env bash
# Static analysis of the project's own C code. Third-party code is never analysed.
#
#   scripts/static-analysis.sh               # everything below
#   scripts/static-analysis.sh --format-only # just the formatting check
#
#   1. clang-format   all project sources match .clang-format
#   2. cppcheck       warning, style, performance and portability checks on all project code
#   3. cppcheck MISRA the MISRA C:2012 addon on lib/ and app/, the code that would ship on a
#                     microcontroller; deviations are listed in config/misra-suppressions.txt
#                     and explained in docs/misra-deviations.md
#   4. clang-tidy     the checks in .clang-tidy and clang's own compiler warnings, as errors
#   5. self-test      a file of planted defects goes through the commands of steps 1 to 4;
#                     each analyser has to report the defect planted for it and to fail
#
# Between steps 1 and 2 the script also checks that every C file under the code directories
# belongs to a build target: a file that is not compiled is not analysed either.
#
# Any finding fails the script. Reports are written to $FCN_BUILD_ROOT/analysis/.
source "$(dirname "${BASH_SOURCE[0]}")/_common.sh"

out="$FCN_BUILD_ROOT/analysis"
mkdir -p "$out"
status=0

# ---------------------------------------------------------------- the analyser commands
# Each analyser is run through one function, used for the project and for the self-test alike.

run_clang_format() { # run_clang_format <log> <file>...
    local log="$1"
    shift
    clang-format --dry-run --Werror --style=file "$@" >"$log" 2>&1
}

cppcheck_common=(
    --std=c11 --platform=unix64
    --inline-suppr
    --suppress=missingIncludeSystem
    "--suppress=*:*/third_party/*"
    --error-exitcode=1
    --quiet
)

# missingInclude is enabled on purpose: cppcheck analyses a file whose project or kernel
# headers it cannot find without a word, and then knows none of their macros and types. With
# it, a compile database that lost its include directories is a finding, not a quiet pass.
run_cppcheck() { # run_cppcheck <compile db dir> <cppcheck build dir> <log>
    mkdir -p "$2"
    cppcheck --project="$1/compile_commands.json" --cppcheck-build-dir="$2" \
        --enable=warning,style,performance,portability,missingInclude \
        "${cppcheck_common[@]}" 2>"$3"
}

run_cppcheck_misra() { # run_cppcheck_misra <compile db dir> <cppcheck build dir> <log>
    mkdir -p "$2"
    # The addon reports through the 'style' severity, so that has to be enabled for it to speak.
    cppcheck --project="$1/compile_commands.json" --cppcheck-build-dir="$2" \
        --addon=misra --enable=style,missingInclude \
        --suppressions-list="$FCN_ROOT/config/misra-suppressions.txt" \
        "${cppcheck_common[@]}" 2>"$3"
}

show() { # show <log> <pattern> <max lines>: matching lines of a report, never a failure itself
    { grep -E "$2" "$1" || true; } | sort -u | head -n "$3" || true
}

units_of() { # units_of <compile db dir>: the source file of every entry, one per line
    python3 -c '
import json, sys
for entry in json.load(open(sys.argv[1])):
    print(entry["file"])
' "$1/compile_commands.json"
}

run_clang_tidy() { # run_clang_tidy <compile db dir> <log>
    local unit rc=0
    : >"$2"
    while IFS= read -r unit; do
        clang-tidy -p "$1" --quiet --extra-arg=-Wno-unknown-warning-option "$unit" \
            >>"$2" 2>&1 || rc=1
    done < <(units_of "$1")
    return "$rc"
}

# ---------------------------------------------------------------- 1. formatting
echo "==> clang-format"
mapfile -t files < <(fcn_project_sources)
if run_clang_format "$out/clang-format.log" "${files[@]}"; then
    echo "    ${#files[@]} files formatted correctly"
else
    show "$out/clang-format.log" 'error|warning' 40
    echo "    formatting differs: run scripts/format.sh"
    status=1
fi
[[ "${1:-}" == "--format-only" ]] && exit "$status"

# The analysers read the compile commands of the debug build, reduced to project sources.
if ! fcn_build debug >"$out/build.log" 2>&1; then
    tail -n 40 "$out/build.log" >&2
    echo "static analysis: the debug build failed, nothing was analysed (see $out/build.log)"
    exit 1
fi
full_db="$FCN_BUILD_ROOT/debug/compile_commands.json"
filter="$FCN_ROOT/scripts/filter_compile_db.py"
db_tidy="$out/db-tidy"   # all project code, as compiled (for clang-tidy)
db_all="$out/db-all"     # all project code, third-party headers visible to cppcheck
db_misra="$out/db-misra" # lib/ and app/ only
python3 "$filter" "$full_db" "$db_tidy" "$FCN_ROOT" "${FCN_CODE_DIRS[@]}"
python3 "$filter" --plain-includes "$full_db" "$db_all" "$FCN_ROOT" "${FCN_CODE_DIRS[@]}"
python3 "$filter" --plain-includes "$full_db" "$db_misra" "$FCN_ROOT" lib app

# A C file that no build target lists is not compiled and so reaches neither the compiler's
# warnings nor any analyser below. "All project code" has to mean every file in the tree.
echo "==> every C file is part of the build"
mapfile -t unlisted < <(comm -23 \
    <(fcn_project_sources | grep '\.c$' | xargs -r -d '\n' realpath | sort -u) \
    <(units_of "$db_all" | xargs -r -d '\n' realpath | sort -u))
if [[ ${#unlisted[@]} -eq 0 ]]; then
    echo "    $(units_of "$db_all" | wc -l) translation units, none left out"
else
    printf '    in no build target: %s\n' "${unlisted[@]}"
    status=1
fi

# ---------------------------------------------------------------- 2. cppcheck
echo "==> cppcheck (all project code)"
if run_cppcheck "$db_all" "$out/cppcheck-build-all" "$out/cppcheck.log"; then
    echo "    no findings"
else
    cat "$out/cppcheck.log"
    status=1
fi

# ---------------------------------------------------------------- 3. cppcheck MISRA
echo "==> cppcheck MISRA C:2012 addon (lib/ and app/)"
if run_cppcheck_misra "$db_misra" "$out/cppcheck-build-misra" "$out/cppcheck-misra.log"; then
    echo "    no findings outside the documented deviations"
else
    cat "$out/cppcheck-misra.log"
    echo "    $(grep -c 'misra-c2012' "$out/cppcheck-misra.log" || true) MISRA finding(s)"
    status=1
fi

# ---------------------------------------------------------------- 4. clang-tidy
echo "==> clang-tidy"
if run_clang_tidy "$db_tidy" "$out/clang-tidy.log"; then
    echo "    $(units_of "$db_tidy" | wc -l) translation units clean"
else
    show "$out/clang-tidy.log" 'warning:|error:|ERROR' 80
    echo "    clang-tidy failed (full report: $out/clang-tidy.log)"
    status=1
fi

# ---------------------------------------------------------------- 5. the analysers' own test
# "No findings" only means something if the same commands do find a defect that is known to
# be there. scripts/selftest/planted_violations.c holds one for each analyser. It gets the
# compile flags of a module of lib/ and goes through the functions used above, so a check
# that was switched off or suppressed for the project is missed here as well. Each analyser
# has to do both things the steps above rely on: name the planted defect in its report, and
# end with a failing exit status, which is what makes this script fail.
echo "==> analyser self-test (planted violations)"
planted="$FCN_ROOT/scripts/selftest/planted_violations.c"
python3 "$filter" --retarget "$planted" "$full_db" "$out/db-selftest-tidy" "$FCN_ROOT" lib
python3 "$filter" --plain-includes --retarget "$planted" "$full_db" "$out/db-selftest" \
    "$FCN_ROOT" lib

expect_failure() { # <tool name> <command...>: the command has to exit with a failure
    local tool="$1"
    shift
    if "$@"; then
        echo "    SELF-TEST FAILED: $tool exits 0 on the planted file, so findings would not fail this script"
        status=1
    fi
}
expect_failure "cppcheck" \
    run_cppcheck "$out/db-selftest" "$out/cppcheck-build-selftest" "$out/selftest-cppcheck.log"
expect_failure "the cppcheck MISRA addon" \
    run_cppcheck_misra "$out/db-selftest" "$out/cppcheck-build-selftest-misra" \
    "$out/selftest-cppcheck-misra.log"
expect_failure "clang-tidy" \
    run_clang_tidy "$out/db-selftest-tidy" "$out/selftest-clang-tidy.log"
run_clang_format "$out/selftest-clang-format.log" "$planted" && format_caught=0 || format_caught=1

expect_finding() { # <log file> <finding id> <tool name>
    if grep -q "$2" "$1"; then
        echo "    $3 reports $2"
    else
        echo "    SELF-TEST FAILED: $3 did not report the planted $2 (see $1)"
        status=1
    fi
}
expect_finding "$out/selftest-cppcheck.log" "arrayIndexOutOfBounds" "cppcheck"
expect_finding "$out/selftest-cppcheck.log" "duplicateExpression" "cppcheck"
expect_finding "$out/selftest-cppcheck-misra.log" "misra-c2012-15.6" "cppcheck MISRA addon"
expect_finding "$out/selftest-clang-tidy.log" "clang-analyzer-core.NullDereference" "clang-tidy"
if [[ "$format_caught" -eq 1 ]]; then
    echo "    clang-format reports the unbraced if"
else
    echo "    SELF-TEST FAILED: clang-format accepted the planted formatting defect"
    status=1
fi

if [[ "$status" -eq 0 ]]; then
    echo "static analysis: clean"
else
    echo "static analysis: FINDINGS (reports in $out)"
fi
exit "$status"

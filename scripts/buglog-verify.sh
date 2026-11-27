#!/usr/bin/env bash
# Prove that every entry of the bug log is real and still detected.
#   scripts/buglog-verify.sh
#
# Each directory docs/buglog/<id>/ holds
#   reintroduce.patch   puts the bug back into the current sources
#   test*.sh            one or more tests that detect it: the test that caught the bug and,
#                       where one was added with the fix, the regression test
#                       (exit status 0 = the test passes)
#   build-fails         optional, empty: the re-created bug shows as a build failure
#
# For each entry this script works on a scratch copy of the sources and requires that
#   1. every test PASSES without the patch (a test that fails anyway proves nothing),
#   2. the patch applies and the sources still build (unless the entry has build-fails),
#   3. every test FAILS with the patch.
# Anything else, a build that breaks for another reason included, fails the script.
#
# A test whose verdict depends on the host keeping time exits with status 75 when it missed
# only timing tolerances, twice (as scripts/test-system.sh does). Such a run neither passes nor
# fails: the entry is reported as inconclusive, with or without the patch.
#
# Exit status: 0 when every entry was verified; 75 when the only problems were inconclusive
# entries (repeat on an otherwise idle machine); 1 for anything else.
#
# A test script is started in the scratch tree with
#   BUILD            build directory of the release preset of the scratch tree
#   FCN_PYTHON       the project's Python interpreter
#   FCN_BUILD_ROOT   set so that Python tests and tools use the scratch build
#   PYTHONPATH       set so that they import the scratch tree's fcn_tools
source "$(dirname "${BASH_SOURCE[0]}")/_common.sh"

work="$FCN_BUILD_ROOT/buglog-verify"
src="$work/src"
build="$work/release"
python="$(fcn_python)"
rm -rf "$work"
mkdir -p "$src"

# Scratch copy of everything that is source. The list names what is left out, so a directory
# added to the project later is copied without this script having to know about it.
# (Names starting with a dot, such as .git and .venv, are not matched by the pattern.)
for item in "$FCN_ROOT"/*; do
    case "$(basename "$item")" in
    build | third_party | results | docs) continue ;;
    *) cp -a "$item" "$src/" ;;
    esac
done
ln -s "$FCN_ROOT/third_party" "$src/third_party"

shopt -s nullglob
entries=("$FCN_ROOT"/docs/buglog/*/)
if [[ ${#entries[@]} -eq 0 ]]; then
    echo "bug log: no entries"
    exit 0
fi

build_scratch() { # build_scratch <log file>
    { cmake --preset release -S "$src" -B "$build" && cmake --build "$build"; } >>"$1" 2>&1
}

run_test() { # run_test <test script> <log file>; returns the test's exit status
    echo "---- $(basename "$1")" >>"$2"
    (cd "$src" && BUILD="$build" FCN_BUILD_ROOT="$work" FCN_PYTHON="$python" \
        PYTHONPATH="$src/tools" bash "$1") >>"$2" 2>&1
}

if ! build_scratch "$work/build.log"; then
    echo "bug log: the scratch copy of the sources does not build (see $work/build.log)"
    exit 1
fi

status=0
inconclusive=0
for entry in "${entries[@]}"; do
    id="$(basename "$entry")"
    log="$work/$id.log"
    tests=("$entry"test*.sh)
    : >"$log"

    if [[ ! -f "$entry/reintroduce.patch" || ${#tests[@]} -eq 0 ]]; then
        echo "$id: incomplete entry (needs reintroduce.patch and at least one test*.sh)"
        status=1
        continue
    fi

    # 1. The control: without the bug every test passes.
    control=0
    echo "==== without the patch" >>"$log"
    for t in "${tests[@]}"; do
        rc=0
        run_test "$t" "$log" || rc=$?
        if [[ "$rc" -eq 75 ]]; then
            echo "$id: inconclusive: $(basename "$t") missed only timing tolerances WITHOUT the bug (see $log)"
            [[ "$control" -eq 0 ]] && control=75
        elif [[ "$rc" -ne 0 ]]; then
            echo "$id: $(basename "$t") fails WITHOUT the bug, so it cannot show the bug (see $log)"
            control=1
        fi
    done
    if [[ "$control" -eq 1 ]]; then
        status=1
        continue
    elif [[ "$control" -eq 75 ]]; then
        inconclusive=1
        continue
    fi

    # 2. The bug goes back in and the sources still build.
    if ! patch -p1 -s --dry-run -d "$src" <"$entry/reintroduce.patch" >>"$log" 2>&1; then
        echo "$id: reintroduce.patch no longer applies to the sources (see $log)"
        status=1
        continue
    fi
    patch -p1 -s -d "$src" <"$entry/reintroduce.patch"
    echo "==== with the patch" >>"$log"

    if ! build_scratch "$log"; then
        if [[ -f "$entry/build-fails" ]]; then
            echo "$id: caught (the build fails with the bug re-created, as the entry says)"
        else
            echo "$id: the sources do not build with reintroduce.patch applied (see $log)"
            status=1
        fi
    elif [[ -f "$entry/build-fails" ]]; then
        echo "$id: NOT caught: the entry expects a build failure, but the sources built"
        status=1
    else
        # 3. With the bug every test fails.
        for t in "${tests[@]}"; do
            rc=0
            run_test "$t" "$log" || rc=$?
            if [[ "$rc" -eq 0 ]]; then
                echo "$id: NOT caught: $(basename "$t") passes with the bug re-created (see $log)"
                status=1
            elif [[ "$rc" -eq 75 ]]; then
                echo "$id: inconclusive: $(basename "$t") missed only timing tolerances with the bug re-created (see $log)"
                inconclusive=1
            else
                echo "$id: caught by $(basename "$t")"
            fi
        done
    fi

    patch -R -p1 -s -d "$src" <"$entry/reintroduce.patch"
    if ! build_scratch "$work/build.log"; then
        echo "bug log: the scratch copy no longer builds after removing the patch of $id"
        exit 1
    fi
done
if [[ "$status" -eq 0 && "$inconclusive" -ne 0 ]]; then
    status=75
fi
exit "$status"

# Writes the build id of the source tree into a C file. Run as a script at BUILD time (every
# build, not only when CMake configures), so that a binary built after a commit or an edit
# carries the tree it was built from even if nothing was re-configured:
#   cmake -DSOURCE_DIR=<repository> -DOUTPUT=<file.c> -P cmake/FcnBuildId.cmake
#
# The id is the commit. When tracked files differ from it, "-dirty-" and the first 12 hex
# digits of the git blob hash of the tree's changes follow: the output of
#   git diff HEAD --binary --no-color --no-ext-diff --no-textconv --no-renames
#            --submodule=diff --ignore-submodules=untracked -- . ':(exclude)results'
# piped into `git hash-object --stdin`. Two different sets of uncommitted changes therefore
# give two different ids, and a binary built from one dirty tree is told apart from a binary
# built from another. "Dirty" itself is decided exactly as tools/fcn_tools/results.py decides
# it for the provenance of a result: `git status --porcelain --untracked-files=no
# --ignore-submodules=untracked`, without results/ (which every measurement rewrites).
# tools/fcn_tools/results.py computes the same id for the tree a result records
# (tree_build_id(); tests/python/test_results.py runs this script against it), and every bench
# (bench/) refuses to write a result measured with a binary that carries another one. The host
# programs report the id (platform/posix/build_id.h): the node in its statistics, --version and
# log, fcn_boot in --version and its log, fcn_flash in --version, plat_selftest in its reports.
# The submodules count (third_party/: the FreeRTOS kernel, whose POSIX port the node runs on,
# and Unity): one checked out at another commit than the tree records, or with a tracked file
# changed in it, makes the tree dirty, and --submodule=diff puts the submodule's own diff, from
# the recorded commit to its work tree, into the hash. In that diff a changed binary file is
# named with its abbreviated blob hash instead of given whole (git passes --binary on to no
# submodule); the build compiles no binary file.
# Untracked files are not part of the id, in the tree or in a submodule: the build compiles only
# files that tracked CMakeLists.txt files name, so a new source file in the build means a
# modified tracked file. Without git, or outside a repository, the id is "unknown". A clean
# tree, submodules included, has the commit alone as its id. Looking into the submodules costs
# every build a scan of their files: several seconds on the 9p mount of a Windows drive under
# WSL, a few milliseconds on a Linux file system.
#
# The file is only rewritten when the id changed, so an unchanged tree does not relink.

if(NOT DEFINED SOURCE_DIR OR NOT DEFINED OUTPUT)
    message(FATAL_ERROR "usage: cmake -DSOURCE_DIR=<dir> -DOUTPUT=<file> -P FcnBuildId.cmake")
endif()

set(_id "unknown")
find_program(_git git)
if(_git)
    execute_process(COMMAND "${_git}" -C "${SOURCE_DIR}" rev-parse HEAD
                    OUTPUT_VARIABLE _commit RESULT_VARIABLE _rc
                    OUTPUT_STRIP_TRAILING_WHITESPACE ERROR_QUIET)
    if(_rc EQUAL 0 AND _commit MATCHES "^[0-9a-f]+$")
        execute_process(COMMAND "${_git}" -C "${SOURCE_DIR}" --no-optional-locks status
                                --porcelain --untracked-files=no
                                --ignore-submodules=untracked
                        OUTPUT_VARIABLE _status RESULT_VARIABLE _rc ERROR_QUIET)
        set(_id "${_commit}")
        set(_dirty FALSE)
        if(NOT _rc EQUAL 0)
            set(_id "unknown")
        else()
            # One line per file: two status characters, a space, the path.
            string(REPLACE "\n" ";" _lines "${_status}")
            foreach(_line IN LISTS _lines)
                string(LENGTH "${_line}" _len)
                if(_len GREATER 3)
                    string(SUBSTRING "${_line}" 3 -1 _path)
                    if(NOT _path MATCHES "^results/")
                        set(_dirty TRUE)
                    endif()
                endif()
            endforeach()
        endif()
        if(_dirty)
            # The two commands run as a pipe; RESULTS_VARIABLE holds the status of each.
            execute_process(COMMAND "${_git}" -C "${SOURCE_DIR}" --no-optional-locks diff HEAD
                                    --binary --no-color --no-ext-diff --no-textconv
                                    --no-renames --submodule=diff
                                    --ignore-submodules=untracked -- . ":(exclude)results"
                            COMMAND "${_git}" -C "${SOURCE_DIR}" hash-object --stdin
                            OUTPUT_VARIABLE _hash RESULTS_VARIABLE _rcs
                            OUTPUT_STRIP_TRAILING_WHITESPACE ERROR_QUIET)
            if(_rcs STREQUAL "0;0" AND _hash MATCHES "^[0-9a-f]+$")
                string(SUBSTRING "${_hash}" 0 12 _hash)
                set(_id "${_commit}-dirty-${_hash}")
            else()
                set(_id "unknown")
            endif()
        endif()
    endif()
endif()

set(_content "/* Generated by cmake/FcnBuildId.cmake at build time. Do not edit. */
#include \"build_id.h\"

const char fcn_build_id[] = \"${_id}\";
")

set(_old "")
if(EXISTS "${OUTPUT}")
    file(READ "${OUTPUT}" _old)
endif()
if(NOT _old STREQUAL _content)
    file(WRITE "${OUTPUT}" "${_content}")
endif()

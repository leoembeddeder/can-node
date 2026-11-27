#!/usr/bin/env bash
# The regression test of the programs themselves: the system tests of the ends of fcn_flash,
# fcn_boot, the node and the crash payload, run as their held variants (tests/tools/exit_held.c),
# whose end keeps the program's memory map, and with it the open file descriptions of the flash
# and the cell, alive until the test lets it go. The next program (fcn_flash info, fcn_boot),
# started at once, finds a file "in use" unless the program that ended released its locks itself.
# The three tests of the ends that close the files in order ("in_order") pass with and without
# the bug and are left out.
out="$("$FCN_PYTHON" -m pytest tests/system/test_held_ends.py -q -p no:cacheprovider -rs \
    -k "not in_order" --artifacts "$FCN_BUILD_ROOT/system-test-logs" 2>&1)"
status=$?
echo "$out"
if [ "$status" -eq 0 ] && ! grep -q '8 passed' <<<"$out"; then
    echo "the tests did not all run here, so they cannot show the bug"
    exit 1
fi
exit "$status"

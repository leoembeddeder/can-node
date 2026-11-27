#!/usr/bin/env bash
# The regression test of the bootloader itself: the system test that starts the node through
# fcn_boot_held (tests/tools/boot_held.c), fcn_boot whose exec keeps its old memory map, and with
# it the open file descriptions of the flash and the cell, alive until the image has ended. The
# node then finds a file "in use" unless fcn_boot released its locks before the exec.
out="$("$FCN_PYTHON" -m pytest tests/system/test_boot.py -q -p no:cacheprovider -rs \
    -k test_the_image_locks_both_files_while_the_bootloaders_old_memory_map_lives_on \
    --artifacts "$FCN_BUILD_ROOT/system-test-logs" 2>&1)"
status=$?
echo "$out"
if [ "$status" -eq 0 ] && ! grep -q '1 passed' <<<"$out"; then
    echo "the test did not run here, so it cannot show the bug"
    exit 1
fi
exit "$status"

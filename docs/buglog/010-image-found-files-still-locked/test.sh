#!/usr/bin/env bash
# The regression test added with the fix: the platform test of the retained cell. Its tests of
# the lock fork a process that tries to lock the cell's file while the cell is still open and
# mapped, so that its open file description exists, as after an exec whose old memory map
# another process holds. It fails if the bootloader's release (posix_retained_unlock()) leaves
# the lock in place, or if the lock it takes back after a failed exec is not refused while
# another process holds the cell.
"$BUILD/tests/platform/test_posix_retained"

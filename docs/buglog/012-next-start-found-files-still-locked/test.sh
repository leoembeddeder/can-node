#!/usr/bin/env bash
# The regression test added with the fix for the retained cell: the platform test of the cell.
# Its test of the close forks a process while the cell is mapped, which keeps a mapping of the
# cell, and with it the cell's open file description, alive after the close: the state an end
# of the process leaves while another process holds its memory map. It fails if the close leaves
# the lock to the end of that description.
"$BUILD/tests/platform/test_posix_retained"

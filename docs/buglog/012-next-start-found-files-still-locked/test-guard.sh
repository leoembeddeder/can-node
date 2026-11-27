#!/usr/bin/env bash
# The regression test of the one end in the entry's list that nothing outside a program can
# provoke: the shutdown guard's (status 70). A child of the flash self-test opens a retained
# cell and the flash, leaves a keeper of both mappings behind (tests/platform/map_keeper.h) and
# has a task program the flash under a scheduler that never stops; the guard, armed for 50 ms,
# ends the run. The parent then opens the cell and the flash at once: both are "in use" unless
# the guard's end released the locks itself, and with the lock left in place the mapping is
# still the shared one, so that the programs the task makes while the end is held open reach the
# file (the child's status 81).
"$BUILD/tests/platform/flash_selftest" end-while-writing guard \
    "$BUILD/buglog-012-guard-flash.bin" "$BUILD/buglog-012-guard.cell"

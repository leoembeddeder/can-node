#!/usr/bin/env bash
# The regression test of plat_reset() itself: the self-test of the retained cell across real
# resets of a FreeRTOS program. Each child resets from a task (a requested reset, an assertion,
# a stack overflow, a power loss, a failed allocation, an image fault) or ends in order, and
# leaves a keeper of its mapping of the cell behind (tests/platform/map_keeper.h), so that the
# cell's open file description outlives it. The next child opens the cell at once: it ends with
# "in use by another node" unless the run before it released the cell's lock itself.
"$BUILD/tests/platform/wdg_selftest" retained "$BUILD/buglog-012-retained-cell.bin"

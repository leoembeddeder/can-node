#!/usr/bin/env bash
# A task keeps the scheduler suspended for 20 ms of busy work. The self-test fails if the tick
# count does not advance by the time that passed, during the suspension or in the second after
# it. Status 75 means the host dropped more ticks than the test tolerates (a timing miss, not
# the bug, which fails with status 1): only that is repeated, once, as CTest does.
"$BUILD/tests/platform/plat_selftest" suspend 20
status=$?
if [ "$status" -eq 75 ]; then
    "$BUILD/tests/platform/plat_selftest" suspend 20
    status=$?
fi
exit "$status"

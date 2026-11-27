#!/usr/bin/env bash
# The same on the real port: plat_selftest tick-late 50 leaves a tick pending on a task's
# thread that is switched out for 3.5 ms, so that the locked tick makes it up, and then watches
# the tick count after the late tick is taken. It fails (status 1) if two ticks in a row are
# reached before they were due. Status 75 means the host let too few rounds make the late tick
# up (a timing miss, not the bug): only that is repeated, once, as CTest does.
"$BUILD/tests/platform/plat_selftest" tick-late 50
status=$?
if [ "$status" -eq 75 ]; then
    "$BUILD/tests/platform/plat_selftest" tick-late 50
    status=$?
fi
exit "$status"
